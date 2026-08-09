/* config.c: load Config from the environment and the settings files.
 *
 * Keys, all at the head of the config file: base_url, model, api_key, api,
 * max_tokens, max_messages, stream, mode, retries, retry_delay_ms,
 * disable_tools. A "[provider
 * <name>]" section of the same file is an endpoint (see endpoints.c).
 * The system prompt is not a key here: it is a document, so it lives in
 * SYSTEM.md (see prompt.c).
 * Precedence: CLI > env var YOKE_<KEY> > remembered UI state > the active
 * provider > $XDG_CONFIG_HOME/yoke/config > the same file in each
 * $XDG_CONFIG_DIRS entry. See paths.c for the directories.
 */
#include "yoke.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Str env_str(Arena *a, const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return (Str){0};
    return str_dup(a, str_c(v));
}

/* Bounds every numeric setting: a config file or environment is not a
 * trusted source of array capacities. */
static size_t clamp_size(i64 v, size_t lo, size_t hi) {
    if (v < 0 || (u64)v < lo) return lo;
    if ((u64)v > hi) return hi;
    return (size_t)v;
}

/* False when the variable is unset or is not a number, which leaves the
 * setting as whatever set it before. */
static b8 env_num(const char *name, size_t lo, size_t hi, size_t *out) {
    const char *v = getenv(name);
    if (!v || !*v) return false;
    b8 ok = false;
    i64 n = str_int(str_c(v), &ok);
    if (!ok) return false;
    *out = clamp_size(n, lo, hi);
    return true;
}

/* Copies `src` into `persist` over `*dst` unless it is empty or `blocked` by
 * a higher-precedence source. True when it landed, which is what tells
 * base_url it has one. */
static b8 config_str(Arena *persist, Str src, b8 blocked, Str *dst) {
    if (!src.n || blocked) return false;
    Str v = str_dup(persist, src);
    if (!v.p) return false;
    *dst = v;
    return true;
}

/* Keys the environment already set: no config file may override them. */
typedef struct { b8 base, model, key, api, msgs, tools; } EnvSet;

/* Every key read here is the file's top-level one: a named section is a
 * provider, which endpoints.c reads. */
static Str top_key(const Settings *s, Str key) {
    return settings_get(s, (Str){0}, key);
}

static b8 config_num(const Settings *s, Str key, size_t lo, size_t hi,
                     size_t *out) {
    Str v = top_key(s, key);
    if (!v.n) return false;
    b8 ok = false;
    i64 m = str_int(v, &ok);
    if (!ok) return false;
    *out = clamp_size(m, lo, hi);
    return true;
}

static void config_apply_file(Config *c, Str path, EnvSet env,
                              Arena *persist, Arena *scratch) {
    size_t mark = scratch->off;
    Settings s;
    if (!settings_load(&s, path, scratch)) { scratch->off = mark; return; }

    if (config_str(persist, top_key(&s, STR("base_url")), env.base,
                   &c->base_url))
        c->base_url_set = true;
    config_str(persist, top_key(&s, STR("model")), env.model, &c->model);
    config_str(persist, top_key(&s, STR("api_key")), env.key, &c->api_key);
    config_str(persist, top_key(&s, STR("disable_tools")), env.tools,
               &c->disable_tools);
    Str api = top_key(&s, STR("api"));
    if (api.n && !env.api) c->api = api_from_str(api);

    size_t n;
    if (config_num(&s, STR("max_tokens"), 1, 1u << 20, &n))
        c->max_tokens = (i32)n;
    if (!env.msgs && config_num(&s, STR("max_messages"), 8, 1u << 20, &n))
        c->max_messages = n;
    if (config_num(&s, STR("retries"), 0, 16, &n))
        c->retries = (i32)n;
    if (config_num(&s, STR("retry_delay_ms"), 0, YOKE_MAX_RETRY_DELAY_MS, &n))
        c->retry_delay_ms = (i32)n;
    Str stream = top_key(&s, STR("stream"));
    if (stream.n) c->stream = !str_eq(stream, STR("false"));
    Str mode = top_key(&s, STR("mode"));
    if (str_eq(mode, STR("plan"))) c->mode = MODE_PLAN;
    else if (str_eq(mode, STR("build"))) c->mode = MODE_BUILD;

    scratch->off = mark;
}

