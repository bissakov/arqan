/* config.c — load config from env and ~/.config/yoke/config.
 *
 * Keys: base_url=, model=, api_key=, system_prompt=, max_tokens=, stream=
 * First match wins: env var YOKE_<KEY> > file value.
 */
#include "yoke.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

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

b8 config_load(Config *c, Arena *persist) {
    memset(c, 0, sizeof *c);
    c->max_tokens = 4096;
    c->stream     = true;

    Str env_base = env_str(persist, "YOKE_BASE_URL");
    Str env_model = env_str(persist, "YOKE_MODEL");
    Str env_key   = env_str(persist, "YOKE_API_KEY");
    Str env_sys   = env_str(persist, "YOKE_SYSTEM_PROMPT");

    /* read config file into a scratch arena */
    Arena scratch;
    static u8 sbuf[1 << 20];
    arena_init(&scratch, sbuf, sizeof sbuf);

    const char *home = getenv("XDG_CONFIG_HOME");
    if (!home || !*home) {
        const char *h = getenv("HOME");
        if (!h || !*h) { struct passwd *pw = getpwuid(getuid()); h = pw ? pw->pw_dir : "/tmp"; }
        home = h;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/yoke/config", home);

    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); i64 sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            char *buf = arena_new(&scratch, char, (size_t)sz + 1);
            size_t rd = fread(buf, 1, (size_t)sz, f);
            buf[rd] = '\0';
            Str src = { buf, rd };
            size_t s = 0;
            for (size_t i = 0; i <= src.n; i++) {
                if (i == src.n || src.p[i] == '\n') {
                    Str line = { src.p + s, i - s };
                    Str k, v;
                    if (file_kv(line, &k, &v)) {
                        Str vd = str_dup(persist, v);
                        if (str_eq(k, STR("base_url")) && !env_base.p) c->base_url = vd;
                        else if (str_eq(k, STR("model")) && !env_model.p) c->model = vd;
                        else if (str_eq(k, STR("api_key")) && !env_key.p) c->api_key = vd;
                        else if (str_eq(k, STR("system_prompt")) && !env_sys.p) c->system_prompt = vd;
                        else if (str_eq(k, STR("max_tokens"))) { b8 ok; i64 m = str_int(vd,&ok); if (ok) c->max_tokens = (i32)m; }
                        else if (str_eq(k, STR("stream"))) c->stream = !str_eq(vd, STR("false"));
                    }
                    s = i + 1;
                }
            }
        }
        fclose(f);
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
