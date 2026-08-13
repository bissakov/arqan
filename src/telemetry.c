#include "agent.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Of a name or a diagnostic, what one field keeps. Longer than any tool or
 * model name and short enough that a line stays one line. */
#define TEL_STR_MAX 120

static struct {
    b8   on;
    b8   ready;              /* the root below resolved                     */
    char root_buf[AGENT_MAX_PATH];  /* .../arqan/telemetry                    */
    char dir_buf[AGENT_MAX_PATH];   /* the current file's directory          */
    char path_buf[AGENT_MAX_PATH];  /* the current file                      */
    char slug_buf[256];      /* the session directory's last component      */
    char stem_buf[64];       /* the session file without its extension      */
    char run_stem[40];       /* the name a session that has none falls to   */
    Str  dir;
    u64  run;
    f64  t0;
    u64  seq;
    b8   header_due;         /* the file has no session event yet           */
    b8   attached;           /* a file was chosen                           */
    /* What was recorded before a session named a file. Sized for a startup
     * and the commands that reach one: past that the run is a record of its
     * own rather than a reason to drop lines. */
    char pend[8192];
    size_t pend_n;
    TelHeader header;
    void *header_ud;
} g_tel;

/* Copy a resolved path into the struct: the arena it was built in belongs to
 * the caller and is rewound as soon as startup is over. */
static b8 tel_keep(char *dst, size_t cap, Str path) {
    if (!path.n || path.n >= cap) return false;
    memcpy(dst, path.p, path.n);
    dst[path.n] = '\0';
    return true;
}

/* The one write: whole lines, appended to the file the record is bound to.
 * A directory that cannot be made or a file that cannot be opened costs the
 * session nothing. */
static void tel_append(const char *data, size_t n, b8 newline) {
    if (!n || !g_tel.path_buf[0]) return;
    paths_ensure_dir(g_tel.dir);
    FILE *f = fopen(g_tel.path_buf, "ab");
    if (!f) return;
    fwrite(data, 1, n, f);
    if (newline) fputc('\n', f);
    fclose(f);
}

/* Take the file and the lines that were waiting for one. They open with the
 * session event, so a file that receives them is owed no other; one that
 * starts empty is, since a reader of it has no earlier line to learn the run
 * from. */
static void tel_attach(const char *dir, size_t dn, const char *path, size_t pn) {
    if (dn >= sizeof g_tel.dir_buf || pn >= sizeof g_tel.path_buf) return;
    memcpy(g_tel.dir_buf, dir, dn + 1);
    memcpy(g_tel.path_buf, path, pn + 1);
    g_tel.dir = (Str){ g_tel.dir_buf, dn };
    g_tel.attached = true;
    g_tel.header_due = g_tel.pend_n == 0;
    if (g_tel.pend_n) {
        tel_append(g_tel.pend, g_tel.pend_n, false);
        g_tel.pend_n = 0;
    }
}

/* Where waiting lines go when no conversation ever claims them. */
static void tel_attach_run(void) {
    char path[AGENT_MAX_PATH];
    i32 pn = snprintf(path, sizeof path, "%s/%s.jsonl", g_tel.root_buf,
                      g_tel.run_stem);
    if (pn <= 0 || (size_t)pn >= sizeof path) return;
    tel_attach(g_tel.root_buf, strlen(g_tel.root_buf), path, (size_t)pn);
}

/* The record of a conversation is named after its session file and sits under
 * the same per-directory component, so one is found from the other. It is
 * taken from the path rather than from Session.name, which is the label the
 * picker shows. */
void telemetry_bind(Str session_path) {
    if (!g_tel.ready) return;
    size_t base = session_path.n;
    while (base && session_path.p[base - 1] != '/') base--;
    Str name = str_drop(session_path, base);
    Str dir = str_take(session_path, base ? base - 1 : 0);
    if (name.n > 6 && !memcmp(name.p + name.n - 6, ".jsonl", 6)) name.n -= 6;
    size_t cut = dir.n;
    while (cut && dir.p[cut - 1] != '/') cut--;
    Str slug = str_drop(dir, cut);
    if (!name.n || name.n >= sizeof g_tel.stem_buf
        || slug.n >= sizeof g_tel.slug_buf) return;
    memcpy(g_tel.stem_buf, name.p, name.n);
    g_tel.stem_buf[name.n] = '\0';
    memcpy(g_tel.slug_buf, slug.p, slug.n);
    g_tel.slug_buf[slug.n] = '\0';

    char d[AGENT_MAX_PATH], path[AGENT_MAX_PATH];
    i32 dn = slug.n ? snprintf(d, sizeof d, "%s/%s", g_tel.root_buf,
                               g_tel.slug_buf)
                    : snprintf(d, sizeof d, "%s", g_tel.root_buf);
    if (dn <= 0 || (size_t)dn >= sizeof d) return;
    i32 pn = snprintf(path, sizeof path, "%s/%s.jsonl", d, g_tel.stem_buf);
    if (pn <= 0 || (size_t)pn >= sizeof path) return;
    if (g_tel.attached && !strcmp(path, g_tel.path_buf)) return;
    tel_attach(d, (size_t)dn, path, (size_t)pn);
}

