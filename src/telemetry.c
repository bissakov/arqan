/* telemetry.c: an anonymized record of a session, for debugging.
 *
 * Off until /telemetry turns it on, which is remembered in
 * $XDG_STATE_HOME/yoke/telemetry so a later run records without being asked
 * again. Events are JSON objects, one per line, appended to
 * $XDG_STATE_HOME/yoke/telemetry.jsonl.
 *
 * The file holds the shape of a session and none of its content: a message is
 * a byte and a line count, a tool call is its name and the keys of its
 * arguments, a reply is its size and its token counts. The strings that do
 * land there are the ones yoke formats itself (a tool name, a model id, a log
 * line), so the record says what happened without saying what was said. The
 * working directory is a hash for the same reason: two runs can be told apart
 * without naming the project either of them was in.
 *
 * A line is built on the stack and appended with its own open and close, so an
 * interrupted run leaves whole lines behind and a recorder that cannot write
 * costs the session nothing.
 */
#include "yoke.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Of a name or a diagnostic, what one field keeps. Longer than any tool or
 * model name and short enough that a line stays one line. */
#define TEL_STR_MAX 120

static struct {
    b8   on;
    b8   ready;              /* the paths below resolved                    */
    char dir_buf[YOKE_MAX_PATH];
    char path_buf[YOKE_MAX_PATH];
    char flag_buf[YOKE_MAX_PATH];
    Str  dir;
    u64  run;                /* distinguishes runs sharing the file         */
    f64  t0;
    u64  seq;
} g_tel;

