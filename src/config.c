/* config.c: every setting arqan has, in one table, resolved once.
 *
 * A setting is a row of k_conf: the name it has in a file, its type, its
 * bounds and whether a project file may set it. That name is also its
 * environment variable, upper-cased under ARQAN_, and the name the state file
 * remembers it by, so a new setting is a row rather than a branch in each of
 * five readers.
 *
 * Sources are read lowest precedence first (see ConfOrigin); a write from an
 * origin below the one already recorded is dropped, which is the whole of the
 * precedence rule. A value a source may not set, or one outside its bounds,
 * is reported and dropped rather than clamped: a mistyped line should fall
 * through to the value below it, not shadow it with something nobody wrote.
 *
 * A project's .arqan/config.toml arrives with a `git clone`, so it is not
 * trusted with anything that names a secret or chooses what arqan runs:
 * `api_key` is refused there, and the key directives are refused in every
 * config file (see endpoints.c and secrets.c).
 *
 * The system prompt is not a row here: it is a document, so it lives in
 * SYSTEM.md (see prompt.c). Only ARQAN_SYSTEM_PROMPT and --system set it.
 */
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { CV_STR, CV_NUM, CV_BOOL, CV_ENUM } ConfType;

typedef struct {
    const char *name;
    const char *dflt;
    const char *options;   /* CV_ENUM: every value it may take            */
    ConfType    type;
    i64         lo, hi;    /* CV_NUM: the range, refused outside          */
    size_t      max_len;   /* CV_STR: longer is refused, never truncated  */
    b8          project;   /* may a project's .arqan/config.toml set it    */
} ConfSpec;

#define CONF_TEXT2(x) #x
#define CONF_TEXT(x)  CONF_TEXT2(x)
/* The status field mask has one bit per field, so its default is all of
 * them; a literal is needed because the table's defaults are text. */
_Static_assert(AGENT_STATUS_FIELDS == 9, "the status_fields default is 511");

