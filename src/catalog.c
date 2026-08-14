#include "agent.h"

#include <stdio.h>
#include <string.h>

/* One reply at a time: a listing is read into a buffer of its own and parsed
 * into a DOM beside it, and the ids worth keeping are copied out before the
 * next provider is asked. The region is carved once and reset per provider,
 * so thirty providers cost what the largest one does rather than thirty times
 * the cap of a reply. The multiple is room for the DOM over the bytes it was
 * parsed from, which no reply of that cap has been seen to exceed. */
#define CATALOG_REPLY_BYTES (AGENT_MAX_MODEL_BYTES * 8)

b8 catalog_init(Catalog *c, size_t cap, Arena *a) {
    memset(c, 0, sizeof *c);
    if (!cap) return false;
    c->provider = arena_new(a, Str, cap);
    c->model = arena_new(a, Str, cap);
    if (!c->provider || !c->model) return false;
    c->cap = cap;
    return true;
}

b8 catalog_add(Catalog *c, Str provider, Str model) {
    if (!model.n) return false;
    if (c->n >= c->cap) {
        c->full = true;
        return false;
    }
    c->provider[c->n] = provider;
    c->model[c->n] = model;
    c->n++;
    return true;
}

size_t catalog_endpoints(const Config *cfg, const Endpoints *e, Str *out,
                         size_t max) {
    size_t n = 0;
    /* The run's own endpoint is offered only when no provider was chosen:
     * with one chosen, its stored connection is already in the list, and a
     * second unnamed copy of it would be a second row for every model. */
    if (n < max && cfg && !cfg->provider.n && cfg->base_url_set)
        out[n++] = (Str){0};
    for (size_t i = 0; e && i < e->n && n < max; i++) out[n++] = e->name[i];
    return n;
}

/* The configuration one endpoint is listed through: the stored provider's URL,
 * API and key, or the run's own when the name is empty. False when the
 * provider is gone or its key cannot be read, with `err` saying which. */
static b8 catalog_probe(Config *probe, Str name, const Config *cfg,
                        const Endpoints *e, Arena *tmp,
                        char *err, size_t err_cap) {
    *probe = *cfg;
    probe->provider = (Str){0};
    probe->model = (Str){0};
    if (!name.n) return true;
    size_t i = endpoints_find(e, name);
    if (i == ENDPOINT_NONE) {
        snprintf(err, err_cap, "no longer configured");
        return false;
    }
    char key_err[AGENT_MAX_PATH + 96] = {0};
    Str key = endpoints_key(name, tmp, tmp, key_err, sizeof key_err);
    if (key_err[0]) {
        snprintf(err, err_cap, "%s", key_err);
        return false;
    }
    probe->base_url = e->base_url[i];
    probe->api = e->api[i];
    probe->api_key = key;
    return true;
}

static void catalog_failed(Catalog *c, Str name, Str reason, Arena *out) {
    if (c->n_failed >= AGENT_MAX_ENDPOINTS) return;
    c->failed[c->n_failed] = str_dup_opt(out, name);
    c->reason[c->n_failed] = str_dup_opt(out, reason);
    c->n_failed++;
}

size_t catalog_load(Catalog *c, const Config *cfg, const Endpoints *e,
                    size_t cap, Arena *out, CatalogProgress progress,
                    void *ud) {
    if (!catalog_init(c, cap, out)) return 0;
    Str names[AGENT_MAX_ENDPOINTS + 1];
    size_t n_names = catalog_endpoints(cfg, e, names,
                                      AGENT_MAX_ENDPOINTS + 1);
    if (!n_names) return 0;

    void *mem = arena_alloc(out, CATALOG_REPLY_BYTES, 64);
    Str *ids = arena_new(out, Str, AGENT_MAX_MODELS);
    if (!mem || !ids) return 0;
    Arena tmp;
    arena_init(&tmp, mem, CATALOG_REPLY_BYTES);

    for (size_t i = 0; i < n_names; i++) {
        arena_reset(&tmp);
        if (progress) progress(names[i], ud);
        Config probe;
        char err[AGENT_MAX_PATH + 160] = {0};
        size_t got = 0;
        if (catalog_probe(&probe, names[i], cfg, e, &tmp, err, sizeof err))
            got = provider_models(&probe, &tmp, ids, AGENT_MAX_MODELS,
                                  err, sizeof err);
        if (!got) {
            catalog_failed(c, names[i], str_c(err), out);
            continue;
        }
        /* Copied out of `tmp` before the next provider overwrites it. A name
         * that will not fit is dropped rather than half-stored: a truncated
         * id names another model. */
        for (size_t j = 0; j < got; j++) {
            Str id = str_dup_opt(out, ids[j]);
            if (!id.p || !catalog_add(c, names[i], id)) break;
        }
    }
    return c->n;
}
