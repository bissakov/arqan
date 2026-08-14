#include "agent.h"

#include <stdio.h>
#include <string.h>

#define FAVORITES_SECTION STR("favorites")
#define FAVORITES_KEY     STR("models")

/* "favorites.<provider>", or "favorites" for the endpoint a run names with a
 * base URL alone. A name that is not a TOML bare key has no section of its
 * own, and must not fall back to the shared one: that would pin its models
 * for every other run. */
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

/* One provider's comma-separated list, appended as pairs. `provider` is kept
 * by reference, which is why it must outlive the Favorites. */
static void favorites_parse(Favorites *f, Str provider, Str list) {
    size_t off = 0;
    while (off < list.n && f->n < AGENT_MAX_FAVORITES) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') end++;
        Str item = str_trim((Str){ list.p + off, end - off });
        off = end + 1;
        if (!item.n || item.n > AGENT_MAX_MODEL_NAME) continue;
        if (favorites_has(f, provider, item)) continue;
        f->provider[f->n] = provider;
        f->model[f->n] = item;
        f->n++;
    }
}

size_t favorites_load(Favorites *f, const Endpoints *e, Arena *a) {
    memset(f, 0, sizeof *f);
    Str path = paths_file(AGENT_DIR_STATE, AGENT_STATE_NAME, a);
    Settings s;
    // The state file is read once here rather than once per provider.
    if (!path.n || !settings_load(&s, path, a)) return 0;
    favorites_parse(f, (Str){0}, settings_get(&s, FAVORITES_SECTION,
                                              FAVORITES_KEY));
    for (size_t i = 0; e && i < e->n; i++) {
        Str section = favorites_section(e->name[i], a);
        if (!section.n) continue;
        favorites_parse(f, e->name[i], settings_get(&s, section,
                                                    FAVORITES_KEY));
    }
    return f->n;
}

b8 favorites_has(const Favorites *f, Str provider, Str model) {
    for (size_t i = 0; i < f->n; i++)
        if (str_eq(f->model[i], model) && str_eq(f->provider[i], provider))
            return true;
    return false;
}

/* One provider's pins as one settings value. Empty when it has none, which is
 * how settings_set removes the key. */
static Str favorites_join(const Favorites *f, Str provider, Arena *a) {
    Buf b;
    buf_init(&b, a, AGENT_MAX_FAVORITES * (AGENT_MAX_MODEL_NAME + 2) + 1);
    for (size_t i = 0, written = 0; i < f->n; i++) {
        if (!str_eq(f->provider[i], provider)) continue;
        if (written++) buf_puts(&b, STR(", "));
        buf_puts(&b, f->model[i]);
    }
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

static size_t favorites_count(const Favorites *f, Str provider) {
    size_t n = 0;
    for (size_t i = 0; i < f->n; i++)
        if (str_eq(f->provider[i], provider)) n++;
    return n;
}

b8 favorites_toggle(Favorites *f, Str provider, Str model, Arena *scratch,
                    b8 *on, char *err, size_t err_cap) {
    if (on) *on = favorites_has(f, provider, model);
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
        if (str_eq(f->model[i], model) && str_eq(f->provider[i], provider)) {
            at = i;
            break;
        }
    if (at < f->n) {
        for (size_t i = at + 1; i < f->n; i++) {
            f->provider[i - 1] = f->provider[i];
            f->model[i - 1] = f->model[i];
        }
        f->n--;
    } else {
        if (f->n >= AGENT_MAX_FAVORITES) {
            if (err) snprintf(err, err_cap, "no room for another favorite "
                              "(%d); unfavorite one first",
                              (i32)AGENT_MAX_FAVORITES);
            return false;
        }
        f->provider[f->n] = provider;
        f->model[f->n] = model;
        f->n++;
    }
    if (on) *on = favorites_has(f, provider, model);

    size_t mark = scratch->off;
    Str section = favorites_section(provider, scratch);
    Str value = favorites_join(f, provider, scratch);
    b8 ok = section.n && (value.p || !favorites_count(f, provider))
         && state_set_in(section, FAVORITES_KEY, value, scratch);
    scratch->off = mark;
    if (!ok && err) snprintf(err, err_cap, "could not write the state file");
    return ok;
}

b8 favorites_forget(Str provider, Arena *scratch) {
    if (!provider.n) return false;
    size_t mark = scratch->off;
    Str path = paths_file(AGENT_DIR_STATE, AGENT_STATE_NAME, scratch);
    Str section = favorites_section(provider, scratch);
    /* Nothing to remove is success: this runs to leave no pins behind, and a
     * provider that never had any has none. */
    b8 ok = path.n && section.n
         && settings_remove_section(path, section, scratch);
    scratch->off = mark;
    return ok;
}