static const ConfSpec k_conf[CONF_N] = {
    [CONF_PROVIDER] = { "provider", "", NULL, CV_STR, 0, 0,
                        AGENT_MAX_ENDPOINT_NAME, true },
    [CONF_BASE_URL] = { "base_url", "", NULL, CV_STR, 0, 0,
                        AGENT_MAX_URL, true },
    [CONF_MODEL]    = { "model", "", NULL, CV_STR, 0, 0,
                        AGENT_MAX_MODEL_NAME, true },
    [CONF_API]      = { "api", "openai", "openai,anthropic", CV_ENUM,
                        0, 0, 0, true },
    /* Never from a project file: a repository must not be able to hand a
     * key to the endpoint it also names. */
    [CONF_API_KEY]  = { "api_key", "", NULL, CV_STR, 0, 0,
                        AGENT_MAX_API_KEY, false },
    [CONF_MAX_TOKENS]     = { "max_tokens", CONF_TEXT(AGENT_MAX_TOKENS), NULL,
                              CV_NUM, 1, 1 << 20, 0, true },
    [CONF_MAX_MESSAGES]   = { "max_messages", CONF_TEXT(AGENT_MAX_MESSAGES),
                              NULL, CV_NUM, 8, 1 << 20, 0, true },
    [CONF_STREAM]         = { "stream", "true", NULL, CV_BOOL, 0, 0, 0, true },
    [CONF_MODE]           = { "mode", "build", "build,plan", CV_ENUM,
                              0, 0, 0, true },
    [CONF_RETRIES]        = { "retries", CONF_TEXT(AGENT_RETRIES), NULL,
                              CV_NUM, 0, 16, 0, true },
    [CONF_RETRY_DELAY_MS] = { "retry_delay_ms", CONF_TEXT(AGENT_RETRY_DELAY_MS),
                              NULL, CV_NUM, 0, AGENT_MAX_RETRY_DELAY_MS, 0,
                              true },
    /* "none" rather than an empty value: an empty value removes the key,
     * which would read as "nothing was ever chosen" on the next run. */
    [CONF_DISABLE_TOOLS]  = { "disable_tools", "", NULL, CV_STR, 0, 0,
                              AGENT_MAX_TOOL_LIST, true },
    [CONF_VERBOSE_TOOLS]  = { "verbose_tools", "false", NULL, CV_BOOL,
                              0, 0, 0, true },
    [CONF_RAW_MARKDOWN]   = { "raw_markdown", "false", NULL, CV_BOOL,
                              0, 0, 0, true },
    [CONF_SHOW_IGNORED]   = { "show_ignored", "false", NULL, CV_BOOL,
                              0, 0, 0, true },
    [CONF_SHOW_INSTRUCTIONS] = { "show_instructions", "false", NULL, CV_BOOL,
                                 0, 0, 0, true },
    [CONF_WRAP]           = { "wrap", "word", "word,justified", CV_ENUM,
                              0, 0, 0, true },
    [CONF_STATUS_FIELDS]  = { "status_fields", "511", NULL, CV_NUM,
                              0, 511, 0, true },
    /* Recording is the user's decision about their own machine, so a cloned
     * repository does not get to make it. */
    [CONF_TELEMETRY]      = { "telemetry", "false", NULL, CV_BOOL,
                              0, 0, 0, false },
    [CONF_NOTIFY]         = { "notify", "osc9", "off,bel,osc9,both", CV_ENUM,
                              0, 0, 0, true },
    /* Never from a project file: it names a program arqan will run, so a
     * `git clone` must not be able to choose it. */
    [CONF_NOTIFY_COMMAND] = { "notify_command", "", NULL, CV_STR, 0, 0,
                              AGENT_MAX_NOTIFY_CMD, false },
    /* A turn shorter than this had the user watching it, so it passes in
     * silence. Errors and questions ignore the floor. */
    [CONF_NOTIFY_MIN_MS]  = { "notify_min_ms", "10000", NULL, CV_NUM,
                              0, 24 * 60 * 60 * 1000, 0, true },
    /* "auto" is the keyless engine chain. Naming one engine uses only that
     * engine, which is what an endpoint or a key is worth configuring for. */
    [CONF_SEARCH_BACKEND] = { "search_backend", "auto",
                              "auto,ddg,bing,brave,brave_api,google,searxng",
                              CV_ENUM, 0, 0, 0, true },
    /* Never from a project file: these three name where a search goes and
     * what it pays with, so a `git clone` must not be able to choose them. */
    [CONF_SEARCH_ENDPOINT] = { "search_endpoint", "", NULL, CV_STR, 0, 0,
                               AGENT_MAX_URL, false },
    [CONF_SEARCH_API_KEY]  = { "search_api_key", "", NULL, CV_STR, 0, 0,
                               AGENT_MAX_API_KEY, false },
    [CONF_SEARCH_ENGINE_ID] = { "search_engine_id", "", NULL, CV_STR, 0, 0,
                                AGENT_MAX_ENDPOINT_NAME, false },
};

Str conf_key_name(ConfKey k) {
    return k < CONF_N ? str_c(k_conf[k].name) : (Str){0};
}

/* ---- values -------------------------------------------------------------- */

static b8 conf_bool_value(Str v, b8 *out) {
    if (str_eq(v, STR("true")) || str_eq(v, STR("on"))) { *out = true; return true; }
    if (str_eq(v, STR("false")) || str_eq(v, STR("off"))) { *out = false; return true; }
    return false;
}

/* True when `v` is one of the comma separated `options`. */
static b8 conf_option_has(const char *options, Str v) {
    Str all = str_c(options);
    size_t start = 0;
    for (size_t i = 0; i <= all.n; i++) {
        if (i != all.n && all.p[i] != ',') continue;
        if (str_eq(str_trim((Str){ all.p + start, i - start }), v)) return true;
        start = i + 1;
    }
    return false;
}

