/* session.c: persistent conversations, one set per working directory.
 *
 * A session is the conversation as it happened: a JSON object per line in
 * $XDG_DATA_HOME/yoke/sessions/<cwd>/<timestamp>.jsonl, appended as messages
 * are produced rather than flushed at exit, because the session worth resuming
 * is often the one that ended badly.
 *
 * Sessions are keyed by the directory yoke was launched in, because that is
 * the unit of work: browsing them from ~/src/foo must not surface what was
 * said in ~/src/bar. The key is the absolute cwd, percent-encoded so it is
 * one path component and still readable, with a hash appended when the
 * encoding is too long to keep whole.
 *
 * Paths live in the struct rather than in an arena: /clear rewinds the
 * session arena, and the file the next message appends to has to survive
 * that.
 */
#include "yoke.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SESSION_SLUG_MAX 200      /* encoded cwd kept whole up to this        */
#define SESSION_PREVIEW_BYTES 60  /* of the first prompt, shown when browsing */
#define SESSION_PREVIEW_READ 8192 /* bytes of a file scanned for that prompt  */

static b8 sess_unreserved(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

static u64 sess_hash(Str s) {
    u64 h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < s.n; i++) {
        h ^= (u8)s.p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* One path component naming `path`. Percent-encoding keeps it reversible by
 * eye; a path too long to encode whole is cut at an escape boundary and
 * disambiguated by a hash of the original, so two long neighbours can never
 * collapse onto the same directory. */
static size_t sess_slug(char *out, size_t cap, Str path) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    b8 truncated = false;
    for (size_t i = 0; i < path.n; i++) {
        char c = path.p[i];
        size_t need = sess_unreserved(c) ? 1u : 3u;
        if (n + need > SESSION_SLUG_MAX || n + need + 1 > cap) {
            truncated = true;
            break;
        }
        if (need == 1) out[n++] = c;
        else {
            out[n++] = '%';
            out[n++] = hex[((u8)c >> 4) & 0xf];
            out[n++] = hex[(u8)c & 0xf];
        }
    }
    if (truncated) {
        u64 h = sess_hash(path);
        if (n + 17 >= cap) n = cap > 18 ? cap - 18 : 0;
        out[n++] = '-';
        for (i32 shift = 60; shift >= 0; shift -= 4)
            out[n++] = hex[(h >> shift) & 0xf];
    }
    if (!n && cap > 1) out[n++] = '_';
    out[n] = '\0';
    return n;
}

/* Resolve the per-cwd session directory. `scratch` only holds the XDG base
 * for the length of the call; the result is copied into the struct. */
b8 session_init(Session *s, Arena *scratch) {
    memset(s, 0, sizeof *s);
    Str base = paths_dir(YOKE_DIR_DATA, scratch);
    if (!base.n) return false;
    char cwd[YOKE_MAX_PATH];
    if (!getcwd(cwd, sizeof cwd)) return false;
    char slug[SESSION_SLUG_MAX + 32];
    size_t slug_n = sess_slug(slug, sizeof slug, str_c(cwd));

    i32 n = snprintf(s->dir_buf, sizeof s->dir_buf, "%.*s/sessions/%.*s",
                     (i32)base.n, base.p, (i32)slug_n, slug);
    if (n <= 0 || (size_t)n >= sizeof s->dir_buf) { s->dir_buf[0] = '\0'; return false; }
    s->dir = (Str){ s->dir_buf, (size_t)n };
    return true;
}

static void sess_set_current(Session *s, Str path, Str name) {
    size_t pn = path.n < sizeof s->path_buf - 1 ? path.n : 0;
    size_t nn = name.n < sizeof s->name_buf - 1 ? name.n : 0;
    if (!pn) { s->path = (Str){0}; s->name = (Str){0}; return; }
    memcpy(s->path_buf, path.p, pn);
    s->path_buf[pn] = '\0';
    s->path = (Str){ s->path_buf, pn };
    memcpy(s->name_buf, name.p, nn);
    s->name_buf[nn] = '\0';
    s->name = (Str){ s->name_buf, nn };
}

/* "20250607-134501" becomes "2025-06-07 13:45:01"; anything else is shown raw. */
static Str sess_label(Arena *a, Str file) {
    Str stem = file;
    if (stem.n > 6 && !memcmp(stem.p + stem.n - 6, ".jsonl", 6)) stem.n -= 6;
    b8 stamp = stem.n == 15 && stem.p[8] == '-';
    for (size_t i = 0; stamp && i < stem.n; i++)
        if (i != 8 && (stem.p[i] < '0' || stem.p[i] > '9')) stamp = false;
    if (!stamp) return str_dup(a, stem);
    Buf b; buf_init(&b, a, 24);
    const char *p = stem.p;
    buf_putf(&b, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
             p, p + 4, p + 6, p + 9, p + 11, p + 13);
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

/* Start a fresh session file. The file itself is only created by the first
 * save, so an untouched session never shows up in the picker. */
b8 session_begin(Session *s) {
    s->written = 0;
    sess_set_current(s, (Str){0}, (Str){0});
    if (!s->dir.n) return false;
    time_t t = time(NULL);
    for (i32 tries = 0; tries < 64; tries++, t++) {
        struct tm tm;
        if (!localtime_r(&t, &tm)) return false;
        char file[32];
        i32 fn = snprintf(file, sizeof file, "%04d%02d%02d-%02d%02d%02d.jsonl",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (fn <= 0) return false;
        char path[YOKE_MAX_PATH];
        i32 pn = snprintf(path, sizeof path, "%.*s/%s",
                          (i32)s->dir.n, s->dir.p, file);
        if (pn <= 0 || (size_t)pn >= sizeof path) return false;
        if (access(path, F_OK) == 0) continue;   /* same second, twice */
        sess_set_current(s, (Str){ path, (size_t)pn }, str_c(file));
        return s->path.n != 0;
    }
    return false;
}

/* JSON-escaped straight to the stream: a message has no bound worth a buffer
 * and the escaping rules are the same ones buf_json_str applies. */
static void sess_put_json(FILE *f, Str s) {
    fputc('"', f);
    for (size_t i = 0; i < s.n; i++) {
        u8 c = (u8)s.p[i];
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\b': fputs("\\b", f); break;
            case '\f': fputs("\\f", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else fputc((i32)c, f);
        }
    }
    fputc('"', f);
}

/* Append the messages produced since the last save. The system prompt is
 * left out: it comes from the configuration, which may well have changed by
 * the time the session is resumed. */
void session_save(Session *s, const Conv *c) {
    if (!s->path.n || s->written >= c->n) return;
    Str dir = s->dir;
    if (dir.n) paths_ensure_dir(dir);
    FILE *f = fopen(s->path.p, "ab");
    if (!f) return;
    for (size_t i = s->written; i < c->n; i++) {
        if (c->role[i] == M_SYSTEM) continue;
        const char *role = c->role[i] == M_USER ? "user"
                         : c->role[i] == M_TOOL ? "tool" : "assistant";
        fprintf(f, "{\"role\":\"%s\"", role);
        if (c->tool_call_id[i].n) {
            fputs(",\"id\":", f);
            sess_put_json(f, c->tool_call_id[i]);
        }
        if (c->tool_name[i].n) {
            fputs(",\"name\":", f);
            sess_put_json(f, c->tool_name[i]);
        }
        if (c->role[i] == M_ASSISTANT && c->has_tool_call[i] && !c->tool_name[i].n)
            fputs(",\"calls\":true", f);
        fputs(",\"content\":", f);
        sess_put_json(f, c->text[i]);
        fputs("}\n", f);
    }
    fclose(f);
    s->written = c->n;
}

/* Read at most `max` bytes of a file into `a`. The size is measured before
 * anything is allocated, so a file that grew past what we will read can
 * never turn into an allocation we cannot satisfy. */
static Str sess_read(Arena *a, const char *path, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return (Str){0};
    fseek(f, 0, SEEK_END);
    i64 sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (i64)YOKE_MAX_SESSION_BYTES) { fclose(f); return (Str){0}; }
    size_t want = (size_t)sz < max ? (size_t)sz : max;
    char *buf = arena_new(a, char, want + 1);
    if (!buf) { fclose(f); return (Str){0}; }
    size_t rd = fread(buf, 1, want, f);
    fclose(f);
    buf[rd] = '\0';
    return (Str){ buf, rd };
}

/* Single-line preview of a session: its first user prompt, flattened. */
static Str sess_preview(Arena *a, const char *path) {
    size_t mark = a->off;
    Str src = sess_read(a, path, SESSION_PREVIEW_READ);
    Str out = {0};
    size_t start = 0;
    for (size_t i = 0; i <= src.n && !out.n; i++) {
        if (i != src.n && src.p[i] != '\n') continue;
        Str line = { src.p + start, i - start };
        start = i + 1;
        if (line.n < 2) continue;
        JVal *v = json_parse(a, line);
        if (!v) continue;
        const JVal *role = json_get(v, STR("role"));
        if (!role || role->type != J_STR || !str_eq(role->u.s, STR("user"))) continue;
        const JVal *content = json_get(v, STR("content"));
        if (!content || content->type != J_STR) continue;
        out = content->u.s;
    }
    if (!out.n) { a->off = mark; return (Str){0}; }

    /* Flatten to one line and cut on a UTF-8 boundary: this lands in a popup
     * row, so a stray newline or half a glyph would be a hole in the frame. */
    char tmp[SESSION_PREVIEW_BYTES + 8];
    size_t n = 0;
    for (size_t i = 0; i < out.n && n < SESSION_PREVIEW_BYTES; i++) {
        u8 c = (u8)out.p[i];
        tmp[n++] = c < 0x20 ? ' ' : (char)c;
    }
    while (n && ((u8)tmp[n - 1] & 0xc0u) == 0x80u) n--;
    b8 cut = out.n > n;
    a->off = mark;
    Buf b; buf_init(&b, a, n + 8);
    buf_put(&b, tmp, n);
    if (cut) buf_puts(&b, STR("..."));
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

/* Saved sessions for this directory, newest first. Names are timestamps, so
 * a descending sort on the file name is a sort on time. */
size_t session_list(const Session *s, Arena *a, SessionList *out, size_t max) {
    memset(out, 0, sizeof *out);
    if (!s->dir.n || !max) return 0;
    if (max > YOKE_MAX_SESSIONS) max = YOKE_MAX_SESSIONS;

    char names[YOKE_MAX_SESSIONS][64];
    size_t n = 0;
    DIR *d = opendir(s->dir.p);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        size_t len = strlen(e->d_name);
        if (len < 7 || len >= sizeof names[0]) continue;
        if (memcmp(e->d_name + len - 6, ".jsonl", 6) != 0) continue;
        size_t pos = n;
        while (pos > 0 && strcmp(names[pos - 1], e->d_name) < 0) pos--;
        if (pos >= max) continue;               /* older than everything kept */
        if (n < max) n++;
        for (size_t i = n - 1; i > pos; i--)
            memcpy(names[i], names[i - 1], sizeof names[0]);
        memcpy(names[pos], e->d_name, len + 1);
    }
    closedir(d);
    if (!n) return 0;

    out->name    = arena_new(a, Str, n);
    out->path    = arena_new(a, Str, n);
    out->preview = arena_new(a, Str, n);
    if (!out->name || !out->path || !out->preview) return 0;
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        Buf b; buf_init(&b, a, s->dir.n + sizeof names[0] + 2);
        buf_puts(&b, s->dir);
        buf_putc(&b, '/');
        buf_puts(&b, str_c(names[i]));
        if (!buf_ok(&b)) continue;
        Str path = buf_finish(&b);
        Str label = sess_label(a, str_c(names[i]));
        if (!label.n) continue;
        out->path[kept] = path;
        out->name[kept] = label;
        out->preview[kept] = sess_preview(a, path.p);
        kept++;
    }
    out->n = kept;
    return kept;
}

/* Raw contents of a saved session, held in `scratch`. Reading is separate
 * from replaying because replaying rewinds the live conversation and
 * overwrites its storage: whether the file can be read at all has to be known
 * before anything is thrown away. */
Str session_read(Str path, Arena *scratch) {
    if (!path.n) return (Str){0};
    return sess_read(scratch, path.p, YOKE_MAX_SESSION_BYTES);
}

/* Replay contents into `c`, which the caller has already rewound to the
 * system prompt. Messages are copied into `persist`; `scratch` holds each
 * parsed line and is rewound to where it started. False means the
 * conversation filled up and holds only part of the session. */
b8 session_apply(Session *s, Str src, Str path, Str name, Conv *c,
                 Arena *persist, Arena *scratch) {
    sess_set_current(s, path, name);
    s->written = c->n;
    if (!src.n) return false;
    size_t mark = scratch->off;

    size_t start = 0;
    b8 ok = true;
    for (size_t i = 0; i <= src.n; i++) {
        if (i != src.n && src.p[i] != '\n') continue;
        Str line = { src.p + start, i - start };
        start = i + 1;
        if (line.n < 2) continue;
        size_t line_mark = scratch->off;
        JVal *v = json_parse(scratch, line);
        const JVal *role = v ? json_get(v, STR("role")) : NULL;
        if (!role || role->type != J_STR) { scratch->off = line_mark; continue; }
        const JVal *content = json_get(v, STR("content"));
        const JVal *id = json_get(v, STR("id"));
        const JVal *nm = json_get(v, STR("name"));
        const JVal *calls = json_get(v, STR("calls"));
        Str text = content && content->type == J_STR
                 ? str_dup(persist, content->u.s) : (Str){0};
        Str call_id = id && id->type == J_STR ? str_dup(persist, id->u.s) : (Str){0};
        Str tool = nm && nm->type == J_STR ? str_dup(persist, nm->u.s) : (Str){0};
        size_t slot;
        if (str_eq(role->u.s, STR("user"))) {
            slot = conv_add(c, M_USER, text);
        } else if (str_eq(role->u.s, STR("tool"))) {
            slot = conv_add_tool(c, call_id, text);
        } else if (tool.n) {
            slot = conv_add_call(c, call_id, tool, text);
        } else if (calls && calls->type == J_BOOL && calls->u.b) {
            slot = conv_add_assistant_calls(c, text);
        } else {
            slot = conv_add(c, M_ASSISTANT, text);
        }
        scratch->off = line_mark;
        if (slot == CONV_NONE) { ok = false; break; }
    }
    scratch->off = mark;
    s->written = c->n;
    return ok;
}
