/* endpoints.c: the providers the /provider command creates and switches to.
 *
 * An endpoint is a user-defined service: a name, a base URL, the API that URL
 * speaks and the model last used against it. Nothing is built in, because
 * only the user knows which endpoints exist.
 * (`Provider` in provider.c is the streaming run context; this is the entry a
 * run is configured from.)
 *
 * An endpoint is a section of the config file, so the settings a user edits
 * are one document:
 *
 *   [provider openai]
 *   base_url = https://api.openai.com/v1
 *   model = gpt-4o-mini
 *   api = openai
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

ApiKind api_from_str(Str s) {
    return str_eq(str_trim(s), STR("anthropic")) ? API_ANTHROPIC : API_OPENAI;
}

Str api_name(ApiKind k) {
    return k == API_ANTHROPIC ? STR("anthropic") : STR("openai");
}

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

/* Provider capabilities are deliberately data, not a built-in ladder. */
static b8 endpoint_list_ok(Str list, b8 budgets) {
    if (list.n > YOKE_MAX_REASONING_LIST) return false;
    size_t off = 0, count = 0;
    while (off < list.n) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') end++;
        Str item = str_trim((Str){ list.p + off, end - off });
        if (!item.n || ++count > YOKE_MAX_ENDPOINTS) return false;
        if (budgets) {
            b8 ok = false; i64 n = str_int(item, &ok);
            if (!ok || n <= 0) return false;
        }
        size_t prev = 0;
        while (prev < off) {
            size_t pend = prev;
            while (pend < list.n && list.p[pend] != ',') pend++;
            if (str_eq(item, str_trim((Str){ list.p + prev, pend - prev })))
                return false;
            prev = pend + 1;
        }
        off = end + 1;
    }
    return true;
}

static b8 endpoint_selected_ok(Str list, Str selected) {
    if (!selected.n) return true;
    size_t off = 0;
    while (off < list.n) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') end++;
        if (str_eq(selected, str_trim((Str){ list.p + off, end - off }))) return true;
        off = end + 1;
    }
    return false;
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
        ApiKind api = api_from_str(settings_get(s, sections[i], STR("api")));
        Str efforts = endpoint_field(s, sections[i], STR("reasoning_efforts"), YOKE_MAX_REASONING_LIST);
        Str budgets = endpoint_field(s, sections[i], STR("thinking_budgets"), YOKE_MAX_REASONING_LIST);
        Str effort = endpoint_field(s, sections[i], STR("reasoning_effort"), YOKE_MAX_REASONING_LIST);
        Str budget = endpoint_field(s, sections[i], STR("thinking_budget"), YOKE_MAX_REASONING_LIST);
        Str templ = endpoint_field(s, sections[i], STR("reasoning_template"), YOKE_MAX_REASONING_TEMPLATE);
        endpoints_put(e, name, url, model, api, efforts, budgets, effort, budget, templ, a);
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

b8 endpoints_put(Endpoints *e, Str name, Str base_url, Str model, ApiKind api,
                 Str efforts, Str budgets, Str effort, Str budget, Str templ,
                 Arena *a) {
    if (!name.n || name.n > YOKE_MAX_ENDPOINT_NAME) return false;
    if (!base_url.n || base_url.n > YOKE_MAX_URL) return false;
    if (model.n > YOKE_MAX_MODEL_NAME) return false;
    if (!endpoint_list_ok(efforts, false) || !endpoint_list_ok(budgets, true)
        || !endpoint_selected_ok(efforts, effort)
        || !endpoint_selected_ok(budgets, budget)
        || templ.n > YOKE_MAX_REASONING_TEMPLATE) return false;
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
    Str ef = efforts.n ? str_dup(a, efforts) : (Str){0};
    Str bu = budgets.n ? str_dup(a, budgets) : (Str){0};
    Str es = effort.n ? str_dup(a, effort) : (Str){0};
    Str bs = budget.n ? str_dup(a, budget) : (Str){0};
    Str te = templ.n ? str_dup(a, templ) : (Str){0};
    if (!url.p || (model.n && !mdl.p) || (efforts.n && !ef.p)
        || (budgets.n && !bu.p) || (effort.n && !es.p)
        || (budget.n && !bs.p) || (templ.n && !te.p)) return false;
    e->base_url[i] = url;
    e->model[i] = mdl;
    e->api[i] = api;
    e->reasoning_efforts[i] = ef;
    e->thinking_budgets[i] = bu;
    e->reasoning_effort[i] = es;
    e->thinking_budget[i] = bs;
    e->reasoning_template[i] = te;
    return true;
}

/* One endpoint at a time, since the rest of the config file is the user's and
 * a rewrite would cost them their comments and their order. */
b8 endpoints_save_one(Str name, Str base_url, Str model, ApiKind api,
                      Str efforts, Str budgets, Str effort, Str budget,
                      Str templ, Arena *scratch) {
    size_t mark = scratch->off;
    Str dir  = paths_dir(YOKE_DIR_CONFIG, scratch);
    Str path = paths_file(YOKE_DIR_CONFIG, STR("config"), scratch);
    Str section = endpoint_section(name, scratch);
    if (!dir.n || !path.n || !section.n || !paths_ensure_dir(dir)) {
        scratch->off = mark;
        return false;
    }
    Str keys[8] = { STR("base_url"), STR("model"), STR("api"),
        STR("reasoning_efforts"), STR("thinking_budgets"),
        STR("reasoning_effort"), STR("thinking_budget"), STR("reasoning_template") };
    Str vals[8] = { base_url, model, api_name(api), efforts, budgets, effort, budget, templ };
    b8 ok = settings_set(path, section, keys, vals, 8, 0600, scratch);
    scratch->off = mark;
    return ok;
}

b8 endpoints_remember_model(Str name, Str model, Arena *scratch) {
    Endpoints e;
    size_t mark = scratch->off;
    endpoints_load(&e, scratch);
    size_t i = endpoints_find(&e, name);
    b8 ok = i != ENDPOINT_NONE
         && endpoints_save_one(name, e.base_url[i], model, e.api[i],
             e.reasoning_efforts[i], e.thinking_budgets[i], e.reasoning_effort[i],
             e.thinking_budget[i], e.reasoning_template[i], scratch);
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

b8 endpoints_delete(Str name, Arena *scratch, char *err, size_t err_cap) {
    if (!name.n || name.n > YOKE_MAX_ENDPOINT_NAME) return false;
    size_t mark = scratch->off;
    Settings credentials;
    Str credential_path = {0};
    Str config_path = paths_file(YOKE_DIR_CONFIG, STR("config"), scratch);
    Str section = endpoint_section(name, scratch);
    b8 ok = config_path.n && section.n
         && creds_open(&credentials, scratch, &credential_path, err, err_cap)
         && settings_remove_section(config_path, section, scratch)
         && settings_remove_section(credential_path, section, scratch);
    scratch->off = mark;
    return ok;
}

Str endpoints_active(Arena *a) {
    Str name = state_get(STR("provider"), a, a);
    return name.n <= YOKE_MAX_ENDPOINT_NAME ? name : (Str){0};
}

b8 endpoints_remember_active(Str name, Arena *scratch) {
    if (name.n > YOKE_MAX_ENDPOINT_NAME) return false;
    return state_set(STR("provider"), name, scratch);
}
