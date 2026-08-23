#include "agent.h"

#include <stddef.h>


#define CTX_SLOT_BYTES 48

#define CTX_FIT_MIN_BYTES 2048.0

#define CTX_SLOPE_MIN     0.05
#define CTX_SLOPE_MAX     2.0
#define CTX_SLOPE_DEFAULT 0.25

#define CTX_IMAGE_PIXELS 750.0
#define CTX_IMAGE_TOKENS 1600.0

void ctx_init(CtxGauge *g) {
    if (!g) return;
    *g = (CtxGauge){0};
    g->slope = CTX_SLOPE_DEFAULT;
    g->exact_slots = SIZE_MAX;
}


/* What slot `i` costs a request whose recent window begins at `recent`: a
 * tool result the writer elides for age costs its note instead of its
 * bytes, or a fit would be made from bytes no request ever sent. */
static f64 ctx_slot_bytes(const Conv *c, size_t i, size_t recent) {
    size_t text = conv_result_elided(c, i, recent) ? AGENT_ELIDE_NOTE_BYTES
                                                   : c->text[i].n;
    size_t b = text + c->tool_name[i].n + c->tool_call_id[i].n
               + c->shell_out[i].n + c->anthropic_thinking[i].n;
    return (f64)b + (f64)CTX_SLOT_BYTES;
}

static f64 ctx_bytes(const Conv *c) {
    size_t recent = conv_elide_start(c);
    f64 total = 0;
    for (size_t i = 0; i < c->n; i++) total += ctx_slot_bytes(c, i, recent);
    return total;
}


static f64 ctx_slot_media(const Conv *c, size_t i) {
    const MediaSet *m = c->media;
    if (!m) return 0;
    f64 total = 0;
    size_t off = c->media_off[i], n = c->media_n[i];
    for (size_t k = 0; k < n; k++) {
        size_t id = off + k;
        // An entry a resumed session lost never reaches the wire.
        if (!media_live(m, id)) continue;
        f64 t = CTX_IMAGE_TOKENS;
        if (m->w[id] && m->h[id]) {
            t = (f64)m->w[id] * (f64)m->h[id] / CTX_IMAGE_PIXELS;
            if (t > CTX_IMAGE_TOKENS) t = CTX_IMAGE_TOKENS;
        }
        total += t;
    }
    return total;
}

static f64 ctx_media_tokens(const Conv *c) {
    f64 total = 0;
    for (size_t i = 0; i < c->n; i++) total += ctx_slot_media(c, i);
    return total;
}

void ctx_note_usage(CtxGauge *g, const Conv *c, size_t prompt_tokens) {
    if (!g || !c || !prompt_tokens) return;
    f64 bytes = ctx_bytes(c);
    f64 media = ctx_media_tokens(c);
    f64 text = (f64)prompt_tokens - media;
    if (text < 0) text = 0;

    if (g->basis && bytes - g->fit_bytes >= CTX_FIT_MIN_BYTES
        && text > g->fit_text) {
        f64 slope = (text - g->fit_text) / (bytes - g->fit_bytes);
        if (slope >= CTX_SLOPE_MIN && slope <= CTX_SLOPE_MAX) g->slope = slope;
    }
    g->offset = text - g->slope * bytes;
    g->fit_tokens = prompt_tokens;
    g->fit_text = text;
    g->fit_media = media;
    g->fit_bytes = bytes;
    g->exact_slots = c->n;
    g->measured = true;
    g->basis = true;
}

void ctx_model_changed(CtxGauge *g) {
    if (!g) return;

    g->exact_slots = SIZE_MAX;
    g->window = 0;
    g->basis = false;
}

void ctx_set_window(CtxGauge *g, size_t window) {
    if (!g) return;
    g->window = window <= AGENT_MAX_CONTEXT_WINDOW ? window : 0;
}

void ctx_set_tools(CtxGauge *g, const ToolRegistry *tools) {
    if (!g) return;
    g->tools = tools;
}

static b8 ctx_view(const CtxGauge *g, const Conv *c, size_t *tokens,
                   b8 *exact) {
    if (!g || !c) return false;
    f64 bytes = ctx_bytes(c);
    f64 media = ctx_media_tokens(c);

    if (!g->measured) {
        f64 v = CTX_SLOPE_DEFAULT * (bytes + (f64)tools_schema_bytes(g->tools))
                + media;
        if (v > (f64)AGENT_MAX_CONTEXT_WINDOW)
            v = (f64)AGENT_MAX_CONTEXT_WINDOW;
        *tokens = (size_t)v;
        *exact = false;
        return true;
    }

    if (c->n == g->exact_slots && bytes == g->fit_bytes
        && media == g->fit_media) {
        *tokens = g->fit_tokens;
        *exact = true;
        return true;
    }
    f64 v = g->offset + g->slope * bytes + media;

    if (bytes > g->fit_bytes && media >= g->fit_media && v < (f64)g->fit_tokens)
        v = (f64)g->fit_tokens;
    if (!(v > 0)) v = 0;
    if (v > (f64)AGENT_MAX_CONTEXT_WINDOW) v = (f64)AGENT_MAX_CONTEXT_WINDOW;
    *tokens = (size_t)v;
    *exact = false;
    return true;
}

