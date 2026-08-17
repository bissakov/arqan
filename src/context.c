#include "agent.h"

#include <stddef.h>


#define CTX_SLOT_BYTES     48

#define CTX_FIT_MIN_BYTES  2048.0

#define CTX_SLOPE_MIN      0.05
#define CTX_SLOPE_MAX      2.0
#define CTX_SLOPE_DEFAULT  0.25

#define CTX_IMAGE_PIXELS   750.0
#define CTX_IMAGE_TOKENS   1600.0

void ctx_init(CtxGauge *g) {
    if (!g) return;
    *g = (CtxGauge){0};
    g->slope = CTX_SLOPE_DEFAULT;
    g->exact_slots = SIZE_MAX;
}


static f64 ctx_bytes(const Conv *c) {
    f64 total = 0;
    for (size_t i = 0; i < c->n; i++) {
        size_t b = c->text[i].n + c->tool_name[i].n + c->tool_call_id[i].n
                 + c->shell_out[i].n + c->anthropic_thinking[i].n;
        total += (f64)b + (f64)CTX_SLOT_BYTES;
    }
    return total;
}


static f64 ctx_media_tokens(const Conv *c) {
    const MediaSet *m = c->media;
    if (!m) return 0;
    f64 total = 0;
    for (size_t i = 0; i < c->n; i++) {
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
    }
    return total;
}

void ctx_note_usage(CtxGauge *g, const Conv *c, size_t prompt_tokens) {
    if (!g || !c || !prompt_tokens) return;
    f64 bytes = ctx_bytes(c);
    f64 media = ctx_media_tokens(c);
    f64 text  = (f64)prompt_tokens - media;
    if (text < 0) text = 0;
    
    if (g->basis && bytes - g->fit_bytes >= CTX_FIT_MIN_BYTES
        && text > g->fit_text) {
        f64 slope = (text - g->fit_text) / (bytes - g->fit_bytes);
        if (slope >= CTX_SLOPE_MIN && slope <= CTX_SLOPE_MAX) g->slope = slope;
    }
    g->offset      = text - g->slope * bytes;
    g->fit_tokens  = prompt_tokens;
    g->fit_text    = text;
    g->fit_media   = media;
    g->fit_bytes   = bytes;
    g->exact_slots = c->n;
    g->measured    = true;
    g->basis       = true;
}

void ctx_model_changed(CtxGauge *g) {
    if (!g) return;
    
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
    f64 media = ctx_media_tokens(c);
    
    if (c->n == g->exact_slots && bytes == g->fit_bytes
        && media == g->fit_media) {
        *tokens = g->fit_tokens;
        *exact  = true;
        return true;
    }
    f64 v = g->offset + g->slope * bytes + media;
    
    if (bytes > g->fit_bytes && media >= g->fit_media
        && v < (f64)g->fit_tokens) v = (f64)g->fit_tokens;
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