static u64 tel_hash(Str s) {
    u64 h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < s.n; i++) {
        h ^= (u8)s.p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* Copy a resolved path into the struct: the arena it was built in belongs to
 * the caller and is rewound as soon as startup is over. */
static b8 tel_keep(char *dst, size_t cap, Str path) {
    if (!path.n || path.n >= cap) return false;
    memcpy(dst, path.p, path.n);
    dst[path.n] = '\0';
    return true;
}

static void tel_flush(const char *line, size_t n) {
    if (!g_tel.on || !g_tel.ready) return;
    paths_ensure_dir(g_tel.dir);
    FILE *f = fopen(g_tel.path_buf, "ab");
    if (!f) return;
    fwrite(line, 1, n, f);
    fputc('\n', f);
    fclose(f);
}

b8 telemetry_on(void) { return g_tel.on && g_tel.ready; }

Str telemetry_file(void) {
    return g_tel.ready ? str_c(g_tel.path_buf) : (Str){0};
}

void telemetry_init(Arena *scratch) {
    g_tel.t0 = yoke_now_seconds();
    size_t mark = scratch->off;
    g_tel.ready =
        tel_keep(g_tel.dir_buf, sizeof g_tel.dir_buf,
                 paths_dir(YOKE_DIR_STATE, scratch))
        && tel_keep(g_tel.path_buf, sizeof g_tel.path_buf,
                    paths_file(YOKE_DIR_STATE, STR("telemetry.jsonl"), scratch))
        && tel_keep(g_tel.flag_buf, sizeof g_tel.flag_buf,
                    paths_file(YOKE_DIR_STATE, STR("telemetry"), scratch));
    scratch->off = mark;
    g_tel.dir = str_c(g_tel.dir_buf);
    if (!g_tel.ready) return;

    /* Enough to tell two runs apart in one file, and nothing that says whose
     * they are: a pid and two clocks, hashed. */
    char seed[64];
    i32 n = snprintf(seed, sizeof seed, "%ld:%ld:%f", (long)getpid(),
                     (long)time(NULL), g_tel.t0);
    g_tel.run = tel_hash((Str){ seed, n > 0 ? (size_t)n : 0 });

    FILE *f = fopen(g_tel.flag_buf, "rb");
    if (!f) return;
    char line[16] = {0};
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (got) g_tel.on = str_eq(str_trim(str_c(line)), STR("on"));
}

b8 telemetry_set(b8 on, Arena *scratch) {
    (void)scratch;
    if (!g_tel.ready || !paths_ensure_dir(g_tel.dir)) return false;
    FILE *f = fopen(g_tel.flag_buf, "wb");
    if (!f) return false;
    fputs(on ? "on\n" : "off\n", f);
    if (fclose(f) != 0) return false;
    g_tel.on = on;
    return true;
}

/* ---- one event ---------------------------------------------------------- */
static void tel_add(TelEvent *e, const char *p, size_t n) {
    if (!e->live) return;
    /* The tail is reserved for what tel_send owes the line: the truncation
     * marker and the closing brace, which therefore always fit. */
    size_t cap = sizeof e->buf - 16;
    if (n > cap - e->n) { e->full = true; return; }
    memcpy(e->buf + e->n, p, n);
    e->n += n;
}

static void tel_addz(TelEvent *e, const char *z) { tel_add(e, z, strlen(z)); }

static void tel_key(TelEvent *e, const char *key) {
    tel_addz(e, ",\"");
    tel_addz(e, key);
    tel_addz(e, "\":");
}

void tel_open(TelEvent *e, const char *ev) {
    e->n = 0;
    e->full = false;
    e->live = telemetry_on();
    if (!e->live) return;
    char head[96];
    u64 ms = (u64)((yoke_now_seconds() - g_tel.t0) * 1000.0);
    i32 n = snprintf(head, sizeof head,
                     "{\"t\":%llu,\"run\":\"%016llx\",\"seq\":%llu,\"ev\":\"%s\"",
                     (unsigned long long)ms, (unsigned long long)g_tel.run,
                     (unsigned long long)g_tel.seq++, ev);
    if (n > 0) tel_add(e, head, (size_t)n);
}

void tel_int(TelEvent *e, const char *key, i64 v) {
    if (!e->live) return;
    char num[24];
    i32 n = snprintf(num, sizeof num, "%lld", (long long)v);
    if (n <= 0) return;
    tel_key(e, key);
    tel_add(e, num, (size_t)n);
}

void tel_bool(TelEvent *e, const char *key, b8 v) {
    if (!e->live) return;
    tel_key(e, key);
    tel_addz(e, v ? "true" : "false");
}

void tel_str(TelEvent *e, const char *key, Str v) {
    if (!e->live) return;
    tel_key(e, key);
    tel_addz(e, "\"");
    v = str_clip_utf8(v, TEL_STR_MAX);
    for (size_t i = 0; i < v.n; i++) {
        u8 c = (u8)v.p[i];
        char esc[8];
        switch (c) {
            case '"':  tel_add(e, "\\\"", 2); break;
            case '\\': tel_add(e, "\\\\", 2); break;
            case '\n': tel_add(e, "\\n", 2); break;
            case '\r': tel_add(e, "\\r", 2); break;
            case '\t': tel_add(e, "\\t", 2); break;
            default:
                if (c < 0x20) {
                    i32 n = snprintf(esc, sizeof esc, "\\u%04x", c);
                    if (n > 0) tel_add(e, esc, (size_t)n);
                } else {
                    tel_add(e, (const char *)&c, 1);
                }
        }
    }
    tel_addz(e, "\"");
}

void tel_shape(TelEvent *e, const char *key, Str text) {
    if (!e->live) return;
    char field[64];
    i32 n = snprintf(field, sizeof field, "%s_bytes", key);
    if (n > 0 && (size_t)n < sizeof field) tel_int(e, field, (i64)text.n);
    n = snprintf(field, sizeof field, "%s_lines", key);
    if (n > 0 && (size_t)n < sizeof field)
        tel_int(e, field, (i64)(text.n ? str_lines(text) : 0));
}

void tel_arg_keys(TelEvent *e, const char *key, Str args, Arena *scratch) {
    if (!e->live) return;
    size_t mark = scratch->off;
    const JVal *j = json_parse(scratch, args);
    tel_key(e, key);
    tel_addz(e, "\"");
    if (j && j->type == J_OBJ) {
        b8 first = true;
        for (const JVal *m = j->u.obj.head; m; m = m->next) {
            if (!first) tel_addz(e, ",");
            first = false;
            /* A key is a schema field name, so it is written as it came; a
             * value never is. */
            Str k = str_clip_utf8(m->key, 40);
            for (size_t i = 0; i < k.n; i++) {
                char c = k.p[i];
                b8 plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '_' || c == '-';
                tel_add(e, plain ? &c : "?", 1);
            }
        }
    }
    tel_addz(e, "\"");
    scratch->off = mark;
}

void tel_send(TelEvent *e) {
    if (!e->live) return;
    /* Straight past the reserve the fields respect: a line that dropped one
     * says so, whichever field it was that did not fit. */
    if (e->full) {
        static const char trunc[] = ",\"trunc\":true";
        memcpy(e->buf + e->n, trunc, sizeof trunc - 1);
        e->n += sizeof trunc - 1;
    }
    e->buf[e->n++] = '}';
    tel_flush(e->buf, e->n);
    e->live = false;
}

void telemetry_log(i32 level, Str msg) {
    static const char *tags[] = { "debug", "info", "warn", "error" };
    if (!telemetry_on()) return;
    if (level < YOKE_LOG_DEBUG || level > YOKE_LOG_ERROR) level = YOKE_LOG_ERROR;
    TelEvent e;
    tel_open(&e, "log");
    tel_str(&e, "level", str_c(tags[level]));
    tel_str(&e, "msg", msg);
    tel_send(&e);
}

void tel_hash_field(TelEvent *e, const char *key, Str v) {
    if (!e->live || !v.n) return;
    char hex[20];
    i32 n = snprintf(hex, sizeof hex, "%016llx",
                     (unsigned long long)tel_hash(v));
    if (n > 0) tel_str(e, key, (Str){ hex, (size_t)n });
}