/* The conversation the record was following is over: what follows belongs to
 * the next one, so it waits for the file that one will name. */
void telemetry_detach(void) {
    g_tel.attached = false;
    g_tel.pend_n = 0;
    g_tel.stem_buf[0] = g_tel.slug_buf[0] = '\0';
    g_tel.header_due = true;
}

void telemetry_close(void) {
    if (g_tel.ready && g_tel.pend_n) tel_attach_run();
}

void telemetry_set_header(TelHeader fn, void *ud) {
    g_tel.header = fn;
    g_tel.header_ud = ud;
}

static void tel_flush(const char *line, size_t n) {
    if (!g_tel.on || !g_tel.ready) return;
    if (!g_tel.attached) {
        if (n + 1 <= sizeof g_tel.pend - g_tel.pend_n) {
            memcpy(g_tel.pend + g_tel.pend_n, line, n);
            g_tel.pend_n += n;
            g_tel.pend[g_tel.pend_n++] = '\n';
            return;
        }
        tel_attach_run();
        if (!g_tel.attached) return;
    }
    tel_append(line, n, true);
}

b8 telemetry_on(void) { return g_tel.on && g_tel.ready; }

Str telemetry_file(void) {
    return g_tel.ready && g_tel.path_buf[0] ? str_c(g_tel.path_buf) : (Str){0};
}

void telemetry_init(Arena *scratch, b8 on) {
    g_tel.t0 = agent_now_seconds();
    size_t mark = scratch->off;
    g_tel.ready = tel_keep(g_tel.root_buf, sizeof g_tel.root_buf,
                           paths_file(AGENT_DIR_STATE, STR("telemetry"), scratch));
    scratch->off = mark;
    if (!g_tel.ready) return;

    /* Enough to tell two sessions of the same second apart, and nothing that
     * says whose they are: a pid and two clocks, hashed. */
    time_t now = time(NULL);
    char seed[64];
    i32 n = snprintf(seed, sizeof seed, "%ld:%ld:%f", (long)getpid(),
                     (long)now, g_tel.t0);
    g_tel.run = str_hash64((Str){ seed, n > 0 ? (size_t)n : 0 });

    struct tm tm;
    char stamp[24] = "00000000-000000";
    if (localtime_r(&now, &tm))
        snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    n = snprintf(g_tel.run_stem, sizeof g_tel.run_stem, "%s-%016llx", stamp,
                 (unsigned long long)g_tel.run);
    if (n <= 0 || (size_t)n >= sizeof g_tel.run_stem) {
        g_tel.ready = false;
        return;
    }
    g_tel.header_due = true;
    g_tel.on = on;
}

b8 telemetry_set(b8 on, Arena *scratch) {
    if (!g_tel.ready) return false;
    g_tel.on = on;
    if (on) g_tel.header_due = true;
    return conf_remember_bool(CONF_TELEMETRY, on, scratch);
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
    if (g_tel.header_due && g_tel.header) {
        g_tel.header_due = false;   /* before the call, which records too */
        g_tel.header(g_tel.header_ud);
    }
    char head[96];
    u64 ms = (u64)((agent_now_seconds() - g_tel.t0) * 1000.0);
    /* The run id names the file, so a line carries only its place in it. */
    i32 n = snprintf(head, sizeof head, "{\"t\":%llu,\"seq\":%llu,\"ev\":\"%s\"",
                     (unsigned long long)ms,
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
    if (level < AGENT_LOG_DEBUG || level > AGENT_LOG_ERROR) level = AGENT_LOG_ERROR;
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
                     (unsigned long long)str_hash64(v));
    if (n > 0) tel_str(e, key, (Str){ hex, (size_t)n });
}
