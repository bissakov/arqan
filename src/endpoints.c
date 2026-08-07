/* endpoints.c: the providers the /provider command creates and switches to.
 *
 * An endpoint is a user-defined OpenAI-compatible service: a name, a base URL
 * and the model last used against it. Nothing is built in, because every
 * endpoint speaks the same protocol and only the user knows which ones exist.
 * (`Provider` in provider.c is the streaming run context; this is the entry a
 * run is configured from.)
 *
 * Two files, split so the settings can be shared and the secret cannot:
 *   $XDG_CONFIG_HOME/yoke/providers   JSONL {"name","base_url","model"}
 *   $XDG_STATE_HOME/yoke/credentials  JSONL {"name","key"}, mode 0600
 * The first is what a dotfile repository would carry, so it never holds a
 * key; the second is machine-local and is refused when its mode lets anyone
 * else read it. $XDG_STATE_HOME/yoke/provider names the active entry.
 *
 * A store is rewritten whole through a temporary file and a rename, so an
 * interrupted write leaves the previous list rather than half a line.
 */
#include "yoke.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENDPOINT_STORE_BYTES (1u << 16)   /* largest store file we will read */

/* A credential pair, only ever held for the length of one rewrite. */
typedef struct {
    Str    name[YOKE_MAX_ENDPOINTS];
    Str    key[YOKE_MAX_ENDPOINTS];
    size_t n;
} Creds;

static Str endpoint_read(Arena *a, Str path, size_t max) {
    if (!path.n) return (Str){0};
    FILE *f = fopen(path.p, "rb");
    if (!f) return (Str){0};
    fseek(f, 0, SEEK_END);
    i64 sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > max) { fclose(f); return (Str){0}; }
    char *buf = arena_new(a, char, (size_t)sz + 1);
    if (!buf) { fclose(f); return (Str){0}; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return (Str){ buf, rd };
}

/* Replace `path` with `data` at `mode`. The rename is atomic within the
 * directory, so a reader sees either store whole. */
static b8 endpoint_write(Str path, Str data, mode_t mode) {
    char tmp[YOKE_MAX_PATH];
    i32 n = snprintf(tmp, sizeof tmp, "%.*s.tmp", (i32)path.n, path.p);
    if (n <= 0 || (size_t)n >= sizeof tmp) return false;
    i32 fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    b8 ok = true;
    for (size_t off = 0; ok && off < data.n;) {
        ssize_t w = write(fd, data.p + off, data.n - off);
        if (w <= 0) ok = false; else off += (size_t)w;
    }
    if (fchmod(fd, mode) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, path.p) == 0) return true;
    unlink(tmp);
    return false;
}

/* A field a store line may carry, refused rather than clamped when it is
 * longer than its cap: a truncated URL or key names something else. */
static Str endpoint_field(const JVal *line, Str key, size_t max) {
    const JVal *v = json_get(line, key);
    if (!v || v->type != J_STR || v->u.s.n > max) return (Str){0};
    return v->u.s;
}

/* Call `fn` for every JSON object in a JSONL document. */
typedef void (*EndpointLine)(const JVal *obj, void *ud);

static void endpoint_each(Str src, Arena *scratch, EndpointLine fn, void *ud) {
    size_t start = 0;
    for (size_t i = 0; i <= src.n; i++) {
        if (i != src.n && src.p[i] != '\n') continue;
        Str line = { src.p + start, i - start };
        start = i + 1;
        if (line.n < 2) continue;
        JVal *v = json_parse(scratch, line);
        if (v && v->type == J_OBJ) fn(v, ud);
    }
}

static void endpoint_collect(const JVal *obj, void *ud) {
    Endpoints *e = ud;
    if (e->n >= YOKE_MAX_ENDPOINTS) return;
    Str name = endpoint_field(obj, STR("name"), YOKE_MAX_ENDPOINT_NAME);
    Str url  = endpoint_field(obj, STR("base_url"), YOKE_MAX_URL);
    if (!name.n || !url.n) return;
    e->name[e->n] = name;
    e->base_url[e->n] = url;
    e->model[e->n] = endpoint_field(obj, STR("model"), YOKE_MAX_MODEL_NAME);
    e->n++;
}

size_t endpoints_load(Endpoints *e, Arena *a) {
    memset(e, 0, sizeof *e);
    Str path = paths_file(YOKE_DIR_CONFIG, STR("providers"), a);
    Str src = endpoint_read(a, path, ENDPOINT_STORE_BYTES);
    if (src.n) endpoint_each(src, a, endpoint_collect, e);
    return e->n;
}

size_t endpoints_find(const Endpoints *e, Str name) {
    for (size_t i = 0; i < e->n; i++)
        if (str_eq(e->name[i], name)) return i;
    return ENDPOINT_NONE;
}

b8 endpoints_put(Endpoints *e, Str name, Str base_url, Str model, Arena *a) {
    if (!name.n || name.n > YOKE_MAX_ENDPOINT_NAME) return false;
    if (!base_url.n || base_url.n > YOKE_MAX_URL) return false;
    if (model.n > YOKE_MAX_MODEL_NAME) return false;
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
    if (!url.p || (model.n && !mdl.p)) return false;
    e->base_url[i] = url;
    e->model[i] = mdl;
    return true;
}

