/* config.c: load config from env and the XDG config files.
 *
 * Keys: base_url=, model=, api_key=, system_prompt=, max_tokens=, stream=
 * Precedence: env var YOKE_<KEY> > $XDG_CONFIG_HOME/yoke/config > the same
 * file in each $XDG_CONFIG_DIRS entry. See paths.c for the directories.
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
typedef struct { b8 base, model, key, sys, msgs; } EnvSet;

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
        if (str_eq(k, STR("base_url")) && !env.base) c->base_url = vd;
        else if (str_eq(k, STR("model")) && !env.model) c->model = vd;
        else if (str_eq(k, STR("api_key")) && !env.key) c->api_key = vd;
        else if (str_eq(k, STR("system_prompt")) && !env.sys) c->system_prompt = vd;
        else if (str_eq(k, STR("max_tokens"))) { b8 ok; i64 m = str_int(vd,&ok); if (ok) c->max_tokens = (i32)clamp_size(m, 1, 1u << 20); }
        else if (str_eq(k, STR("max_messages"))) { b8 ok; i64 m = str_int(vd,&ok); if (ok && !env.msgs) c->max_messages = clamp_size(m, 8, 1u << 20); }
        else if (str_eq(k, STR("stream"))) c->stream = !str_eq(vd, STR("false"));
    }
}

b8 config_load(Config *c, Arena *persist, Arena *scratch) {
    memset(c, 0, sizeof *c);
    c->max_tokens   = 4096;
    c->max_messages = YOKE_MAX_MESSAGES;
    c->stream       = true;

    Str env_base = env_str(persist, "YOKE_BASE_URL");
    Str env_model = env_str(persist, "YOKE_MODEL");
    Str env_key   = env_str(persist, "YOKE_API_KEY");
    Str env_sys   = env_str(persist, "YOKE_SYSTEM_PROMPT");
    const char *env_msgs = getenv("YOKE_MAX_MESSAGES");

    Str candidates[YOKE_MAX_CONFIG_FILES];
    size_t cand_n = paths_config_files(STR("config"), scratch, candidates,
                                       YOKE_MAX_CONFIG_FILES);

    /* Lowest precedence first: a later file overwrites what an earlier set. */
    EnvSet env = {
        .base  = env_base.p != NULL,
        .model = env_model.p != NULL,
        .key   = env_key.p != NULL,
        .sys   = env_sys.p != NULL,
        .msgs  = env_msgs != NULL && *env_msgs != '\0',
    };
    for (size_t ci = 0; ci < cand_n; ci++)
        config_apply_file(c, candidates[ci], env, persist, scratch);

    if (env_msgs && *env_msgs) {
        b8 ok = false;
        i64 m = str_int(str_c(env_msgs), &ok);
        if (ok) c->max_messages = clamp_size(m, 8, 1u << 20);
    }
    if (env_base.p)  c->base_url = env_base;
    if (env_model.p) c->model = env_model;
    if (env_key.p)   c->api_key = env_key;
    if (env_sys.p)   c->system_prompt = env_sys;

    if (!c->base_url.p) c->base_url = str_c("https://api.openai.com/v1");
    if (!c->model.p)   c->model   = str_c("gpt-4o-mini");

    if (!c->system_prompt.p) {
        c->system_prompt = str_c(
            "You are yoke, a terminal coding agent. You edit files using tools.\n"
            "Call tools to accomplish the user's task. Be concise.\n");
    }

    return true;
}
