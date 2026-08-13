#include "agent.h"

#include <stddef.h>

/* The JSON envelope a message is sent in: role, keys, quotes and commas. */
#define CTX_SLOT_BYTES     48
/* Below this the growth between two requests is too small to divide by. */
#define CTX_FIT_MIN_BYTES  2048.0
/* A token is a handful of bytes in every tokenizer anyone serves; a ratio
 * outside this came from something other than the conversation growing. */
#define CTX_SLOPE_MIN      0.05
#define CTX_SLOPE_MAX      2.0
#define CTX_SLOPE_DEFAULT  0.25

void ctx_init(CtxGauge *g) {
    if (!g) return;
    *g = (CtxGauge){0};
    g->slope = CTX_SLOPE_DEFAULT;
    g->exact_slots = SIZE_MAX;
}

/* What the conversation contributes to a request. Every field a request body
 * carries is counted, including the shell output a '!' run attached, since
 * that is replayed too. */
static f64 ctx_bytes(const Conv *c) {
    f64 total = 0;
    for (size_t i = 0; i < c->n; i++) {
        size_t b = c->text[i].n + c->tool_name[i].n + c->tool_call_id[i].n
                 + c->shell_out[i].n + c->anthropic_thinking[i].n;
        total += (f64)b + (f64)CTX_SLOT_BYTES;
    }
    return total;
}

void ctx_note_usage(CtxGauge *g, const Conv *c, size_t prompt_tokens) {
    if (!g || !c || !prompt_tokens) return;
    f64 bytes = ctx_bytes(c);
    /* A conversation that shrank, or grew by too little to measure, says
     * nothing about the slope, and neither does a request whose prompt came
     * out no larger than the last one. Those guards are also what keeps a
     * dropped conversation, a rewind or a compaction from fitting a slope to
     * a difference that is not growth. */
    if (g->basis && bytes - g->fit_bytes >= CTX_FIT_MIN_BYTES
        && prompt_tokens > g->fit_tokens) {
        f64 slope = (f64)(prompt_tokens - g->fit_tokens)
                  / (bytes - g->fit_bytes);
        if (slope >= CTX_SLOPE_MIN && slope <= CTX_SLOPE_MAX) g->slope = slope;
    }
    g->offset      = (f64)prompt_tokens - g->slope * bytes;
    g->fit_tokens  = prompt_tokens;
    g->fit_bytes   = bytes;
    g->exact_slots = c->n;
    g->measured    = true;
    g->basis       = true;
}

void ctx_model_changed(CtxGauge *g) {
    if (!g) return;
    /* The fit stays: a byte is worth about as much in one tokenizer as in
     * another, and an estimate carried across is far closer than the
     * previous model's exact count would be. Exactness and the window are
     * the model's own, and go with it. */
    g->exact_slots = SIZE_MAX;
    g->window      = 0;
    g->basis       = false;
}

void ctx_set_window(CtxGauge *g, size_t window) {
    if (!g) return;
    g->window = window <= AGENT_MAX_CONTEXT_WINDOW ? window : 0;
}

static b8 ctx_view(const CtxGauge *g, const Conv *c, size_t *tokens,
                   b8 *exact) {
    if (!g || !c || !g->measured) return false;
    f64 bytes = ctx_bytes(c);
    /* The same slots holding the same bytes are the conversation that was
     * measured. A compaction or a rewind can land on the slot count the
     * measurement covered while holding something else entirely, which is
     * why the count alone does not decide this. */
    if (c->n == g->exact_slots && bytes == g->fit_bytes) {
        *tokens = g->fit_tokens;
        *exact  = true;
        return true;
    }
    f64 v = g->offset + g->slope * bytes;
    /* Appending to a measured conversation cannot make the next request
     * smaller than the one that was measured. A conversation that lost bytes
     * is a rewind or a compaction, where it can. */
    if (bytes > g->fit_bytes && v < (f64)g->fit_tokens) v = (f64)g->fit_tokens;
    if (!(v > 0)) v = 0;
    if (v > (f64)AGENT_MAX_CONTEXT_WINDOW) v = (f64)AGENT_MAX_CONTEXT_WINDOW;
    *tokens = (size_t)v;
    *exact  = false;
    return true;
}

void ctx_sync(const CtxGauge *g, const Conv *c) {
    size_t tokens = 0;
    b8 exact = false;
    b8 known = ctx_view(g, c, &tokens, &exact);
    tui_set_context(tokens, known, exact, g ? g->window : 0);
}