b8 conf_value_ok(ConfKey k, Str val) {
    if (k >= CONF_N) return false;
    const ConfSpec *sp = &k_conf[k];
    switch (sp->type) {
        case CV_STR:  return val.n <= sp->max_len;
        case CV_ENUM: return conf_option_has(sp->options, val);
        case CV_BOOL: { b8 b; return conf_bool_value(val, &b); }
        case CV_NUM: {
            b8 ok = false;
            i64 n = str_int(val, &ok);
            return ok && n >= sp->lo && n <= sp->hi;
        }
    }
    return false;
}

Str conf_str(const Conf *c, ConfKey k) {
    return k < CONF_N ? c->val[k] : (Str){0};
}

i64 conf_num(const Conf *c, ConfKey k) {
    b8 ok = false;
    i64 n = str_int(conf_str(c, k), &ok);
    return ok ? n : 0;
}

b8 conf_bool(const Conf *c, ConfKey k) {
    b8 out = false;
    conf_bool_value(conf_str(c, k), &out);
    return out;
}

/* ---- resolution ---------------------------------------------------------- */

static Str conf_dup(Arena *a, Str s) {
    return s.n ? str_dup(a, s) : (Str){0};
}

/* One value from one source. `where` names the file for a diagnostic and is
 * empty for the built-in defaults. */
static void conf_take(Conf *c, ConfKey k, Str v, ConfOrigin o, Str where,
                      Arena *persist) {
    const ConfSpec *sp = &k_conf[k];
    if (o == CONF_FROM_PROJECT && !sp->project) {
        agent_log(AGENT_LOG_WARN, "ignoring %s in %.*s: a project file may not "
                 "set it", sp->name, (i32)where.n, where.p);
        return;
    }
    if (!conf_value_ok(k, v)) {
        agent_log(AGENT_LOG_WARN, "ignoring %s in %.*s: %.*s is not a value it "
                 "takes", sp->name, (i32)where.n, where.p, (i32)v.n, v.p);
        return;
    }
    if (c->origin[k] > o) return;
    /* An empty value is "unset", and callers that pass the string to curl or
     * test for a pointer must not see an allocated empty one. */
    Str dup = v.n ? str_dup(persist, v) : (Str){0};
    if (v.n && !dup.p) return;
    c->val[k] = dup;
    c->origin[k] = (u8)o;
}

/* Top-level keys of one settings file. A named section is a provider, which
 * endpoints.c reads. */
static void conf_apply_settings(Conf *c, const Settings *s, ConfOrigin o,
                                Str where, Arena *persist) {
    for (size_t i = 0; i < s->n; i++) {
        if (s->section[i].n) continue;
        ConfKey k = CONF_N;
        for (ConfKey j = 0; j < CONF_N; j++)
            if (str_eq(s->key[i], str_c(k_conf[j].name))) { k = j; break; }
        if (k == CONF_N) {
            /* The state file is arqan's own and may carry a key this build no
             * longer has; a config file is the user's and a typo in it is
             * worth saying out loud. */
            if (o != CONF_FROM_STATE)
                agent_log(AGENT_LOG_WARN, "unknown setting %.*s in %.*s",
                         (i32)s->key[i].n, s->key[i].p,
                         (i32)where.n, where.p);
            continue;
        }
        conf_take(c, k, s->val[i], o, where, persist);
    }
}

static void conf_apply_file(Conf *c, Str path, ConfOrigin o, Arena *persist,
                            Arena *scratch) {
    size_t mark = scratch->off;
    Settings s;
    if (settings_load(&s, path, scratch))
        conf_apply_settings(c, &s, o, path, persist);
    scratch->off = mark;
}

/* AGENT_ENV_PREFIX<NAME>, built from the key's own name so the two cannot
 * drift. */
