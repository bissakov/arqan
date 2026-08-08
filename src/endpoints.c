/* endpoints.c: the providers the /provider command creates and switches to.
 *
 * An endpoint is a user-defined OpenAI-compatible service: a name, a base URL
 * and the model last used against it. Nothing is built in, because every
 * endpoint speaks the same protocol and only the user knows which ones exist.
 * (`Provider` in provider.c is the streaming run context; this is the entry a
 * run is configured from.)
 *
 * An endpoint is a section of the config file, so the settings a user edits
 * are one document:
 *
 *   [provider openai]
 *   base_url = https://api.openai.com/v1
 *   model = gpt-4o-mini
 *
 * The key is not there. It lives under the same section name in
 * $XDG_STATE_HOME/yoke/credentials at mode 0600, so the file a dotfile
 * repository carries holds no secret, and a credentials file anyone else can
 * read is refused rather than loaded. The active endpoint is the `provider`
 * key of the state file.
 */
#include "yoke.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define ENDPOINT_SECTION STR("provider ")

/* "provider <name>", the section both files key an endpoint by. */
static Str endpoint_section(Str name, Arena *a) {
    Buf b; buf_init(&b, a, ENDPOINT_SECTION.n + name.n + 1);
    buf_puts(&b, ENDPOINT_SECTION);
    buf_puts(&b, name);
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

/* A field is refused rather than clamped when it is longer than its cap: a
 * truncated URL or key names something else. */
static Str endpoint_field(const Settings *s, Str section, Str key, size_t max) {
    Str v = settings_get(s, section, key);
    return v.n <= max ? v : (Str){0};
}

static void endpoints_collect(Endpoints *e, const Settings *s, Arena *a) {
    Str sections[YOKE_MAX_ENDPOINTS];
    size_t n = settings_sections(s, ENDPOINT_SECTION, sections,
                                 YOKE_MAX_ENDPOINTS);
    for (size_t i = 0; i < n; i++) {
        Str name = str_trim(str_drop(sections[i], ENDPOINT_SECTION.n));
        if (!name.n || name.n > YOKE_MAX_ENDPOINT_NAME) continue;
        Str url = endpoint_field(s, sections[i], STR("base_url"), YOKE_MAX_URL);
        if (!url.n) continue;
        Str model = endpoint_field(s, sections[i], STR("model"),
                                   YOKE_MAX_MODEL_NAME);
        endpoints_put(e, name, url, model, a);
    }
}

size_t endpoints_load(Endpoints *e, Arena *a) {
    memset(e, 0, sizeof *e);
    Str files[YOKE_MAX_CONFIG_FILES];
    size_t n = paths_config_files(STR("config"), a, files, YOKE_MAX_CONFIG_FILES);
    /* Lowest precedence first, so a user's entry replaces a system one of the
     * same name the way a config key does. */
    for (size_t i = 0; i < n; i++) {
        Settings s;
        if (settings_load(&s, files[i], a)) endpoints_collect(e, &s, a);
    }
    return e->n;
}

size_t endpoints_find(const Endpoints *e, Str name) {
    for (size_t i = 0; i < e->n; i++)
        if (str_eq(e->name[i], name)) return i;
    return ENDPOINT_NONE;
}

b8 endpoints_put(Endpoints *e, Str name, Str base_url, Str model, Arena *a) {
    if (!name.n || name.n > YOKE_MAX_ENDPOINT_NAME) return false;
    if (!base_url.n || base_url.n > YOKE_MAX_URL) return false;
    if (model.n > YOKE_MAX_MODEL_NAME) return false;
    size_t i = endpoints_find(e, name);
    if (i == ENDPOINT_NONE) {
        if (e->n >= YOKE_MAX_ENDPOINTS) return false;
        i = e->n;
        Str dup = str_dup(a, name);
        if (!dup.p) return false;
        e->name[i] = dup;
        e->n++;
    }
    Str url = str_dup(a, base_url);
    Str mdl = model.n ? str_dup(a, model) : (Str){0};
    if (!url.p || (model.n && !mdl.p)) return false;
    e->base_url[i] = url;
    e->model[i] = mdl;
    return true;
}

/* One endpoint at a time, since the rest of the config file is the user's and
 * a rewrite would cost them their comments and their order. */
b8 endpoints_save_one(Str name, Str base_url, Str model, Arena *scratch) {
    size_t mark = scratch->off;
    Str dir  = paths_dir(YOKE_DIR_CONFIG, scratch);
    Str path = paths_file(YOKE_DIR_CONFIG, STR("config"), scratch);
    Str section = endpoint_section(name, scratch);
    if (!dir.n || !path.n || !section.n || !paths_ensure_dir(dir)) {
        scratch->off = mark;
        return false;
    }
    Str keys[2] = { STR("base_url"), STR("model") };
    Str vals[2] = { base_url, model };
    b8 ok = settings_set(path, section, keys, vals, 2, 0600, scratch);
    scratch->off = mark;
    return ok;
}

b8 endpoints_remember_model(Str name, Str model, Arena *scratch) {
    Endpoints e;
    size_t mark = scratch->off;
    endpoints_load(&e, scratch);
    size_t i = endpoints_find(&e, name);
    b8 ok = i != ENDPOINT_NONE
         && endpoints_save_one(name, e.base_url[i], model, scratch);
    scratch->off = mark;
    return ok;
}

/* The credentials file, refused when anyone but the owner can read it: a key
 * left world-readable is a key to rotate, not one to load. */
static b8 creds_open(Settings *s, Arena *a, Str *path_out,
                     char *err, size_t err_cap) {
    s->n = 0;
    Str path = paths_file(YOKE_DIR_STATE, STR("credentials"), a);
    if (path_out) *path_out = path;
    if (!path.n) return false;
    struct stat st;
    if (stat(path.p, &st) != 0) return true;   /* no file is not a failure */
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        if (err) snprintf(err, err_cap, "credentials are readable by others: "
                          "chmod 600 %.*s", (i32)path.n, path.p);
        return false;
    }
    settings_load(s, path, a);
    return true;
}

Str endpoints_key(Str name, Arena *out, Arena *scratch,
                  char *err, size_t err_cap) {
    Settings s;
    size_t mark = scratch->off;
    Str key = {0};
    Str section = endpoint_section(name, scratch);
    if (section.n && creds_open(&s, scratch, NULL, err, err_cap)) {
        Str v = endpoint_field(&s, section, STR("key"), YOKE_MAX_API_KEY);
        if (v.n) key = str_dup(out, v);
    }
    scratch->off = mark;
    return key;
}

b8 endpoints_set_key(Str name, Str key, Arena *scratch,
                     char *err, size_t err_cap) {
    if (!name.n || key.n > YOKE_MAX_API_KEY) return false;
    size_t mark = scratch->off;
    Settings s;
    Str path = {0};
    Str section = endpoint_section(name, scratch);
    Str dir = paths_dir(YOKE_DIR_STATE, scratch);
    b8 ok = section.n && creds_open(&s, scratch, &path, err, err_cap)
         && path.n && dir.n && paths_ensure_dir(dir)
         && settings_set_one(path, section, STR("key"), key, 0600, scratch);
    scratch->off = mark;
    return ok;
}

Str endpoints_active(Arena *a) {
    Str name = state_get(STR("provider"), a, a);
    return name.n <= YOKE_MAX_ENDPOINT_NAME ? name : (Str){0};
}

b8 endpoints_remember_active(Str name, Arena *scratch) {
    if (!name.n || name.n > YOKE_MAX_ENDPOINT_NAME) return false;
    return state_set(STR("provider"), name, scratch);
}
