/* config.c: load Config from the environment and the settings files.
 *
 * Keys, all at the head of the config file: base_url, model, api_key,
 * max_tokens, max_messages, stream, retries, retry_delay_ms. A "[provider
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

/* Keys the environment already set: no config file may override them. */
typedef struct { b8 base, model, key, msgs; } EnvSet;

static b8 config_num(const Settings *s, Str key, size_t lo, size_t hi,
                     size_t *out) {
    Str v = settings_get(s, (Str){0}, key);
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

    Str base  = settings_get(&s, (Str){0}, STR("base_url"));
    Str model = settings_get(&s, (Str){0}, STR("model"));
    Str key   = settings_get(&s, (Str){0}, STR("api_key"));
    if (base.n && !env.base) {
        Str v = str_dup(persist, base);
        if (v.p) { c->base_url = v; c->base_url_set = true; }
    }
    if (model.n && !env.model) {
        Str v = str_dup(persist, model);
        if (v.p) c->model = v;
    }
    if (key.n && !env.key) {
        Str v = str_dup(persist, key);
        if (v.p) c->api_key = v;
    }

    size_t n;
    if (config_num(&s, STR("max_tokens"), 1, 1u << 20, &n))
        c->max_tokens = (i32)n;
    if (!env.msgs && config_num(&s, STR("max_messages"), 8, 1u << 20, &n))
        c->max_messages = n;
    if (config_num(&s, STR("retries"), 0, 16, &n))
        c->retries = (i32)n;
    if (config_num(&s, STR("retry_delay_ms"), 0, YOKE_MAX_RETRY_DELAY_MS, &n))
        c->retry_delay_ms = (i32)n;
    Str stream = settings_get(&s, (Str){0}, STR("stream"));
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
    const char *env_msgs = getenv("YOKE_MAX_MESSAGES");
    const char *env_retries = getenv("YOKE_RETRIES");
    const char *env_retry_ms = getenv("YOKE_RETRY_DELAY_MS");

    Str candidates[YOKE_MAX_CONFIG_FILES];
    size_t cand_n = paths_config_files(STR("config"), scratch, candidates,
                                       YOKE_MAX_CONFIG_FILES);

    /* Lowest precedence first: a later file overwrites what an earlier set. */
    EnvSet env = {
        .base  = env_base.p != NULL,
        .model = env_model.p != NULL,
        .key   = env_key.p != NULL,
        .msgs  = env_msgs != NULL && *env_msgs != '\0',
    };
    for (size_t ci = 0; ci < cand_n; ci++)
        config_apply_file(c, candidates[ci], env, persist, scratch);

    if (env_msgs && *env_msgs) {
        b8 ok = false;
        i64 m = str_int(str_c(env_msgs), &ok);
        if (ok) c->max_messages = clamp_size(m, 8, 1u << 20);
    }
    if (env_retries && *env_retries) {
        b8 ok = false;
        i64 m = str_int(str_c(env_retries), &ok);
        if (ok) c->retries = (i32)clamp_size(m, 0, 16);
    }
    if (env_retry_ms && *env_retry_ms) {
        b8 ok = false;
        i64 m = str_int(str_c(env_retry_ms), &ok);
        if (ok) c->retry_delay_ms = (i32)clamp_size(m, 0, YOKE_MAX_RETRY_DELAY_MS);
    }
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
            }
            if (eps.model[i].n) {
                Str model = str_dup(persist, eps.model[i]);
                if (model.p) c->model = model;
            }
            Str key = endpoints_key(active, persist, scratch, NULL, 0);
            if (key.n) c->api_key = key;
        }
    }
    if (env_base.p)  { c->base_url = env_base; c->base_url_set = true; }
    if (env_model.p) c->model = env_model;
    if (env_key.p)   c->api_key = env_key;
    if (env_sys.p)   c->system_prompt = env_sys;

    if (!c->base_url.p) c->base_url = str_c("https://api.openai.com/v1");
    if (!c->model.p)   c->model   = str_c("gpt-4o-mini");

    /* An unset system prompt is prompt_build's cue to look for a SYSTEM.md
     * and fall back to the built-in template. */

    return true;
}