static void conf_apply_env(Conf *c, Arena *persist) {
    for (ConfKey k = 0; k < CONF_N; k++) {
        char name[64] = AGENT_ENV_PREFIX;
        size_t n = sizeof(AGENT_ENV_PREFIX) - 1, len = strlen(k_conf[k].name);
        if (len + n >= sizeof name) continue;
        for (size_t i = 0; i < len; i++) {
            char ch = k_conf[k].name[i];
            name[n++] = ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : ch;
        }
        name[n] = '\0';
        const char *v = getenv(name);
        if (!v || !*v) continue;
        conf_take(c, k, str_c(v), CONF_FROM_ENV, STR("the environment"),
                  persist);
    }
}

/* The active provider's own settings. They sit above the state file and below
 * the environment: the endpoint is what /provider last configured, while a
 * variable is a statement about this one run. A `provider` naming nothing is
 * cleared, since a name with no endpoint behind it is not a selection. */
static void conf_apply_endpoint(Conf *c, Arena *persist, Arena *scratch) {
    Str name = conf_str(c, CONF_PROVIDER);
    if (!name.n) return;
    size_t mark = scratch->off;
    Endpoints e;
    endpoints_load(&e, scratch);
    size_t i = endpoints_find(&e, name);
    if (i == ENDPOINT_NONE) {
        agent_log(AGENT_LOG_WARN, "no provider named %.*s is configured",
                 (i32)name.n, name.p);
        c->val[CONF_PROVIDER] = (Str){0};
        scratch->off = mark;
        return;
    }
    Str where = STR("the provider's settings");
    conf_take(c, CONF_BASE_URL, e.base_url[i], CONF_FROM_ENDPOINT, where,
              persist);
    conf_take(c, CONF_API, api_name(e.api[i]), CONF_FROM_ENDPOINT, where,
              persist);
    if (e.model[i].n)
        conf_take(c, CONF_MODEL, e.model[i], CONF_FROM_ENDPOINT, where,
                  persist);

    c->reasoning_efforts   = conf_dup(persist, e.reasoning_efforts[i]);
    c->thinking_budgets    = conf_dup(persist, e.thinking_budgets[i]);
    c->reasoning_effort    = conf_dup(persist, e.reasoning_effort[i]);
    c->thinking_budget     = conf_dup(persist, e.thinking_budget[i]);
    c->reasoning_template  = conf_dup(persist, e.reasoning_template[i]);
    scratch->off = mark;

    /* The key is kept apart from the settings, in the credentials file. */
    char err[AGENT_MAX_PATH + 96] = {0};
    Str key = endpoints_key(name, persist, scratch, err, sizeof err);
    if (err[0]) agent_log(AGENT_LOG_WARN, "%s", err);
    if (key.n)
        conf_take(c, CONF_API_KEY, key, CONF_FROM_ENDPOINT, where, persist);
}

void conf_resolve(Conf *c, Arena *persist, Arena *scratch) {
    memset(c, 0, sizeof *c);
    for (ConfKey k = 0; k < CONF_N; k++)
        conf_take(c, k, str_c(k_conf[k].dflt), CONF_FROM_DEFAULT,
                  STR("the defaults"), persist);

    size_t mark = scratch->off;
    Str user = paths_file(AGENT_DIR_CONFIG, AGENT_CONFIG_NAME, scratch);
    Str files[AGENT_MAX_CONFIG_FILES];
    size_t n = paths_config_files(AGENT_CONFIG_NAME, scratch, files,
                                  AGENT_MAX_CONFIG_FILES);
    for (size_t i = 0; i < n; i++)
        conf_apply_file(c, files[i], str_eq(files[i], user) ? CONF_FROM_USER
                                                            : CONF_FROM_SYSTEM,
                        persist, scratch);

    Str project[AGENT_MAX_PROJECT_FILES];
    n = paths_project_files(AGENT_CONFIG_NAME, scratch, project,
                            AGENT_MAX_PROJECT_FILES);
    for (size_t i = 0; i < n; i++)
        conf_apply_file(c, project[i], CONF_FROM_PROJECT, persist, scratch);

    Str state = paths_file(AGENT_DIR_STATE, AGENT_STATE_NAME, scratch);
    if (state.n) conf_apply_file(c, state, CONF_FROM_STATE, persist, scratch);
    scratch->off = mark;

    conf_apply_env(c, persist);
    conf_apply_endpoint(c, persist, scratch);
}

