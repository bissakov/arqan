#include "agent.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define ENDPOINT_SECTION STR("providers.")

ApiKind api_from_str(Str s) {
    return str_eq(str_trim(s), STR("anthropic")) ? API_ANTHROPIC : API_OPENAI;
}

Str api_name(ApiKind k) {
    return k == API_ANTHROPIC ? STR("anthropic") : STR("openai");
}

/* A name that is a TOML bare key, so "[providers.<name>]" stays a header a
 * TOML reader and this one agree on. */
b8 endpoint_name_ok(Str name) {
    if (!name.n || name.n > AGENT_MAX_ENDPOINT_NAME) return false;
    for (size_t i = 0; i < name.n; i++) {
        char c = name.p[i];
        b8 ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
             || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

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

// Provider capabilities are deliberately data, not a built-in ladder.
static b8 endpoint_list_ok(Str list, b8 budgets) {
    if (list.n > AGENT_MAX_REASONING_LIST) return false;
    size_t off = 0, count = 0;
    while (off < list.n) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') end++;
        Str item = str_trim((Str){ list.p + off, end - off });
        if (!item.n || ++count > AGENT_MAX_ENDPOINTS) return false;
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

/* Said rather than obeyed: a shared config file naming a key store would be a
 * way to choose what arqan runs, so the line is reported and dropped. */
static void endpoint_warn_credential_keys(const Settings *s, Str section,
                                          Str name) {
    const Str keys[3] = { STR("key"), STR("key_source"),
                          STR("key_command") };
    for (size_t i = 0; i < 3; i++) {
        if (!settings_get(s, section, keys[i]).n) continue;
        agent_log(AGENT_LOG_WARN,
                 "ignoring %.*s in the config file for provider %.*s: "
                 "it belongs in the credentials file",
                 (i32)keys[i].n, keys[i].p, (i32)name.n, name.p);
    }
}

static void endpoints_collect(Endpoints *e, const Settings *s, Arena *a) {
    Str sections[AGENT_MAX_ENDPOINTS];
    size_t n = settings_sections(s, ENDPOINT_SECTION, sections,
                                 AGENT_MAX_ENDPOINTS);
    for (size_t i = 0; i < n; i++) {
        Str name = str_trim(str_drop(sections[i], ENDPOINT_SECTION.n));
        if (!endpoint_name_ok(name)) continue;
        endpoint_warn_credential_keys(s, sections[i], name);
        Str url = endpoint_field(s, sections[i], STR("base_url"), AGENT_MAX_URL);
        if (!url.n) continue;
        Str model = endpoint_field(s, sections[i], STR("model"),
                                   AGENT_MAX_MODEL_NAME);
        ApiKind api = api_from_str(settings_get(s, sections[i], STR("api")));
        Str efforts = endpoint_field(s, sections[i], STR("reasoning_efforts"), AGENT_MAX_REASONING_LIST);
        Str budgets = endpoint_field(s, sections[i], STR("thinking_budgets"), AGENT_MAX_REASONING_LIST);
        Str effort = endpoint_field(s, sections[i], STR("reasoning_effort"), AGENT_MAX_REASONING_LIST);
        Str budget = endpoint_field(s, sections[i], STR("thinking_budget"), AGENT_MAX_REASONING_LIST);
        Str templ = endpoint_field(s, sections[i], STR("reasoning_template"), AGENT_MAX_REASONING_TEMPLATE);
        endpoints_put(e, name, url, model, api, efforts, budgets, effort, budget, templ, a);
    }
}

size_t endpoints_load(Endpoints *e, Arena *a) {
    memset(e, 0, sizeof *e);
    Str files[AGENT_MAX_CONFIG_FILES + AGENT_MAX_PROJECT_FILES];
    size_t n = paths_config_files(AGENT_CONFIG_NAME, a, files,
                                  AGENT_MAX_CONFIG_FILES);
    /* A project may name endpoints too, and its files sit above the global
     * ones for the same reason its settings do. */
    n += paths_project_files(AGENT_CONFIG_NAME, a, files + n,
                             AGENT_MAX_PROJECT_FILES);
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
    if (!endpoint_name_ok(name)) return false;
    if (!base_url.n || base_url.n > AGENT_MAX_URL) return false;
    if (model.n > AGENT_MAX_MODEL_NAME) return false;
    if (!endpoint_list_ok(efforts, false) || !endpoint_list_ok(budgets, true)
        || !endpoint_selected_ok(efforts, effort)
        || !endpoint_selected_ok(budgets, budget)
        || templ.n > AGENT_MAX_REASONING_TEMPLATE) return false;
    size_t i = endpoints_find(e, name);
    if (i == ENDPOINT_NONE) {
        if (e->n >= AGENT_MAX_ENDPOINTS) return false;
        i = e->n;
        Str dup = str_dup(a, name);
        if (!dup.p) return false;
        e->name[i] = dup;
        e->n++;
    }
    Str url = str_dup(a, base_url);
    Str mdl = str_dup_opt(a, model);
    Str ef = str_dup_opt(a, efforts);
    Str bu = str_dup_opt(a, budgets);
    Str es = str_dup_opt(a, effort);
    Str bs = str_dup_opt(a, budget);
    Str te = str_dup_opt(a, templ);
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
    Str dir  = paths_dir(AGENT_DIR_CONFIG, scratch);
    Str path = paths_file(AGENT_DIR_CONFIG, AGENT_CONFIG_NAME, scratch);
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
    Str path = paths_file(AGENT_DIR_STATE, AGENT_CREDENTIALS_NAME, a);
    if (path_out) *path_out = path;
    if (!path.n) return false;
    struct stat st;
    if (stat(path.p, &st) != 0) return true;   // no file is not a failure
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        if (err) snprintf(err, err_cap, "credentials are readable by others: "
                          "chmod 600 %.*s", (i32)path.n, path.p);
        return false;
    }
    settings_load(s, path, a);
    return true;
}

/* The source line, read from the credentials file alone. An unknown value is
 * refused rather than treated as "the key is in the file": that would send a
 * key the user meant to keep in a keyring back to plaintext. */
static SecretSource creds_source(const Settings *s, Str section, Str name,
                                 char *err, size_t err_cap) {
    Str v = settings_get(s, section, STR("key_source"));
    b8 known = true;
    SecretSource src = secret_source_from_str(v, &known);
    if (!known) {
        if (err) snprintf(err, err_cap, "provider %.*s names an unknown "
                          "key_source", (i32)name.n, name.p);
        return SECRET_STORED;
    }
    return src;
}

SecretSource endpoints_key_source(Str name, Arena *scratch) {
    Settings s;
    size_t mark = scratch->off;
    SecretSource src = SECRET_STORED;
    Str section = endpoint_section(name, scratch);
    if (section.n && creds_open(&s, scratch, NULL, NULL, 0))
        src = creds_source(&s, section, name, NULL, 0);
    scratch->off = mark;
    return src;
}

Str endpoints_key(Str name, Arena *out, Arena *scratch,
                  char *err, size_t err_cap) {
    Settings s;
    size_t mark = scratch->off;
    Str key = {0};
    Str section = endpoint_section(name, scratch);
    if (section.n && creds_open(&s, scratch, NULL, err, err_cap)) {
        char src_err[160] = {0};
        SecretSource src = creds_source(&s, section, name, src_err,
                                        sizeof src_err);
        if (src_err[0]) {
            if (err) snprintf(err, err_cap, "%s", src_err);
        } else if (src == SECRET_STORED) {
            Str v = endpoint_field(&s, section, STR("key"), AGENT_MAX_API_KEY);
            if (v.n) key = str_dup(out, v);
        } else {
            Str cmd = settings_get(&s, section, STR("key_command"));
            key = secret_lookup(src, name, cmd, out, src_err, sizeof src_err);
            if (!key.n && src_err[0] && err)
                snprintf(err, err_cap, "%.*s key store: %s",
                         (i32)name.n, name.p, src_err);
        }
    }
    scratch->off = mark;
    return key;
}

b8 endpoints_set_key(Str name, Str key, SecretSource src, Arena *scratch,
                     char *err, size_t err_cap) {
    if (!name.n || key.n > AGENT_MAX_API_KEY) return false;
    if (src == SECRET_COMMAND) {
        if (err) snprintf(err, err_cap, "a key_command provider is filled in "
                          "with its own tool");
        return false;
    }
    size_t mark = scratch->off;
    Settings s;
    Str path = {0};
    Str section = endpoint_section(name, scratch);
    Str dir = paths_dir(AGENT_DIR_STATE, scratch);
    b8 ok = section.n && creds_open(&s, scratch, &path, err, err_cap)
         && path.n && dir.n && paths_ensure_dir(dir);

    /* The store that held it is cleared first, so switching stores or
     * clearing a key never leaves a live copy in the other one. */
    if (ok) {
        SecretSource old = creds_source(&s, section, name, NULL, 0);
        if (secret_source_external(old) && (old != src || !key.n))
            secret_erase(old, name, NULL, 0);
        if (secret_source_external(src) && key.n)
            ok = secret_store(src, name, key, err, err_cap);
    }
    Str keys[2] = { STR("key"), STR("key_source") };
    Str vals[2] = { secret_source_external(src) ? (Str){0} : key,
                    secret_source_external(src) ? secret_source_name(src)
                                                : (Str){0} };
    ok = ok && settings_set(path, section, keys, vals, 2, 0600, scratch);
    scratch->off = mark;
    return ok;
}

/* The key goes first and on its own. Removing an endpoint touches two files,
 * and only one of them holds a secret: gating the credential's removal on the
 * config rewrite would leave a deleted provider's key on disk whenever that
 * rewrite failed, which is the one outcome this must never have. Each step is
 * reported separately for the same reason. */
b8 endpoints_delete(Str name, Arena *scratch, char *err, size_t err_cap) {
    if (!endpoint_name_ok(name)) return false;
    size_t mark = scratch->off;
    Settings credentials;
    Str credential_path = {0};
    Str config_path = paths_file(AGENT_DIR_CONFIG, AGENT_CONFIG_NAME, scratch);
    Str section = endpoint_section(name, scratch);
    if (!config_path.n || !section.n
        || !creds_open(&credentials, scratch, &credential_path, err, err_cap)) {
        scratch->off = mark;
        return false;
    }

    char store_err[160] = {0};
    SecretSource src = creds_source(&credentials, section, name, NULL, 0);
    b8 erased = !secret_source_external(src)
             || secret_erase(src, name, store_err, sizeof store_err);
    b8 cleared = settings_remove_section(credential_path, section, scratch);
    b8 removed = settings_remove_section(config_path, section, scratch);

    if (!erased)
        snprintf(err, err_cap, "the key is still in its store: %s", store_err);
    else if (!cleared)
        snprintf(err, err_cap, "the key could not be removed from %.*s",
                 (i32)credential_path.n, credential_path.p);
    else if (!removed)
        snprintf(err, err_cap, "the key is gone, but its settings could not "
                 "be removed from %.*s", (i32)config_path.n, config_path.p);
    scratch->off = mark;
    return erased && cleared && removed;
}

b8 endpoints_remember_active(Str name, Arena *scratch) {
    if (name.n && !endpoint_name_ok(name)) return false;
    return state_set(STR("provider"), name, scratch);
}