b8 endpoints_save(const Endpoints *e, Arena *scratch) {
    Str dir = paths_dir(YOKE_DIR_CONFIG, scratch);
    Str path = paths_file(YOKE_DIR_CONFIG, STR("providers"), scratch);
    if (!dir.n || !path.n || !paths_ensure_dir(dir)) return false;
    Buf b; buf_init(&b, scratch, 4096);
    for (size_t i = 0; i < e->n; i++) {
        buf_puts(&b, STR("{\"name\":"));
        buf_json_str(&b, e->name[i]);
        buf_puts(&b, STR(",\"base_url\":"));
        buf_json_str(&b, e->base_url[i]);
        buf_puts(&b, STR(",\"model\":"));
        buf_json_str(&b, e->model[i]);
        buf_puts(&b, STR("}\n"));
    }
    if (!buf_ok(&b)) return false;
    return endpoint_write(path, buf_finish(&b), 0600);
}

b8 endpoints_remember_model(Str name, Str model, Arena *scratch) {
    Endpoints e;
    endpoints_load(&e, scratch);
    size_t i = endpoints_find(&e, name);
    if (i == ENDPOINT_NONE) return false;
    if (!endpoints_put(&e, name, e.base_url[i], model, scratch)) return false;
    return endpoints_save(&e, scratch);
}

static void cred_collect(const JVal *obj, void *ud) {
    Creds *c = ud;
    if (c->n >= YOKE_MAX_ENDPOINTS) return;
    Str name = endpoint_field(obj, STR("name"), YOKE_MAX_ENDPOINT_NAME);
    Str key  = endpoint_field(obj, STR("key"), YOKE_MAX_API_KEY);
    if (!name.n || !key.n) return;
    c->name[c->n] = name;
    c->key[c->n] = key;
    c->n++;
}

/* The credentials file, refused when anyone but the owner can read it: a key
 * left world-readable is a key to rotate, not one to load. */
static b8 creds_read(Creds *c, Arena *a, Str *path_out,
                     char *err, size_t err_cap) {
    memset(c, 0, sizeof *c);
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
    Str src = endpoint_read(a, path, ENDPOINT_STORE_BYTES);
    if (src.n) endpoint_each(src, a, cred_collect, c);
    return true;
}

Str endpoints_key(Str name, Arena *out, Arena *scratch,
                  char *err, size_t err_cap) {
    Creds c;
    size_t mark = scratch->off;
    Str key = {0};
    if (creds_read(&c, scratch, NULL, err, err_cap))
        for (size_t i = 0; i < c.n; i++)
            if (str_eq(c.name[i], name)) key = str_dup(out, c.key[i]);
    scratch->off = mark;
    return key;
}

b8 endpoints_set_key(Str name, Str key, Arena *scratch,
                     char *err, size_t err_cap) {
    if (!name.n || key.n > YOKE_MAX_API_KEY) return false;
    Creds c;
    Str path = {0};
    if (!creds_read(&c, scratch, &path, err, err_cap)) return false;
    Str dir = paths_dir(YOKE_DIR_STATE, scratch);
    if (!path.n || !dir.n || !paths_ensure_dir(dir)) return false;

    size_t at = c.n;
    for (size_t i = 0; i < c.n; i++) if (str_eq(c.name[i], name)) at = i;
    if (at == c.n) {
        if (c.n >= YOKE_MAX_ENDPOINTS) return false;
        c.name[c.n] = name;
        c.n++;
    }
    c.key[at] = key;

    Buf b; buf_init(&b, scratch, 4096);
    for (size_t i = 0; i < c.n; i++) {
        if (!c.key[i].n) continue;
        buf_puts(&b, STR("{\"name\":"));
        buf_json_str(&b, c.name[i]);
        buf_puts(&b, STR(",\"key\":"));
        buf_json_str(&b, c.key[i]);
        buf_puts(&b, STR("}\n"));
    }
    if (!buf_ok(&b)) return false;
    return endpoint_write(path, buf_finish(&b), 0600);
}

Str endpoints_active(Arena *a) {
    size_t mark = a->off;
    Str path = paths_file(YOKE_DIR_STATE, STR("provider"), a);
    if (!path.n) { a->off = mark; return (Str){0}; }
    FILE *f = fopen(path.p, "rb");
    if (!f) { a->off = mark; return (Str){0}; }
    char line[YOKE_MAX_ENDPOINT_NAME + 2];
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    Str name = got ? str_trim(str_c(line)) : (Str){0};
    if (!name.n) { a->off = mark; return (Str){0}; }
    return str_dup(a, name);
}

b8 endpoints_remember_active(Str name, Arena *scratch) {
    if (!name.n || name.n > YOKE_MAX_ENDPOINT_NAME) return false;
    Str dir = paths_dir(YOKE_DIR_STATE, scratch);
    Str path = paths_file(YOKE_DIR_STATE, STR("provider"), scratch);
    if (!dir.n || !path.n || !paths_ensure_dir(dir)) return false;
    Buf b; buf_init(&b, scratch, name.n + 2);
    buf_puts(&b, name);
    buf_putc(&b, '\n');
    if (!buf_ok(&b)) return false;
    return endpoint_write(path, buf_finish(&b), 0600);
}
