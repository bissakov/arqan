/* favorites.c: the models pinned to the top of the /model picker.
 *
 * A favorite is a choice the UI made rather than a document the user edits,
 * so the list lives in the state file beside the remembered model. A model id
 * only means something against the endpoint that served it, so the list is
 * kept per provider:
 *
 *   [favorites.openai]
 *   models = "gpt-5, o3"
 *
 * A run with no named provider writes the "[favorites]" section instead,
 * which is the same split config.c and endpoints.c make between a remembered
 * model and a provider's own.
 */
#include "agent.h"

#include <stdio.h>
#include <string.h>

#define FAVORITES_SECTION STR("favorites")
#define FAVORITES_KEY     STR("models")

/* "favorites.<provider>", or "favorites" when no provider is named. A name
 * that is not a TOML bare key has no section of its own, and must not fall
 * back to the shared one: that would pin its models for every other run. */
static Str favorites_section(Str provider, Arena *a) {
    if (!provider.n) return FAVORITES_SECTION;
    if (!endpoint_name_ok(provider)) return (Str){0};
    Buf b;
    buf_init(&b, a, FAVORITES_SECTION.n + provider.n + 2);
    buf_puts(&b, FAVORITES_SECTION);
    buf_putc(&b, '.');
    buf_puts(&b, provider);
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

size_t favorites_load(Favorites *f, Str provider, Arena *a) {
    memset(f, 0, sizeof *f);
    Str section = favorites_section(provider, a);
    if (!section.n) return 0;
    Str path = paths_file(AGENT_DIR_STATE, AGENT_STATE_NAME, a);
    Settings s;
    if (!path.n || !settings_load(&s, path, a)) return 0;
    Str list = settings_get(&s, section, FAVORITES_KEY);
    size_t off = 0;
    while (off < list.n && f->n < AGENT_MAX_FAVORITES) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') end++;
        Str item = str_trim((Str){ list.p + off, end - off });
        off = end + 1;
        if (!item.n || item.n > AGENT_MAX_MODEL_NAME) continue;
        if (favorites_has(f, item)) continue;
        f->model[f->n++] = item;
    }
    return f->n;
}

b8 favorites_has(const Favorites *f, Str model) {
    for (size_t i = 0; i < f->n; i++)
        if (str_eq(f->model[i], model)) return true;
    return false;
}

/* The list as one settings value. Empty when nothing is pinned, which is how
 * settings_set removes the key. */
static Str favorites_join(const Favorites *f, Arena *a) {
    Buf b;
    buf_init(&b, a, AGENT_MAX_FAVORITES * (AGENT_MAX_MODEL_NAME + 2) + 1);
    for (size_t i = 0; i < f->n; i++) {
        if (i) buf_puts(&b, STR(", "));
        buf_puts(&b, f->model[i]);
    }
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

b8 favorites_toggle(Favorites *f, Str provider, Str model, Arena *scratch,
                    b8 *on, char *err, size_t err_cap) {
    if (on) *on = favorites_has(f, model);
    if (!model.n || model.n > AGENT_MAX_MODEL_NAME) return false;
    /* The list is one comma-separated value, so a model id carrying a comma
     * could not be read back as itself. */
    for (size_t i = 0; i < model.n; i++)
        if (model.p[i] == ',') {
            if (err) snprintf(err, err_cap,
                              "a model id with a comma cannot be favorited");
            return false;
        }

    size_t at = f->n;
    for (size_t i = 0; i < f->n; i++)
        if (str_eq(f->model[i], model)) { at = i; break; }
    if (at < f->n) {
        for (size_t i = at + 1; i < f->n; i++) f->model[i - 1] = f->model[i];
        f->n--;
    } else {
        if (f->n >= AGENT_MAX_FAVORITES) {
            if (err) snprintf(err, err_cap, "no room for another favorite "
                              "(%d); unfavorite one first",
                              (i32)AGENT_MAX_FAVORITES);
            return false;
        }
        f->model[f->n++] = model;
    }
    if (on) *on = favorites_has(f, model);

    size_t mark = scratch->off;
    Str section = favorites_section(provider, scratch);
    Str value = favorites_join(f, scratch);
    b8 ok = section.n && (value.p || !f->n)
         && state_set_in(section, FAVORITES_KEY, value, scratch);
    scratch->off = mark;
    if (!ok && err) snprintf(err, err_cap, "could not write the state file");
    return ok;
}