b8 conf_remember(ConfKey k, Str val, Arena *scratch) {
    if (k >= CONF_N || !conf_value_ok(k, val)) return false;
    return state_set(conf_key_name(k), val, scratch);
}

b8 conf_remember_bool(ConfKey k, b8 on, Arena *scratch) {
    return conf_remember(k, on ? STR("true") : STR("false"), scratch);
}

void ui_prefs_load(UiPrefs *p, const Conf *conf) {
    memset(p, 0, sizeof *p);
    p->verbose_tools     = conf_bool(conf, CONF_VERBOSE_TOOLS);
    p->raw_markdown      = conf_bool(conf, CONF_RAW_MARKDOWN);
    p->show_ignored      = conf_bool(conf, CONF_SHOW_IGNORED);
    p->show_instructions = conf_bool(conf, CONF_SHOW_INSTRUCTIONS);
    p->telemetry         = conf_bool(conf, CONF_TELEMETRY);
    p->justify           = str_eq(conf_str(conf, CONF_WRAP),
                                  STR("justified"));
    p->status_fields     = (u64)conf_num(conf, CONF_STATUS_FIELDS);
}

/* ---- Config -------------------------------------------------------------- */

b8 config_remember_model(Str model, Arena *scratch) {
    return conf_remember(CONF_MODEL, model, scratch);
}

static Str config_owned(char *dst, size_t cap, Str src) {
    if (src.n >= cap) return (Str){0};
    if (src.n) memmove(dst, src.p, src.n);
    dst[src.n] = '\0';
    return (Str){ dst, src.n };
}

b8 config_set_model(Config *c, Str model) {
    if (!model.n || model.n > AGENT_MAX_MODEL_NAME) return false;
    Str saved = config_owned(c->owned_model, sizeof c->owned_model, model);
    if (!saved.p) return false;
    c->model = saved;
    return true;
}

b8 config_set_endpoint(Config *c, Str name, Str base_url, Str model,
                       ApiKind api, Str key, Str efforts, Str budgets,
                       Str effort, Str budget, Str templ) {
    if (!name.n || name.n > AGENT_MAX_ENDPOINT_NAME
        || !base_url.n || base_url.n > AGENT_MAX_URL
        || model.n > AGENT_MAX_MODEL_NAME || key.n > AGENT_MAX_API_KEY
        || efforts.n > AGENT_MAX_REASONING_LIST
        || budgets.n > AGENT_MAX_REASONING_LIST
        || effort.n > AGENT_MAX_REASONING_LIST
        || budget.n > AGENT_MAX_REASONING_LIST
        || templ.n > AGENT_MAX_REASONING_TEMPLATE)
        return false;

    Str saved_name = config_owned(c->owned_provider,
                                  sizeof c->owned_provider, name);
    Str saved_url = config_owned(c->owned_base_url,
                                 sizeof c->owned_base_url, base_url);
    Str saved_key = config_owned(c->owned_api_key,
                                 sizeof c->owned_api_key, key);
    Str saved_efforts = config_owned(c->owned_reasoning_efforts,
                                     sizeof c->owned_reasoning_efforts,
                                     efforts);
    Str saved_budgets = config_owned(c->owned_thinking_budgets,
                                     sizeof c->owned_thinking_budgets,
                                     budgets);
    Str saved_effort = config_owned(c->owned_reasoning_effort,
                                    sizeof c->owned_reasoning_effort, effort);
    Str saved_budget = config_owned(c->owned_thinking_budget,
                                    sizeof c->owned_thinking_budget, budget);
    Str saved_templ = config_owned(c->owned_reasoning_template,
                                   sizeof c->owned_reasoning_template, templ);
    if (!saved_name.p || !saved_url.p || !saved_key.p || !saved_efforts.p
        || !saved_budgets.p || !saved_effort.p || !saved_budget.p
        || !saved_templ.p)
        return false;
    if (model.n && !config_set_model(c, model)) return false;

    c->provider = saved_name;
    c->base_url = saved_url;
    c->base_url_set = true;
    c->api = api;
    c->api_key = key.n ? saved_key : (Str){0};
    c->reasoning_efforts = efforts.n ? saved_efforts : (Str){0};
    c->thinking_budgets = budgets.n ? saved_budgets : (Str){0};
    c->reasoning_effort = effort.n ? saved_effort : (Str){0};
    c->thinking_budget = budget.n ? saved_budget : (Str){0};
    c->reasoning_template = templ.n ? saved_templ : (Str){0};
    return true;
}

