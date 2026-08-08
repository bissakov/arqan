/* config.c: load config from env and the XDG config files.
 *
 * Keys: base_url=, model=, api_key=, max_tokens=, max_messages=, stream=,
 * retries=, retry_delay_ms=
 * The system prompt is not a key here: it is a document, so it lives in
 * SYSTEM.md (see prompt.c).
 * Precedence: env var YOKE_<KEY> > the active provider (see endpoints.c) >
 * $XDG_CONFIG_HOME/yoke/config > the same file in each $XDG_CONFIG_DIRS
 * entry. See paths.c for the directories.
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

static b8 file_kv(Str line, Str *k, Str *v) {
    line = str_trim(line);
    if (line.n == 0 || line.p[0] == '#') return false;
    size_t eq = 0;
    while (eq < line.n && line.p[eq] != '=') eq++;
    if (eq == line.n) return false;
    *k = str_trim(str_take(line, eq));
    *v = str_trim(str_drop(line, eq + 1));
    return k->n > 0;
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

static void config_apply_file(Config *c, Str path, EnvSet env,
                              Arena *persist, Arena *scratch) {
    FILE *f = fopen(path.p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); i64 sz = ftell(f); fseek(f, 0, SEEK_SET);
    /* The file lives in scratch for the length of this function only. */
    char *buf = sz > 0 && sz <= (1 << 20)
              ? arena_new(scratch, char, (size_t)sz + 1) : NULL;
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    Str src = { buf, rd };
    size_t s = 0;
    for (size_t i = 0; i <= src.n; i++) {
        if (i != src.n && src.p[i] != '\n') continue;
        Str line = { src.p + s, i - s };
        s = i + 1;
        Str k, v;
        if (!file_kv(line, &k, &v)) continue;
        Str vd = str_dup(persist, v);
        if (str_eq(k, STR("base_url")) && !env.base) { c->base_url = vd; c->base_url_set = true; }
        else if (str_eq(k, STR("model")) && !env.model) c->model = vd;
        else if (str_eq(k, STR("api_key")) && !env.key) c->api_key = vd;
        else if (str_eq(k, STR("max_tokens"))) { b8 ok; i64 m = str_int(vd,&ok); if (ok) c->max_tokens = (i32)clamp_size(m, 1, 1u << 20); }
        else if (str_eq(k, STR("max_messages"))) { b8 ok; i64 m = str_int(vd,&ok); if (ok && !env.msgs) c->max_messages = clamp_size(m, 8, 1u << 20); }
        else if (str_eq(k, STR("stream"))) c->stream = !str_eq(vd, STR("false"));
        else if (str_eq(k, STR("retries"))) { b8 ok; i64 m = str_int(vd,&ok); if (ok) c->retries = (i32)clamp_size(m, 0, 16); }
        else if (str_eq(k, STR("retry_delay_ms"))) { b8 ok; i64 m = str_int(vd,&ok); if (ok) c->retry_delay_ms = (i32)clamp_size(m, 0, YOKE_MAX_RETRY_DELAY_MS); }
    }
}

/* The model /model last chose: the first line of the state file, empty when
 * there is none. */
static Str state_model(Arena *persist, Arena *scratch) {
    Str path = paths_file(YOKE_DIR_STATE, STR("model"), scratch);
    if (!path.n) return (Str){0};
    FILE *f = fopen(path.p, "rb");
    if (!f) return (Str){0};
    char line[512];
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got) return (Str){0};
    Str name = str_trim(str_c(line));
    return name.n ? str_dup(persist, name) : (Str){0};
}

b8 config_remember_model(Str model, Arena *scratch) {
    if (!model.n || model.n >= 256) return false;
    Str dir = paths_dir(YOKE_DIR_STATE, scratch);
    Str path = paths_file(YOKE_DIR_STATE, STR("model"), scratch);
    if (!dir.n || !path.n || !paths_ensure_dir(dir)) return false;
    FILE *f = fopen(path.p, "wb");
    if (!f) return false;
    size_t wrote = fwrite(model.p, 1, model.n, f);
    fputc('\n', f);
    return fclose(f) == 0 && wrote == model.n;
}

b8 config_load(Config *c, Arena *persist, Arena *scratch) {
    memset(c, 0, sizeof *c);
    c->max_tokens   = 4096;
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
        Str remembered = state_model(persist, scratch);
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