void ctx_sync(const CtxGauge *g, const Conv *c) {
    size_t tokens = 0;
    b8 exact = false;
    b8 known = ctx_view(g, c, &tokens, &exact);
    tui_set_context(tokens, known, exact, g ? g->window : 0);
}

b8 ctx_over(const CtxGauge *g, const Conv *c, u32 percent) {
    if (!g || !g->window || !percent) return false;
    size_t tokens = 0;
    b8 exact = false;
    if (!ctx_view(g, c, &tokens, &exact)) return false;
    f64 share = (f64)g->window * (f64)percent / 100.0;
    /* A window smaller than the reserve has no room to hold back, so the
     * percentage is all there is to go on. */
    f64 limit = g->window > AGENT_COMPACT_RESERVE
                    ? (f64)(g->window - AGENT_COMPACT_RESERVE)
                    : share;
    if (share < limit) limit = share;
    return (f64)tokens >= limit;
}

/* What slots [from, to) cost in this model's tokens, through the same fit
 * the gauge reports. Callers start at slot 1: slot 0 is the system prompt,
 * which the offset already pays for. */
static f64 ctx_conv_tokens(const CtxGauge *g, const Conv *c, size_t from,
                           size_t to, size_t recent) {
    f64 slope = g->slope > 0 ? g->slope : CTX_SLOPE_DEFAULT;
    f64 sum = 0;
    for (size_t i = from; i < to; i++)
        sum += slope * ctx_slot_bytes(c, i, recent) + ctx_slot_media(c, i);
    return sum;
}

size_t ctx_compact_split(const CtxGauge *g, const Conv *c) {
    if (!g || !c || c->n < 3) return 0;
    size_t recent = conv_elide_start(c);
    /* The tail is measured in tokens, but only the conversation's own: the
     * offset pays for the system prompt and the schemas, which a compaction
     * cannot shorten and so must not be charged against what it keeps.
     *
     * A conversation that fits the window's share whole would leave nothing
     * to summarize, and a checkpoint standing for nothing costs a request to
     * lose the tail it was meant to protect. Cap the tail at a share of the
     * conversation so a cut always has both sides. */
    f64 budget = ctx_conv_tokens(g, c, 1, c->n, recent)
                 * (f64)(100 - AGENT_COMPACT_HEAD_PCT) / 100.0;
    /* No window, no share of one to spend: the conversation's own size is the
     * only budget left, and it still cuts a tail worth keeping. A window only
     * ever tightens that. */
    if (g->window) {
        f64 share = (f64)g->window * (f64)AGENT_COMPACT_KEEP_PCT / 100.0;
        if (share < budget) budget = share;
    }

    /* A head is worth a checkpoint only once it holds work that was done.
     * Below the oldest assistant message there is a question and nothing
     * else, and summarizing a question whose answer is kept verbatim buys
     * no room. */
    size_t done = 0;
    for (size_t i = 1; i < c->n && !done; i++)
        if (c->role[i] == M_ASSISTANT) done = i;
    if (!done) return 0;

    f64 total = 0;
    size_t keep = 0, newest = 0;
    for (size_t i = c->n; i-- > 1;) {
        total += ctx_conv_tokens(g, c, i, i + 1, recent);
        if (i <= done || !conv_round_start(c, i)) continue;
        /* The round in flight, whatever it costs: it is the work the turn is
         * doing, and the tail is never emptier than that. */
        if (!newest) newest = i;
        /* Monotone: an older boundary carries everything a newer one does. */
        if (total <= budget) keep = i;
    }
    if (!keep) keep = newest;
    return keep >= 2 ? keep : 0;
}

b8 ctx_compact_worth(const CtxGauge *g, const Conv *c, size_t keep) {
    if (!g || !c || !g->window || keep < 2 || keep > c->n) return false;
    f64 budget = (f64)g->window * (f64)AGENT_COMPACT_KEEP_PCT / 100.0;
    return ctx_conv_tokens(g, c, 1, keep, conv_elide_start(c)) >= budget;
}
