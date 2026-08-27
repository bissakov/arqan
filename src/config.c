#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { CV_STR, CV_NUM, CV_BOOL, CV_ENUM } ConfType;

typedef struct {
    const char *name;
    const char *dflt;
    const char *options;
    ConfType type;
    i64 lo, hi;
    size_t max_len;
    b8 project;
} ConfSpec;

#define CONF_TEXT2(x) #x
#define CONF_TEXT(x)  CONF_TEXT2(x)
/* The status field mask has one bit per field, so its default is all of
 * them; a literal is needed because the table's defaults are text. */
_Static_assert(AGENT_STATUS_FIELDS == 11, "the status_fields default is 2047");

static const ConfSpec k_conf[CONF_N] = {

    [CONF_PROVIDER] = {"provider", "", NULL, CV_STR, 0, 0,
                       AGENT_MAX_ENDPOINT_NAME, true},
    [CONF_BASE_URL] = {"base_url", "", NULL, CV_STR, 0, 0, AGENT_MAX_URL, true},
    [CONF_MODEL] = {"model", "", NULL, CV_STR, 0, 0, AGENT_MAX_MODEL_NAME,
                    true},
    [CONF_API] = {"api", "openai", "openai,anthropic", CV_ENUM, 0, 0, 0, true},
    /* Never from a project file: a repository must not be able to hand a
     * key to the endpoint it also names. */
    [CONF_API_KEY] = {"api_key", "", NULL, CV_STR, 0, 0, AGENT_MAX_API_KEY,
                      false},
    [CONF_MAX_TOKENS] = {"max_tokens", CONF_TEXT(AGENT_MAX_TOKENS), NULL,
                         CV_NUM, 1, 1 << 20, 0, true},
    [CONF_MAX_MESSAGES] = {"max_messages", CONF_TEXT(AGENT_MAX_MESSAGES), NULL,
                           CV_NUM, 8, 1 << 20, 0, true},
    [CONF_STREAM] = {"stream", "true", NULL, CV_BOOL, 0, 0, 0, true},
    [CONF_MODE] = {"mode", "build", "build,plan", CV_ENUM, 0, 0, 0, true},
    /* Never from a project file: a repository must not be able to turn off
     * approval for commands or changes it may induce the model to request. */
    [CONF_PERMISSIONS] = {"permissions", "ask", "ask,free", CV_ENUM, 0, 0, 0,
                          false},
    [CONF_RETRIES] = {"retries", CONF_TEXT(AGENT_RETRIES), NULL, CV_NUM, 0, 16,
                      0, true},
    [CONF_RETRY_DELAY_MS] = {"retry_delay_ms", CONF_TEXT(AGENT_RETRY_DELAY_MS),
                             NULL, CV_NUM, 0, AGENT_MAX_RETRY_DELAY_MS, 0,
                             true},
    /* "none" rather than an empty value: an empty value removes the key,
     * which would read as "nothing was ever chosen" on the next run. */
    [CONF_DISABLE_TOOLS] = {"disable_tools", "", NULL, CV_STR, 0, 0,
                            AGENT_MAX_TOOL_LIST, true},
    [CONF_VERBOSE_TOOLS] = {"verbose_tools", "false", NULL, CV_BOOL, 0, 0, 0,
                            true},
    [CONF_RAW_MARKDOWN] = {"raw_markdown", "false", NULL, CV_BOOL, 0, 0, 0,
                           true},
    [CONF_SHOW_IGNORED] = {"show_ignored", "false", NULL, CV_BOOL, 0, 0, 0,
                           true},
    [CONF_SHOW_INSTRUCTIONS] = {"show_instructions", "false", NULL, CV_BOOL, 0,
                                0, 0, true},
    [CONF_WRAP] = {"wrap", "word", "word,justified", CV_ENUM, 0, 0, 0, true},
    [CONF_STATUS_FIELDS] = {"status_fields", "2047", NULL, CV_NUM, 0, 2047, 0,
                            true},

    [CONF_TELEMETRY] = {"telemetry", "false", NULL, CV_BOOL, 0, 0, 0, false},
    [CONF_NOTIFY] = {"notify", "osc9", "off,bel,osc9,both", CV_ENUM, 0, 0, 0,
                     true},
    /* Never from a project file: it names a program arqan will run, so a
     * `git clone` must not be able to choose it. */
    [CONF_NOTIFY_COMMAND] = {"notify_command", "", NULL, CV_STR, 0, 0,
                             AGENT_MAX_NOTIFY_CMD, false},

    [CONF_NOTIFY_MIN_MS] = {"notify_min_ms", "10000", NULL, CV_NUM, 0,
                            24 * 60 * 60 * 1000, 0, true},

    [CONF_SEARCH_BACKEND] = {"search_backend", "auto",
                             "auto,ddg,brave,brave_api,google,searxng", CV_ENUM,
                             0, 0, 0, true},
    /* Never from a project file: these three name where a search goes and
     * what it pays with, so a `git clone` must not be able to choose them. */
    [CONF_SEARCH_ENDPOINT] = {"search_endpoint", "", NULL, CV_STR, 0, 0,
                              AGENT_MAX_URL, false},
    [CONF_SEARCH_API_KEY] = {"search_api_key", "", NULL, CV_STR, 0, 0,
                             AGENT_MAX_API_KEY, false},
    [CONF_SEARCH_ENGINE_ID] = {"search_engine_id", "", NULL, CV_STR, 0, 0,
                               AGENT_MAX_ENDPOINT_NAME, false},

    [CONF_SMALL_MODEL] = {"small_model", "", NULL, CV_STR, 0, 0,
                          AGENT_MAX_MODEL_NAME, true},

    [CONF_SMALL_PROVIDER] = {"small_provider", "", NULL, CV_STR, 0, 0,
                             AGENT_MAX_ENDPOINT_NAME, false},

    [CONF_AUTO_TITLE] = {"auto_title", "true", NULL, CV_BOOL, 0, 0, 0, true},
    /* Never from a project file: a repository must not be able to shorten
     * the wait until the agent answers its own questions. 0 waits for the
     * user however long they take. */
    [CONF_ASK_TIMEOUT_MS] = {"ask_timeout_ms", CONF_TEXT(AGENT_ASK_TIMEOUT_MS),
                             NULL, CV_NUM, 0, 24 * 60 * 60 * 1000, 0, false},
    /* Never from a project file: a repository must not be able to hold a turn
     * open past a prompt cache by making its build undetachable. The ceiling
     * is a job's longest wait, since both are time the model spends idle; a
     * deadline outside the cache window would defeat the point of having one.
     * 0 is the way to say the wait matters more than the cache. */
    [CONF_SHELL_TIMEOUT_MS] = {"shell_timeout_ms",
                               CONF_TEXT(AGENT_SHELL_TIMEOUT_MS), NULL, CV_NUM,
                               0, AGENT_JOB_WAIT_MAX_MS, 0, false},
    /* Whether a message may carry an image. "auto" offers /attach and is
     * named that way because what a model accepts is the model's to say: a
     * later release may decide it from the model rather than from here.
     * Never from a project file: a repository must not be able to put image
     * bytes into a request for a connection the user gave none. */
    [CONF_IMAGES] = {"images", "auto", "auto,off", CV_ENUM, 0, 0, 0, false},
    /* Off by default: a start that reopens the last conversation is a choice
     * the user makes, not what a first run does. */
    [CONF_RESUME_LAST] = {"resume_last", "false", NULL, CV_BOOL, 0, 0, 0, true},
    /* Automatic by default, which costs nothing until a model profile
     * declares a window: without one there is no percentage to be past. */
    [CONF_COMPACT] = {"compact", "auto", "off,manual,auto", CV_ENUM, 0, 0, 0,
                      true},
    /* The ceiling leaves the reply that discovers the threshold somewhere to
     * land. */
    [CONF_COMPACT_AT] = {"compact_at", CONF_TEXT(AGENT_COMPACT_AT), NULL,
                         CV_NUM, 50, 95, 0, true},
    /* Where the elision boundary is allowed to advance. It has to stay below
     * compact_at: eliding is what buys room before a compaction is due, and
     * a threshold at or past that one would only pay for both rewrites in
     * the same turn. */
    [CONF_ELIDE_AT] = {"elide_at", CONF_TEXT(AGENT_ELIDE_AT), NULL, CV_NUM, 20,
                       94, 0, true},
    [CONF_COMPACT_MODEL] = {"compact_model", "main", "main,small", CV_ENUM, 0,
                            0, 0, true},
    /* What an unexplained cache rebuild does. Never from a project file: a
     * repository must not be able to silence a defect that spends the user's
     * tokens rebuilding the same prefix round after round. */
    [CONF_CACHE_GUARD] = {"cache_guard", "stop", "stop,warn,off", CV_ENUM, 0, 0,
                          0, false},
    [CONF_SUBAGENTS] = {"subagents", "true", NULL, CV_BOOL, 0, 0, 0, true},
    [CONF_SUBAGENT_MODEL] = {"subagent_model", "main", "main,small", CV_ENUM, 0,
                             0, 0, true},
    /* Never from a project file, for the reason shell_timeout_ms carries: a
     * repository must not be able to hold a turn open past a prompt cache,
     * here by asking for a slice longer than the window. The ceiling is a
     * job's longest wait, since both are time the model spends idle. 0 runs
     * the subagent to completion and accepts the rebuild. */
    [CONF_SUBAGENT_SLICE_MS] = {"subagent_slice_ms",
                                CONF_TEXT(AGENT_TASK_SLICE_MS), NULL, CV_NUM, 0,
                                AGENT_JOB_WAIT_MAX_MS, 0, false},
};