b8 config_set_reasoning(Config *c, b8 effort, Str value) {
    char *dst = effort ? c->owned_reasoning_effort
                       : c->owned_thinking_budget;
    size_t cap = effort ? sizeof c->owned_reasoning_effort
                        : sizeof c->owned_thinking_budget;
    Str saved = config_owned(dst, cap, value);
    if (!saved.p) return false;
    if (effort) c->reasoning_effort = value.n ? saved : (Str){0};
    else c->thinking_budget = value.n ? saved : (Str){0};
    return true;
}

b8 config_load(Config *c, const Conf *conf, Arena *persist) {
    memset(c, 0, sizeof *c);

    c->provider = conf_str(conf, CONF_PROVIDER);
    c->base_url = conf_str(conf, CONF_BASE_URL);
    c->base_url_set = c->base_url.n != 0;
    c->model    = conf_str(conf, CONF_MODEL);
    c->api_key  = conf_str(conf, CONF_API_KEY);
    c->api = str_eq(conf_str(conf, CONF_API), STR("anthropic"))
           ? API_ANTHROPIC : API_OPENAI;
    c->mode = str_eq(conf_str(conf, CONF_MODE), STR("plan"))
            ? MODE_PLAN : MODE_BUILD;
    c->stream         = conf_bool(conf, CONF_STREAM);
    c->max_tokens     = (i32)conf_num(conf, CONF_MAX_TOKENS);
    c->max_messages   = (size_t)conf_num(conf, CONF_MAX_MESSAGES);
    c->retries        = (i32)conf_num(conf, CONF_RETRIES);
    c->retry_delay_ms = (i32)conf_num(conf, CONF_RETRY_DELAY_MS);

    /* "none" is how the UI records that nothing is disabled: an empty value
     * removes the key, which the next run would read as never chosen. */
    Str tools = conf_str(conf, CONF_DISABLE_TOOLS);
    c->disable_tools = str_eq(tools, STR("none")) ? (Str){0} : tools;

    c->reasoning_efforts   = conf->reasoning_efforts;
    c->thinking_budgets    = conf->thinking_budgets;
    c->reasoning_effort    = conf->reasoning_effort;
    c->thinking_budget     = conf->thinking_budget;
    c->reasoning_template  = conf->reasoning_template;

    /* A prompt is a document rather than a setting, so it has no row in the
     * table; the variable and --system are the only ways to pass one. */
    const char *sys = getenv(AGENT_ENV_PREFIX "SYSTEM_PROMPT");
    if (sys && *sys) c->system_prompt = str_dup(persist, str_c(sys));

    /* A placeholder rather than a destination: a run that named no endpoint
     * asks for one. It follows the API so the pair is at least coherent. */
    if (!c->base_url.n)
        c->base_url = c->api == API_ANTHROPIC
                    ? str_c("https://api.anthropic.com/v1")
                    : str_c("https://api.openai.com/v1");
    if (!c->model.n)
        c->model = c->api == API_ANTHROPIC ? str_c("claude-sonnet-4-5")
                                           : str_c("gpt-4o-mini");
    return true;
}