b8 config_remember_model(Str model, Arena *scratch) {
    if (!model.n || model.n >= 256) return false;
    return state_set(STR("model"), model, scratch);
}

static Str config_owned(char *dst, size_t cap, Str src) {
    if (src.n >= cap) return (Str){0};
    if (src.n) memmove(dst, src.p, src.n);
    dst[src.n] = '\0';
    return (Str){ dst, src.n };
}

b8 config_set_model(Config *c, Str model) {
    if (!model.n || model.n > YOKE_MAX_MODEL_NAME) return false;
    Str saved = config_owned(c->owned_model, sizeof c->owned_model, model);
    if (!saved.p) return false;
    c->model = saved;
    return true;
}

b8 config_set_endpoint(Config *c, Str name, Str base_url, Str model,
                       ApiKind api, Str key, Str efforts, Str budgets,
                       Str effort, Str budget, Str templ) {
    if (!name.n || name.n > YOKE_MAX_ENDPOINT_NAME
        || !base_url.n || base_url.n > YOKE_MAX_URL
        || model.n > YOKE_MAX_MODEL_NAME || key.n > YOKE_MAX_API_KEY
        || efforts.n > YOKE_MAX_REASONING_LIST
        || budgets.n > YOKE_MAX_REASONING_LIST
        || effort.n > YOKE_MAX_REASONING_LIST
        || budget.n > YOKE_MAX_REASONING_LIST
        || templ.n > YOKE_MAX_REASONING_TEMPLATE)
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

b8 config_load(Config *c, Arena *persist, Arena *scratch) {
    memset(c, 0, sizeof *c);
    c->max_tokens   = YOKE_MAX_TOKENS;
    c->max_messages = YOKE_MAX_MESSAGES;
    c->stream       = true;
    c->retries        = YOKE_RETRIES;
    c->retry_delay_ms = YOKE_RETRY_DELAY_MS;

    Str env_base = env_str(persist, "YOKE_BASE_URL");
    Str env_model = env_str(persist, "YOKE_MODEL");
    Str env_key   = env_str(persist, "YOKE_API_KEY");
    Str env_sys   = env_str(persist, "YOKE_SYSTEM_PROMPT");
    Str env_tools = env_str(persist, "YOKE_DISABLE_TOOLS");
    Str env_api   = env_str(persist, "YOKE_API");
    const char *env_msgs = getenv("YOKE_MAX_MESSAGES");

    Str candidates[YOKE_MAX_CONFIG_FILES];
    size_t cand_n = paths_config_files(STR("config"), scratch, candidates,
                                       YOKE_MAX_CONFIG_FILES);

    /* Lowest precedence first: a later file overwrites what an earlier set. */
    EnvSet env = {
        .base  = env_base.p != NULL,
        .model = env_model.p != NULL,
        .key   = env_key.p != NULL,
        .api   = env_api.p != NULL,
        .msgs  = env_msgs != NULL && *env_msgs != '\0',
        .tools = env_tools.p != NULL,
    };
    for (size_t ci = 0; ci < cand_n; ci++)
        config_apply_file(c, candidates[ci], env, persist, scratch);

    /* Remembered UI choices are newer than config files but remain below
     * per-invocation environment variables and command-line options. */
    Str remembered = state_get(STR("ui_stream"), persist, scratch);
    if (remembered.n) c->stream = !str_eq(remembered, STR("false"));
    remembered = state_get(STR("ui_mode"), persist, scratch);
    if (str_eq(remembered, STR("plan"))) c->mode = MODE_PLAN;
    else if (str_eq(remembered, STR("build"))) c->mode = MODE_BUILD;
    remembered = state_get(STR("ui_max_tokens"), persist, scratch);
    if (remembered.n) {
        b8 ok = false;
        i64 value = str_int(remembered, &ok);
        if (ok) c->max_tokens = (i32)clamp_size(value, 1, 1u << 20);
    }
    remembered = state_get(STR("ui_disable_tools"), persist, scratch);
    if (str_eq(remembered, STR("none"))) c->disable_tools = (Str){0};
    else if (remembered.n) c->disable_tools = remembered;

    size_t n;
    if (env_num("YOKE_MAX_TOKENS", 1, 1u << 20, &n))
        c->max_tokens = (i32)n;
    if (env_num("YOKE_MAX_MESSAGES", 8, 1u << 20, &n)) c->max_messages = n;
    if (env_num("YOKE_RETRIES", 0, 16, &n)) c->retries = (i32)n;
    if (env_num("YOKE_RETRY_DELAY_MS", 0, YOKE_MAX_RETRY_DELAY_MS, &n))
        c->retry_delay_ms = (i32)n;
    /* An in-app choice outranks the config files: it was made later and more
     * explicitly. The environment still wins, being per invocation. */
    if (!env_model.p) {
        remembered = state_get(STR("model"), persist, scratch);
        if (remembered.n) c->model = remembered;
    }
    /* The provider chosen with /provider: the endpoint the user last selected
     * in the UI, so it outranks a file written once and forgotten. Its key is
     * kept apart from its settings and read from the state directory. */
    Str active = endpoints_active(scratch);
    if (active.n) {
        Endpoints eps;
        endpoints_load(&eps, scratch);
        size_t i = endpoints_find(&eps, active);
        if (i != ENDPOINT_NONE) {
            Str name = str_dup(persist, eps.name[i]);
            Str url  = str_dup(persist, eps.base_url[i]);
            if (name.p && url.p) {
                c->provider = name;
                c->base_url = url;
                c->base_url_set = true;
                c->api = eps.api[i];
            }
            config_str(persist, eps.model[i], false, &c->model);
            c->reasoning_efforts = eps.reasoning_efforts[i].n
                ? str_dup(persist, eps.reasoning_efforts[i]) : (Str){0};
            c->thinking_budgets = eps.thinking_budgets[i].n
                ? str_dup(persist, eps.thinking_budgets[i]) : (Str){0};
            c->reasoning_effort = eps.reasoning_effort[i].n
                ? str_dup(persist, eps.reasoning_effort[i]) : (Str){0};
            c->thinking_budget = eps.thinking_budget[i].n
                ? str_dup(persist, eps.thinking_budget[i]) : (Str){0};
            c->reasoning_template = eps.reasoning_template[i].n
                ? str_dup(persist, eps.reasoning_template[i]) : (Str){0};
            Str key = endpoints_key(active, persist, scratch, NULL, 0);
            if (key.n) c->api_key = key;
        }
    }
    if (env_base.p)  { c->base_url = env_base; c->base_url_set = true; }
    if (env_model.p) c->model = env_model;
    if (env_key.p)   c->api_key = env_key;
    if (env_sys.p)   c->system_prompt = env_sys;
    if (env_tools.p) c->disable_tools = env_tools;
    if (env_api.p)   c->api = api_from_str(env_api);
    const char *env_stream = getenv("YOKE_STREAM");
    if (env_stream && *env_stream)
        c->stream = strcmp(env_stream, "false") != 0
                 && strcmp(env_stream, "off") != 0;
    const char *env_mode = getenv("YOKE_MODE");
    if (env_mode && !strcmp(env_mode, "plan")) c->mode = MODE_PLAN;
    else if (env_mode && !strcmp(env_mode, "build")) c->mode = MODE_BUILD;

    /* A placeholder rather than a destination: a run that named no endpoint
     * asks for one. It follows the API so the pair is at least coherent. */
    if (!c->base_url.p)
        c->base_url = c->api == API_ANTHROPIC
                    ? str_c("https://api.anthropic.com/v1")
                    : str_c("https://api.openai.com/v1");
    if (!c->model.p)
        c->model = c->api == API_ANTHROPIC ? str_c("claude-sonnet-4-5")
                                           : str_c("gpt-4o-mini");

    /* An unset system prompt is prompt_build's cue to look for a SYSTEM.md
     * and fall back to the built-in template. */

    return true;
}