Str conf_key_name(ConfKey k) {
    return k < CONF_N ? str_c(k_conf[k].name) : (Str){0};
}


static b8 conf_bool_value(Str v, b8 *out) {
    if (str_eq(v, STR("true")) || str_eq(v, STR("on"))) {
        *out = true;
        return true;
    }
    if (str_eq(v, STR("false")) || str_eq(v, STR("off"))) {
        *out = false;
        return true;
    }
    return false;
}

static b8 conf_option_has(const char *options, Str v) {
    Str all = str_c(options);
    size_t start = 0;
    for (size_t i = 0; i <= all.n; i++) {
        if (i != all.n && all.p[i] != ',') continue;
        if (str_eq(str_trim((Str){all.p + start, i - start}), v)) return true;
        start = i + 1;
    }
    return false;
}

b8 conf_value_ok(ConfKey k, Str val) {
    if (k >= CONF_N) return false;
    const ConfSpec *sp = &k_conf[k];
    switch (sp->type) {
        case CV_STR: return val.n <= sp->max_len;
        case CV_ENUM: return conf_option_has(sp->options, val);
        case CV_BOOL: {
            b8 b;
            return conf_bool_value(val, &b);
        }
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


static void conf_take(Conf *c, ConfKey k, Str v, ConfOrigin o, Str where,
                      Arena *persist) {
    const ConfSpec *sp = &k_conf[k];
    if (o == CONF_FROM_PROJECT && !sp->project) {
        agent_log(AGENT_LOG_WARN,
                  "ignoring %s in %.*s: a project file may not "
                  "set it",
                  sp->name, (i32)where.n, where.p);
        return;
    }
    if (!conf_value_ok(k, v)) {
        agent_log(AGENT_LOG_WARN,
                  "ignoring %s in %.*s: %.*s is not a value it "
                  "takes",
                  sp->name, (i32)where.n, where.p, (i32)v.n, v.p);
        return;
    }
    if (c->origin[k] > o) return;
    /* An empty value is "unset", and callers that pass the string to curl or
     * test for a pointer must not see an allocated empty one. */
    Str dup = str_dup_opt(persist, v);
    if (v.n && !dup.p) return;
    c->val[k] = dup;
    c->origin[k] = (u8)o;
}


static void conf_apply_settings(Conf *c, const Settings *s, ConfOrigin o,
                                Str where, Arena *persist) {
    for (size_t i = 0; i < s->n; i++) {
        if (s->section[i].n) continue;
        ConfKey k = CONF_N;
        for (ConfKey j = 0; j < CONF_N; j++)
            if (str_eq(s->key[i], str_c(k_conf[j].name))) {
                k = j;
                break;
            }
        if (k == CONF_N) {
            if (o != CONF_FROM_STATE)
                agent_log(AGENT_LOG_WARN, "unknown setting %.*s in %.*s",
                          (i32)s->key[i].n, s->key[i].p, (i32)where.n, where.p);
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

/* The connection of the provider serving the chosen model. It sits above the
 * state file and below the environment: the endpoint is what /provider
 * configured, while a variable is a statement about this one run. A `provider`
 * naming nothing is cleared, since a name with no endpoint behind it is not a
 * selection. */
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

    if (e.model[i].n && c->origin[CONF_MODEL] == CONF_FROM_DEFAULT)
        conf_take(c, CONF_MODEL, e.model[i], CONF_FROM_ENDPOINT, where,
                  persist);

    if (e.small_model[i].n && c->origin[CONF_SMALL_MODEL] < CONF_FROM_STATE)
        conf_take(c, CONF_SMALL_MODEL, e.small_model[i], CONF_FROM_ENDPOINT,
                  where, persist);

    model_profile_load(&c->model_profile, name, conf_str(c, CONF_MODEL),
                       persist, scratch);
    scratch->off = mark;

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
        conf_apply_file(c, files[i],
                        str_eq(files[i], user) ? CONF_FROM_USER
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

b8 conf_remember_pair(ConfKey a, Str va, ConfKey b, Str vb, Arena *scratch) {
    if (a >= CONF_N || b >= CONF_N) return false;
    if (!conf_value_ok(a, va) || !conf_value_ok(b, vb)) return false;
    Str keys[2] = {conf_key_name(a), conf_key_name(b)};
    Str vals[2] = {va, vb};
    return state_set_many(keys, vals, 2, scratch);
}

b8 conf_remember_bool(ConfKey k, b8 on, Arena *scratch) {
    return conf_remember(k, on ? STR("true") : STR("false"), scratch);
}

void ui_prefs_load(UiPrefs *p, const Conf *conf) {
    memset(p, 0, sizeof *p);
    p->verbose_tools = conf_bool(conf, CONF_VERBOSE_TOOLS);
    p->raw_markdown = conf_bool(conf, CONF_RAW_MARKDOWN);
    p->show_ignored = conf_bool(conf, CONF_SHOW_IGNORED);
    p->show_instructions = conf_bool(conf, CONF_SHOW_INSTRUCTIONS);
    p->telemetry = conf_bool(conf, CONF_TELEMETRY);
    p->justify = str_eq(conf_str(conf, CONF_WRAP), STR("justified"));
    p->status_fields = (u64)conf_num(conf, CONF_STATUS_FIELDS);
}


b8 config_remember_model(Str provider, Str model, Arena *scratch) {
    return conf_remember_pair(CONF_PROVIDER, provider, CONF_MODEL, model,
                              scratch);
}

static Str config_owned(char *dst, size_t cap, Str src) {
    if (src.n >= cap) return (Str){0};
    if (src.n) memmove(dst, src.p, src.n);
    dst[src.n] = '\0';
    return (Str){dst, src.n};
}

b8 config_set_model(Config *c, Str model) {
    if (!model.n || model.n > AGENT_MAX_MODEL_NAME) return false;
    Str saved = config_owned(c->owned_model, sizeof c->owned_model, model);
    if (!saved.p) return false;
    c->model = saved;
    return true;
}

b8 config_set_small_model(Config *c, Str model, Str provider) {
    if (model.n > AGENT_MAX_MODEL_NAME || provider.n > AGENT_MAX_ENDPOINT_NAME)
        return false;
    Str saved =
        config_owned(c->owned_small_model, sizeof c->owned_small_model, model);
    Str owner = config_owned(c->owned_small_provider,
                             sizeof c->owned_small_provider, provider);
    if (!saved.p || !owner.p) return false;
    c->small_model = model.n ? saved : (Str){0};
    c->small_provider = model.n && provider.n ? owner : (Str){0};
    return true;
}

b8 config_set_connection(Config *c, Str name, Str base_url, ApiKind api,
                         Str key) {
    if (name.n > AGENT_MAX_ENDPOINT_NAME || !base_url.n
        || base_url.n > AGENT_MAX_URL || key.n > AGENT_MAX_API_KEY)
        return false;
    Str saved_name =
        config_owned(c->owned_provider, sizeof c->owned_provider, name);
    Str saved_url =
        config_owned(c->owned_base_url, sizeof c->owned_base_url, base_url);
    Str saved_key =
        config_owned(c->owned_api_key, sizeof c->owned_api_key, key);
    if (!saved_name.p || !saved_url.p || !saved_key.p) return false;

    c->provider = name.n ? saved_name : (Str){0};
    c->base_url = saved_url;
    c->base_url_set = true;
    c->api = api;
    c->api_key = key.n ? saved_key : (Str){0};
    return true;
}

b8 config_set_endpoint(Config *c, Str name, Str base_url, Str model,
                       ApiKind api, Str key) {
    if (!name.n || model.n > AGENT_MAX_MODEL_NAME) return false;
    if (!config_set_connection(c, name, base_url, api, key)) return false;
    return !model.n || config_set_model(c, model);
}

b8 config_set_model_profile(Config *c, const ModelProfile *p) {
    if (!c || !p || p->reasoning_efforts.n > AGENT_MAX_REASONING_LIST
        || p->thinking_budgets.n > AGENT_MAX_REASONING_LIST
        || p->reasoning_effort.n > AGENT_MAX_REASONING_LIST
        || p->thinking_budget.n > AGENT_MAX_REASONING_LIST
        || p->reasoning_template.n > AGENT_MAX_REASONING_TEMPLATE
        || p->context_window > AGENT_MAX_CONTEXT_WINDOW)
        return false;
    Str efforts =
        config_owned(c->owned_reasoning_efforts,
                     sizeof c->owned_reasoning_efforts, p->reasoning_efforts);
    Str budgets =
        config_owned(c->owned_thinking_budgets,
                     sizeof c->owned_thinking_budgets, p->thinking_budgets);
    Str effort =
        config_owned(c->owned_reasoning_effort,
                     sizeof c->owned_reasoning_effort, p->reasoning_effort);
    Str budget =
        config_owned(c->owned_thinking_budget, sizeof c->owned_thinking_budget,
                     p->thinking_budget);
    Str templ =
        config_owned(c->owned_reasoning_template,
                     sizeof c->owned_reasoning_template, p->reasoning_template);
    if (!efforts.p || !budgets.p || !effort.p || !budget.p || !templ.p)
        return false;
    c->reasoning_efforts = p->reasoning_efforts.n ? efforts : (Str){0};
    c->thinking_budgets = p->thinking_budgets.n ? budgets : (Str){0};
    c->reasoning_effort = p->reasoning_effort.n ? effort : (Str){0};
    c->thinking_budget = p->thinking_budget.n ? budget : (Str){0};
    c->reasoning_template = p->reasoning_template.n ? templ : (Str){0};
    c->context_window = p->context_window;
    return true;
}

b8 config_set_reasoning(Config *c, b8 effort, Str value) {
    char *dst = effort ? c->owned_reasoning_effort : c->owned_thinking_budget;
    size_t cap = effort ? sizeof c->owned_reasoning_effort
                        : sizeof c->owned_thinking_budget;
    Str saved = config_owned(dst, cap, value);
    if (!saved.p) return false;
    if (effort)
        c->reasoning_effort = value.n ? saved : (Str){0};
    else
        c->thinking_budget = value.n ? saved : (Str){0};
    return true;
}

b8 config_load(Config *c, const Conf *conf, Arena *persist) {
    memset(c, 0, sizeof *c);

    c->provider = conf_str(conf, CONF_PROVIDER);
    c->base_url = conf_str(conf, CONF_BASE_URL);
    c->base_url_set = c->base_url.n != 0;
    c->model = conf_str(conf, CONF_MODEL);
    c->small_model = conf_str(conf, CONF_SMALL_MODEL);

    c->small_provider =
        c->small_model.n ? conf_str(conf, CONF_SMALL_PROVIDER) : (Str){0};
    c->api_key = conf_str(conf, CONF_API_KEY);
    c->api = str_eq(conf_str(conf, CONF_API), STR("anthropic")) ? API_ANTHROPIC
                                                                : API_OPENAI;
    c->mode =
        str_eq(conf_str(conf, CONF_MODE), STR("plan")) ? MODE_PLAN : MODE_BUILD;
    c->permissions = str_eq(conf_str(conf, CONF_PERMISSIONS), STR("free"))
                         ? PERMISSION_FREE
                         : PERMISSION_ASK;
    c->stream = conf_bool(conf, CONF_STREAM);
    c->max_tokens = (i32)conf_num(conf, CONF_MAX_TOKENS);
    c->max_messages = (size_t)conf_num(conf, CONF_MAX_MESSAGES);
    c->retries = (i32)conf_num(conf, CONF_RETRIES);
    c->retry_delay_ms = (i32)conf_num(conf, CONF_RETRY_DELAY_MS);
    c->auto_title = conf_bool(conf, CONF_AUTO_TITLE);
    c->ask_timeout_ms = (i32)conf_num(conf, CONF_ASK_TIMEOUT_MS);
    c->shell_timeout_ms = (i32)conf_num(conf, CONF_SHELL_TIMEOUT_MS);
    c->images = !str_eq(conf_str(conf, CONF_IMAGES), STR("off"));
    c->resume_last = conf_bool(conf, CONF_RESUME_LAST);
    Str compact = conf_str(conf, CONF_COMPACT);
    c->compact = str_eq(compact, STR("off"))      ? COMPACT_OFF
                 : str_eq(compact, STR("manual")) ? COMPACT_MANUAL
                                                  : COMPACT_AUTO;
    c->compact_at = (u32)conf_num(conf, CONF_COMPACT_AT);
    c->elide_at = (u32)conf_num(conf, CONF_ELIDE_AT);
    /* Refused rather than moved: a value the two thresholds cannot both hold
     * is a statement about a policy that does not exist, and quietly sliding
     * it under compact_at would elide at a percentage nobody chose. Eliding
     * stays off until one of the two is changed. */
    if (c->elide_at >= c->compact_at) {
        agent_log(AGENT_LOG_WARN,
                  "ignoring elide_at %u: it must be below compact_at %u",
                  c->elide_at, c->compact_at);
        c->elide_at = 0;
    }
    c->compact_small = str_eq(conf_str(conf, CONF_COMPACT_MODEL), STR("small"));
    c->subagents = conf_bool(conf, CONF_SUBAGENTS);
    c->subagent_small =
        str_eq(conf_str(conf, CONF_SUBAGENT_MODEL), STR("small"));
    c->subagent_slice_ms = (i32)conf_num(conf, CONF_SUBAGENT_SLICE_MS);
    Str guard = conf_str(conf, CONF_CACHE_GUARD);
    c->cache_guard = str_eq(guard, STR("off"))    ? CACHE_GUARD_OFF
                     : str_eq(guard, STR("warn")) ? CACHE_GUARD_WARN
                                                  : CACHE_GUARD_STOP;


    Str tools = conf_str(conf, CONF_DISABLE_TOOLS);
    c->disable_tools = str_eq(tools, STR("none")) ? (Str){0} : tools;

    c->reasoning_efforts = conf->model_profile.reasoning_efforts;
    c->thinking_budgets = conf->model_profile.thinking_budgets;
    c->reasoning_effort = conf->model_profile.reasoning_effort;
    c->thinking_budget = conf->model_profile.thinking_budget;
    c->reasoning_template = conf->model_profile.reasoning_template;
    c->context_window = conf->model_profile.context_window;

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
