/* config.c: load Config from the environment and the settings files.
 *
 * Keys, all at the head of the config file: base_url, model, api_key, api,
 * max_tokens, max_messages, stream, retries, retry_delay_ms, disable_tools. A "[provider
 * <name>]" section of the same file is an endpoint (see endpoints.c).
 * The system prompt is not a key here: it is a document, so it lives in
 * SYSTEM.md (see prompt.c).
 * Precedence: env var YOKE_<KEY> > the active provider > the state file's
 * `model` > $XDG_CONFIG_HOME/yoke/config > the same file in each
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

    scratch->off = mark;
}

b8 config_remember_model(Str model, Arena *scratch) {
    if (!model.n || model.n >= 256) return false;
    return state_set(STR("model"), model, scratch);
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

    size_t n;
    if (env_num("YOKE_MAX_MESSAGES", 8, 1u << 20, &n)) c->max_messages = n;
    if (env_num("YOKE_RETRIES", 0, 16, &n)) c->retries = (i32)n;
    if (env_num("YOKE_RETRY_DELAY_MS", 0, YOKE_MAX_RETRY_DELAY_MS, &n))
        c->retry_delay_ms = (i32)n;
    /* An in-app choice outranks the config files: it was made later and more
     * explicitly. The environment still wins, being per invocation. */
    if (!env_model.p) {
        Str remembered = state_get(STR("model"), persist, scratch);
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
