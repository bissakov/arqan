#include "agent.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SESSION_PREVIEW_BYTES 60
#define SESSION_PREVIEW_READ  8192
#define SESSION_CLEARED_NAME  ".cleared"

static size_t sess_cleared_path(const Session *s, char *out, size_t cap) {
    if (!s->dir.n) return 0;
    i32 n = snprintf(out, cap, "%.*s/%s", (i32)s->dir.n, s->dir.p,
                     SESSION_CLEARED_NAME);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* Resolve the per-cwd session directory. `scratch` only holds the XDG base
 * for the length of the call; the result is copied into the struct. */
b8 session_init(Session *s, Arena *scratch) {
    memset(s, 0, sizeof *s);
    Str base = paths_dir(AGENT_DIR_DATA, scratch);
    if (!base.n) return false;
    char slug[AGENT_SLUG_MAX + 32];
    size_t slug_n = paths_cwd_slug(slug, sizeof slug);
    if (!slug_n) return false;

    i32 n = snprintf(s->dir_buf, sizeof s->dir_buf, "%.*s/sessions/%.*s",
                     (i32)base.n, base.p, (i32)slug_n, slug);
    if (n <= 0 || (size_t)n >= sizeof s->dir_buf) {
        s->dir_buf[0] = '\0';
        return false;
    }
    s->dir = (Str){s->dir_buf, (size_t)n};
    char mark[AGENT_MAX_PATH];
    s->cleared =
        sess_cleared_path(s, mark, sizeof mark) && access(mark, F_OK) == 0;
    return true;
}

void session_set_cleared(Session *s, b8 cleared) {
    char path[AGENT_MAX_PATH];
    if (!sess_cleared_path(s, path, sizeof path)) return;
    if (!cleared) {
        unlink(path);
        s->cleared = false;
        return;
    }
    if (!paths_ensure_dir(s->dir)) return;
    i32 fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    close(fd);
    s->cleared = true;
}

static void sess_set_current(Session *s, Str path, Str name) {
    size_t pn = path.n < sizeof s->path_buf - 1 ? path.n : 0;
    size_t nn = name.n < sizeof s->name_buf - 1 ? name.n : 0;
    s->title_buf[0] = '\0';
    s->title = (Str){0};
    if (!pn) {
        s->path = (Str){0};
        s->name = (Str){0};
        return;
    }
    memcpy(s->path_buf, path.p, pn);
    s->path_buf[pn] = '\0';
    s->path = (Str){s->path_buf, pn};
    memcpy(s->name_buf, name.p, nn);
    s->name_buf[nn] = '\0';
    s->name = (Str){s->name_buf, nn};
}


static Str sess_label(Arena *a, Str file) {
    Str stem = file;
    if (stem.n > 6 && !memcmp(stem.p + stem.n - 6, ".jsonl", 6)) stem.n -= 6;
    b8 stamp = stem.n == 15 && stem.p[8] == '-';
    for (size_t i = 0; stamp && i < stem.n; i++)
        if (i != 8 && (stem.p[i] < '0' || stem.p[i] > '9')) stamp = false;
    if (!stamp) return str_dup(a, stem);
    Buf b;
    buf_init(&b, a, 24);
    const char *p = stem.p;
    buf_putf(&b, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s", p, p + 4, p + 6, p + 9,
             p + 11, p + 13);
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

/* A session name from arbitrary text: the first line, unwrapped, flattened
 * to one plain line and cut on a UTF-8 boundary the way sess_preview cuts a
 * preview. Zero means "not a title", which every caller reads as absent, so
 * neither a model's stray formatting nor a hand-edited sidecar can put a
 * control byte or a second line into a popup row. */
static size_t sess_title_clean(char *out, size_t cap, Str src) {
    if (!cap) return 0;
    for (size_t i = 0; i < src.n; i++)
        if (src.p[i] == '\n' || src.p[i] == '\r') {
            src.n = i;
            break;
        }
    src = str_trim(src);

    for (size_t before = 0; before != src.n;) {
        before = src.n;
        while (src.n && (src.p[src.n - 1] == '.' || src.p[src.n - 1] == ' '))
            src.n--;
        if (src.n >= 2) {
            char q = src.p[0];
            if ((q == '"' || q == '\'' || q == '`') && src.p[src.n - 1] == q)
                src = str_trim((Str){src.p + 1, src.n - 2});
        }
    }
    size_t n = 0;
    size_t max = cap - 1 < AGENT_MAX_TITLE ? cap - 1 : AGENT_MAX_TITLE;
    for (size_t i = 0; i < src.n && n < max; i++) {
        char c = (u8)src.p[i] < 0x20 ? ' ' : src.p[i];
        if (c == ' ' && (!n || out[n - 1] == ' ')) continue;
        out[n++] = c;
    }

    size_t last = n;
    while (last && ((u8)out[last - 1] & 0xc0u) == 0x80u) last--;
    if (last) {
        u8 lead = (u8)out[last - 1];
        size_t len = lead < 0x80    ? 1
                     : lead >= 0xf0 ? 4
                     : lead >= 0xe0 ? 3
                     : lead >= 0xc0 ? 2
                                    : 0;
        if (!len || last - 1 + len > n) n = last - 1;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    out[n] = '\0';
    return n;
}


static Str sess_title_read(Str session_path, Arena *a) {
    size_t mark = a->off;
    Str src = {0};
    file_read(a, session_path.p, AGENT_MAX_SESSION_BYTES, 512, &src, NULL);
    size_t end = 0;
    while (end < src.n && src.p[end] != '\n') end++;
    JVal *v = json_parse(a, (Str){src.p, end});
    if (!str_eq(json_str(v, STR("type")), STR("session"))) {
        a->off = mark;
        return (Str){0};
    }
    char buf[AGENT_MAX_TITLE + 1];
    size_t n = sess_title_clean(buf, sizeof buf, json_str(v, STR("title")));
    a->off = mark;
    return n ? str_dup(a, (Str){buf, n}) : (Str){0};
}

static b8 sess_file_json(FILE *f, Str s) {
    if (fputc('"', f) == EOF) return false;
    for (size_t i = 0; i < s.n; i++) {
        const char *escaped = NULL;
        switch (s.p[i]) {
            case '"': escaped = "\\\""; break;
            case '\\': escaped = "\\\\"; break;
            case '\b': escaped = "\\b"; break;
            case '\f': escaped = "\\f"; break;
            case '\n': escaped = "\\n"; break;
            case '\r': escaped = "\\r"; break;
            case '\t': escaped = "\\t"; break;
            default: break;
        }
        if (escaped) {
            if (fputs(escaped, f) == EOF) return false;
        } else if (fputc((u8)s.p[i], f) == EOF) {
            return false;
        }
    }
    return fputc('"', f) != EOF;
}

static b8 sess_file_metadata(FILE *f, Str title) {
    return fputs("{\"type\":\"session\",\"title\":", f) != EOF
           && sess_file_json(f, title) && fputs("}\n", f) != EOF;
}

typedef struct {
    const char *path;
    Str title;
} SessTitleUpdate;

static b8 sess_title_replace(FILE *dst, void *ud) {
    const SessTitleUpdate *update = ud;
    if (!sess_file_metadata(dst, update->title)) return false;

    FILE *src = fopen(update->path, "rb");
    if (!src) return errno == ENOENT;
    char first[512];
    if (!fgets(first, sizeof first, src)) {
        b8 empty = feof(src) != 0;
        i32 saved = empty ? 0 : errno ? errno : EIO;
        if (fclose(src) != 0 && !saved) saved = errno;
        if (saved) errno = saved;
        return empty && !saved;
    }
    static const char prefix[] = "{\"type\":\"session\",\"title\":";
    if (!strchr(first, '\n')
        || strncmp(first, prefix, sizeof prefix - 1) != 0) {
        fclose(src);
        errno = EINVAL;
        return false;
    }

    char buf[8192];
    b8 ok = true;
    for (size_t n; (n = fread(buf, 1, sizeof buf, src)) != 0;)
        if (fwrite(buf, 1, n, dst) != n) {
            ok = false;
            break;
        }
    if (ferror(src)) ok = false;
    i32 saved = ok ? 0 : errno ? errno : EIO;
    if (fclose(src) != 0 && !saved) saved = errno;
    if (saved) errno = saved;
    return !saved;
}

b8 session_set_title(Session *s, Str title) {
    if (!s->path.n) return false;
    char clean[AGENT_MAX_TITLE + 1];
    size_t n = sess_title_clean(clean, sizeof clean, title);
    SessTitleUpdate update = {s->path.p, {clean, n}};
    if (!file_write_atomic(s->path.p, 0600, true, sess_title_replace, &update))
        return false;
    memcpy(s->title_buf, clean, n + 1);
    s->title = n ? (Str){s->title_buf, n} : (Str){0};
    return true;
}


b8 session_begin(Session *s) {
    s->written = 0;
    s->elide_written = 0;
    s->save_blocked = false;
    s->sync_dir = false;

    s->title_tried = false;
    /* The name is reserved here, but the conversation starts with its first
     * message: the record follows the file rather than the reservation, so
     * whatever happens in between waits for the session that claims it. */
    telemetry_detach();
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
        char path[AGENT_MAX_PATH];
        i32 pn = snprintf(path, sizeof path, "%.*s/%s", (i32)s->dir.n, s->dir.p,
                          file);
        if (pn <= 0 || (size_t)pn >= sizeof path) return false;
        if (access(path, F_OK) == 0) continue;
        sess_set_current(s, (Str){path, (size_t)pn}, str_c(file));
        return s->path.n != 0;
    }
    return false;
}


/* Persist a directory entry before anything durable refers to it. */
static b8 sess_sync_dir(Str dir, char *err, size_t err_cap) {
    char path[AGENT_MAX_PATH];
    if (dir.n >= sizeof path) {
        snprintf(err, err_cap, "session directory path is too long");
        return false;
    }
    memcpy(path, dir.p, dir.n);
    path[dir.n] = '\0';
    i32 fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        snprintf(err, err_cap, "could not open session directory: %s",
                 strerror(errno));
        return false;
    }
    if (fsync(fd) != 0) {
        i32 saved = errno;
        close(fd);
        snprintf(err, err_cap, "could not sync session directory: %s",
                 strerror(saved));
        return false;
    }
    if (close(fd) != 0) {
        snprintf(err, err_cap, "could not close session directory: %s",
                 strerror(errno));
        return false;
    }
    return true;
}

/* ---- media sidecars ------------------------------------------------------
 * An image is written beside the session rather than into it: base64 in the
 * line would multiply the file by four thirds of the image and put every
 * resume against AGENT_MAX_SESSION_BYTES. The name is the content's hash, so
 * the same image pasted twice costs one file and a re-save writes none.
 */

static b8 sess_media_name(char *rel, size_t cap, const MediaSet *m, size_t id) {
    if (!media_live(m, id)) {
        if (!m->file[id].n || m->file[id].n >= cap) return false;
        memcpy(rel, m->file[id].p, m->file[id].n);
        rel[m->file[id].n] = '\0';
        return true;
    }
    Str ext = media_ext(m->mime[id]);
    i32 rn = snprintf(rel, cap, "media/%016llx.%.*s",
                      (unsigned long long)str_hash64(m->bytes[id]), (i32)ext.n,
                      ext.p);
    return rn > 0 && (size_t)rn < cap;
}

static b8 sess_media_write(Str dir, const MediaSet *m, size_t id,
                           const char *rel, char *err, size_t err_cap) {
    if (!media_live(m, id)) {
        snprintf(err, err_cap, "session image is unavailable");
        return false;
    }
    char sub[AGENT_MAX_PATH];
    i32 sn = snprintf(sub, sizeof sub, "%.*s/media", (i32)dir.n, dir.p);
    if (sn <= 0 || (size_t)sn >= sizeof sub) {
        snprintf(err, err_cap, "session media path is too long");
        return false;
    }
    if (!paths_ensure_dir((Str){sub, (size_t)sn})) {
        snprintf(err, err_cap, "could not create session media directory");
        return false;
    }
    char path[AGENT_MAX_PATH];
    i32 pn = snprintf(path, sizeof path, "%.*s/%s", (i32)dir.n, dir.p, rel);
    if (pn <= 0 || (size_t)pn >= sizeof path) {
        snprintf(err, err_cap, "session media path is too long");
        return false;
    }

    if (access(path, F_OK) == 0)
        return sess_sync_dir((Str){sub, (size_t)sn}, err, err_cap);
    if (errno != ENOENT) {
        snprintf(err, err_cap, "could not inspect session image: %s",
                 strerror(errno));
        return false;
    }
    if (!file_write_atomic_str(path, m->bytes[id], 0666, true)) {
        i32 saved = errno;
        snprintf(err, err_cap, "could not write session image: %s",
                 strerror(saved ? saved : EIO));
        return false;
    }
    return true;
}


static b8 sess_write_all(i32 fd, const char *p, size_t n) {
    for (size_t off = 0; off < n;) {
        ssize_t wr = write(fd, p + off, n - off);
        if (wr < 0 && errno == EINTR) continue;
        if (wr <= 0) {
            if (!wr) errno = EIO;
            return false;
        }
        off += (size_t)wr;
    }
    return true;
}

typedef struct {
    i32 fd;
    i32 error;
    char buf[4096];
    size_t n;
} SessOut;

static b8 sess_out_flush(SessOut *o) {
    if (o->error) return false;
    if (o->n && !sess_write_all(o->fd, o->buf, o->n)) {
        o->error = errno ? errno : EIO;
        return false;
    }
    o->n = 0;
    return true;
}

static void sess_out_put(SessOut *o, const void *src, size_t n) {
    const char *p = src;
    while (n && !o->error) {
        size_t avail = sizeof o->buf - o->n;
        if (!avail) {
            if (!sess_out_flush(o)) return;
            avail = sizeof o->buf;
        }
        size_t take = n < avail ? n : avail;
        memcpy(o->buf + o->n, p, take);
        o->n += take;
        p += take;
        n -= take;
    }
}

static void sess_out_putc(SessOut *o, char c) {
    sess_out_put(o, &c, 1);
}

static void sess_out_puts(SessOut *o, Str s) {
    sess_out_put(o, s.p, s.n);
}

static void sess_out_putf(SessOut *o, const char *fmt, ...) {
    char tmp[96];
    va_list ap;
    va_start(ap, fmt);
    i32 n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        o->error = EOVERFLOW;
        return;
    }
    sess_out_put(o, tmp, (size_t)n);
}

static void sess_out_json(SessOut *o, Str s) {
    sess_out_putc(o, '"');
    size_t start = 0;
    for (size_t i = 0; i < s.n; i++) {
        const char *escaped = NULL;
        size_t escaped_n = 0;
        char control[7];
        switch (s.p[i]) {
            case '"':
                escaped = "\\\"";
                escaped_n = 2;
                break;
            case '\\':
                escaped = "\\\\";
                escaped_n = 2;
                break;
            case '\b':
                escaped = "\\b";
                escaped_n = 2;
                break;
            case '\f':
                escaped = "\\f";
                escaped_n = 2;
                break;
            case '\n':
                escaped = "\\n";
                escaped_n = 2;
                break;
            case '\r':
                escaped = "\\r";
                escaped_n = 2;
                break;
            case '\t':
                escaped = "\\t";
                escaped_n = 2;
                break;
            default:
                if ((u8)s.p[i] < 0x20) {
                    i32 n = snprintf(control, sizeof control, "\\u%04x",
                                     (u8)s.p[i]);
                    if (n != 6) {
                        o->error = EILSEQ;
                        return;
                    }
                    escaped = control;
                    escaped_n = 6;
                }
                break;
        }
        if (!escaped) continue;
        sess_out_put(o, s.p + start, i - start);
        sess_out_put(o, escaped, escaped_n);
        start = i + 1;
    }
    if (start < s.n) sess_out_put(o, s.p + start, s.n - start);
    sess_out_putc(o, '"');
}

static void sess_out_metadata(SessOut *o, Str title) {
    sess_out_puts(o, STR("{\"type\":\"session\",\"title\":"));
    sess_out_json(o, title);
    sess_out_puts(o, STR("}\n"));
}

static b8 sess_put_media(SessOut *o, Str dir, const Conv *c, size_t i,
                         char *err, size_t err_cap) {
    if (!c->media || !c->media_n[i]) return true;
    const MediaSet *m = c->media;
    sess_out_puts(o, STR(",\"media\":["));
    for (size_t k = 0; k < c->media_n[i]; k++) {
        size_t id = (size_t)c->media_off[i] + k;
        if (id >= m->n) {
            snprintf(err, err_cap, "session image index is invalid");
            return false;
        }
        char rel[64];
        b8 named = sess_media_name(rel, sizeof rel, m, id)
                   && (!media_live(m, id)
                       || sess_media_write(dir, m, id, rel, err, err_cap));
        if (!named) {
            if (!err[0])
                snprintf(err, err_cap,
                         "could not construct session media path");
            return false;
        }
        if (k) sess_out_putc(o, ',');
        sess_out_puts(o, STR("{\"mime\":"));
        sess_out_json(o, m->mime[id]);
        sess_out_puts(o, STR(",\"label\":"));
        sess_out_json(o, m->label[id]);
        sess_out_puts(o, STR(",\"file\":"));
        sess_out_json(o, str_c(rel));
        if (m->w[id] && m->h[id])
            sess_out_putf(o, ",\"w\":%u,\"h\":%u", m->w[id], m->h[id]);
        sess_out_putc(o, '}');
    }
    sess_out_putc(o, ']');
    return true;
}

/* Append the messages produced since the last save. The system prompt is
 * left out: it comes from the configuration, which may well have changed by
 * the time the session is resumed. */
b8 session_save(Session *s, const Conv *c, char *err, size_t err_cap) {
    if (err_cap) err[0] = '\0';
    if (s->save_blocked) {
        snprintf(err, err_cap,
                 "a previous failed append could not be rolled back");
        return false;
    }
    if (s->sync_dir) {
        if (!sess_sync_dir(s->dir, err, err_cap)) return false;
        s->sync_dir = false;
    }
    b8 elide_moved = c->elide_start != s->elide_written;
    if (s->written >= c->n && !elide_moved) return true;
    b8 pending = elide_moved;
    for (size_t i = s->written; i < c->n && !pending; i++)
        if (c->role[i] != M_SYSTEM) pending = true;
    if (!pending) {
        s->written = c->n;
        return true;
    }
    if (!s->path.n) {
        snprintf(err, err_cap, "session storage is unavailable");
        return false;
    }
    telemetry_bind(s->path);
    Str dir = s->dir;
    if (!dir.n || !paths_ensure_dir(dir)) {
        snprintf(err, err_cap, "could not create session directory");
        return false;
    }

    b8 created = false;
    i32 fd = open(s->path.p, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0)
        created = true;
    else if (errno == EEXIST)
        fd = open(s->path.p, O_RDWR);
    if (fd < 0) {
        snprintf(err, err_cap, "could not open session file: %s",
                 strerror(errno));
        return false;
    }
    off_t old_end = lseek(fd, 0, SEEK_END);
    if (old_end < 0) {
        i32 saved = errno;
        close(fd);
        if (created) unlink(s->path.p);
        snprintf(err, err_cap, "could not inspect session file: %s",
                 strerror(saved));
        return false;
    }
    char last = '\n';
    if (old_end > 0 && pread(fd, &last, 1, old_end - 1) != 1) {
        i32 saved = errno;
        close(fd);
        snprintf(err, err_cap, "could not inspect session file: %s",
                 strerror(saved ? saved : EIO));
        return false;
    }
    b8 newline = old_end > 0 && last != '\n';
    SessOut out = {.fd = fd};
    if (created) sess_out_metadata(&out, s->title);
    if (newline) sess_out_putc(&out, '\n');
    if (elide_moved)
        sess_out_putf(&out, "{\"type\":\"elide\",\"start\":%zu}\n",
                      c->elide_start);
    b8 serialized = true;
    for (size_t i = s->written; i < c->n && serialized; i++) {
        if (c->role[i] == M_SYSTEM) continue;
        const char *role = c->role[i] == M_USER   ? "user"
                           : c->role[i] == M_TOOL ? "tool"
                                                  : "assistant";
        sess_out_putf(&out, "{\"role\":\"%s\"", role);
        if (c->tool_call_id[i].n) {
            sess_out_puts(&out, STR(",\"id\":"));
            sess_out_json(&out, c->tool_call_id[i]);
        }
        if (c->tool_name[i].n) {
            sess_out_puts(&out, STR(",\"name\":"));
            sess_out_json(&out, c->tool_name[i]);
        }
        if (c->role[i] == M_ASSISTANT && c->has_tool_call[i]
            && !c->tool_name[i].n)
            sess_out_puts(&out, STR(",\"calls\":true"));
        if (c->anthropic_thinking[i].n) {
            sess_out_puts(&out, STR(",\"anthropic_thinking\":"));
            sess_out_puts(&out, c->anthropic_thinking[i]);
        }
        if (c->shell_out[i].n) {
            sess_out_puts(&out, STR(",\"output\":"));
            sess_out_json(&out, c->shell_out[i]);
        }
        serialized = sess_put_media(&out, dir, c, i, err, err_cap);
        if (!serialized) break;
        if (c->ms[i]) sess_out_putf(&out, ",\"ms\":%u", c->ms[i]);
        sess_out_puts(&out, STR(",\"content\":"));
        sess_out_json(&out, c->text[i]);
        sess_out_puts(&out, STR("}\n"));
    }
    b8 ok = serialized && sess_out_flush(&out) && fsync(fd) == 0;
    i32 saved = out.error ? out.error : ok ? 0 : errno;
    if (!ok) {
        b8 restored = ftruncate(fd, old_end) == 0 && fsync(fd) == 0;
        if (close(fd) != 0) restored = false;
        if (created && restored) unlink(s->path.p);
        if (!restored) s->save_blocked = true;
        if (!err[0])
            snprintf(err, err_cap, "could not write session file: %s",
                     strerror(saved ? saved : EIO));
        if (!restored) {
            size_t used = strlen(err);
            if (used < err_cap)
                snprintf(err + used, err_cap - used,
                         "; refusing further appends");
        }
        return false;
    }
    /* Durable bytes are a conversation to come back to, so whatever /clear
     * left behind no longer describes this directory. */
    if (s->cleared) session_set_cleared(s, false);
    if (close(fd) != 0) {
        saved = errno;
        s->written = c->n;
        s->elide_written = c->elide_start;
        if (created) s->sync_dir = true;
        snprintf(err, err_cap, "could not close session file: %s",
                 strerror(saved));
        return false;
    }
    s->written = c->n;
    s->elide_written = c->elide_start;
    if (created) {
        s->sync_dir = true;
        if (!sess_sync_dir(dir, err, err_cap)) { return false; }
        s->sync_dir = false;
    }
    return true;
}


static void sess_rebind(Session *s) {
    if (s->dir.n) s->dir.p = s->dir_buf;
    if (s->path.n) s->path.p = s->path_buf;
    if (s->name.n) s->name.p = s->name_buf;
    if (s->title.n) s->title.p = s->title_buf;
}

b8 session_fork(Session *s, const Conv *c, char *err, size_t err_cap) {
    Session old = *s;
    if (!session_begin(s)) {
        snprintf(err, err_cap, "could not reserve a session path");
        *s = old;
        sess_rebind(s);
        return false;
    }
    if (!session_save(s, c, err, err_cap)) {
        *s = old;
        sess_rebind(s);
        return false;
    }
    if (old.title.n) session_set_title(s, (Str){old.title_buf, old.title.n});
    return true;
}

static b8 export_put(FILE *f, Str s) {
    return !s.n || fwrite(s.p, 1, s.n, f) == s.n;
}

static b8 export_text_section(FILE *f, const char *heading, Str text) {
    if (fprintf(f, "\n## %s\n\n", heading) < 0 || !export_put(f, text))
        return false;
    return text.n && text.p[text.n - 1] == '\n' ? true : fputc('\n', f) != EOF;
}


static size_t export_fence_len(Str body) {
    size_t longest = 0, run = 0;
    for (size_t i = 0; i < body.n; i++) {
        if (body.p[i] == '`') {
            run++;
            if (run > longest) longest = run;
        } else {
            run = 0;
        }
    }
    return longest < 3 ? 3 : longest + 1;
}

static b8 export_code(FILE *f, Str language, Str body) {
    size_t fence = export_fence_len(body);
    for (size_t i = 0; i < fence; i++)
        if (fputc('`', f) == EOF) return false;
    if (!export_put(f, language) || fputc('\n', f) == EOF
        || !export_put(f, body))
        return false;
    if ((!body.n || body.p[body.n - 1] != '\n') && fputc('\n', f) == EOF)
        return false;
    for (size_t i = 0; i < fence; i++)
        if (fputc('`', f) == EOF) return false;
    return fputc('\n', f) != EOF;
}

static Str export_call_name(const Conv *c, size_t result) {
    for (size_t i = result; i-- > 0;)
        if (conv_is_call(c, i)
            && str_eq(c->tool_call_id[i], c->tool_call_id[result]))
            return c->tool_name[i];
    return STR("tool");
}

static b8 export_tool_section(FILE *f, const char *kind, Str name, Str language,
                              Str body) {
    if (fprintf(f, "\n## %s", kind) < 0) return false;
    if (name.n) {
        if (fputs(": `", f) == EOF || !export_put(f, name)
            || fputs("`", f) == EOF)
            return false;
    }
    return fputs("\n\n", f) != EOF && export_code(f, language, body);
}

static b8 export_markdown(FILE *f, const Conv *c) {
    if (fputs("# " AGENT_NAME " session\n", f) == EOF) return false;
    for (size_t i = 0; i < c->n; i++) {
        switch (c->role[i]) {
            case M_SYSTEM: break;
            case M_USER:
                if (conv_is_shell(c, i)) {
                    if (!export_tool_section(f, "Shell", (Str){0}, STR("sh"),
                                             c->text[i])
                        || !export_tool_section(f, "Shell output", (Str){0},
                                                STR("text"), c->shell_out[i]))
                        return false;
                } else if (!export_text_section(f, "User", c->text[i])) {
                    return false;
                }
                break;
            case M_ASSISTANT:
                if (conv_is_call(c, i)) {
                    if (!export_tool_section(f, "Tool call", c->tool_name[i],
                                             STR("json"), c->text[i]))
                        return false;
                } else if (c->text[i].n
                           && !export_text_section(f, "Assistant",
                                                   c->text[i])) {
                    return false;
                }
                break;
            case M_TOOL:
                if (!export_tool_section(f, "Tool result",
                                         export_call_name(c, i), STR("text"),
                                         c->text[i]))
                    return false;
                break;
        }
    }
    return !ferror(f);
}

static b8 export_atomic(FILE *f, void *ud) {
    return export_markdown(f, ud);
}

static b8 export_auto_path(char *path, size_t cap, char *err, size_t err_cap) {
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm)) {
        snprintf(err, err_cap, "could not create an automatic file name");
        return false;
    }
    char stamp[32];
    i32 n = snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec);
    if (n <= 0 || (size_t)n >= sizeof stamp) return false;
    for (i32 suffix = 1; suffix <= 999; suffix++) {
        n = suffix == 1
                ? snprintf(path, cap, AGENT_NAME "-session-%s.md", stamp)
                : snprintf(path, cap, AGENT_NAME "-session-%s-%d.md", stamp,
                           suffix);
        if (n <= 0 || (size_t)n >= cap) break;
        if (access(path, F_OK) != 0) {
            if (errno == ENOENT) return true;
            snprintf(err, err_cap, "could not inspect export path: %s",
                     strerror(errno));
            return false;
        }
    }
    snprintf(err, err_cap, "could not choose an unused export file name");
    return false;
}

b8 session_export_markdown(const Conv *c, Str requested, char *path,
                           size_t path_cap, char *err, size_t err_cap) {
    requested = str_trim(requested);
    if (requested.n) {
        if (requested.n >= path_cap) {
            snprintf(err, err_cap, "export path is too long");
            return false;
        }
        memcpy(path, requested.p, requested.n);
        path[requested.n] = '\0';
    } else if (!export_auto_path(path, path_cap, err, err_cap)) {
        return false;
    }

    if (file_write_atomic(path, 0666, true, export_atomic, (void *)c))
        return true;
    snprintf(err, err_cap, "could not write export file: %s", strerror(errno));
    return false;
}

/* Single-line preview of a session: its first user prompt, flattened. Only
 * the head of the file is read, since the prompt it wants is its first. */
static Str sess_preview(Arena *a, const char *path) {
    size_t mark = a->off;
    Str src = {0};
    file_read(a, path, AGENT_MAX_SESSION_BYTES, SESSION_PREVIEW_READ, &src,
              NULL);
    Str out = {0};
    size_t start = 0;
    for (size_t i = 0; i <= src.n && !out.n; i++) {
        if (i != src.n && src.p[i] != '\n') continue;
        Str line = {src.p + start, i - start};
        start = i + 1;
        if (line.n < 2) continue;
        JVal *v = json_parse(a, line);
        if (!str_eq(json_str(v, STR("role")), STR("user"))) continue;
        out = json_str(v, STR("content"));
    }
    if (!out.n) {
        a->off = mark;
        return (Str){0};
    }


    char tmp[SESSION_PREVIEW_BYTES + 8];
    size_t n = 0;
    for (size_t i = 0; i < out.n && n < SESSION_PREVIEW_BYTES; i++) {
        u8 c = (u8)out.p[i];
        tmp[n++] = c < 0x20 ? ' ' : (char)c;
    }
    while (n && ((u8)tmp[n - 1] & 0xc0u) == 0x80u) n--;
    b8 cut = out.n > n;
    a->off = mark;
    Buf b;
    buf_init(&b, a, n + 8);
    buf_put(&b, tmp, n);
    if (cut) buf_puts(&b, STR("..."));
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}


typedef struct {
    char name[64];
    time_t sec;
    long nsec;
} SessEntry;

/* Whether `e` sorts before a candidate, which is what an insertion walks
 * back over. Last written first, because the session a reader wants back is
 * the one they were last in, not the one that happens to have been started
 * most recently. A file system too coarse to tell two writes apart leaves
 * the names to break the tie, and those are creation timestamps. */
static b8 sess_before(const SessEntry *e, const SessEntry *cand) {
    if (e->sec != cand->sec) return e->sec > cand->sec;
    if (e->nsec != cand->nsec) return e->nsec > cand->nsec;
    return strcmp(e->name, cand->name) > 0;
}


size_t session_list(const Session *s, Arena *a, SessionList *out, size_t max) {
    memset(out, 0, sizeof *out);
    if (!s->dir.n || !max) return 0;
    if (max > AGENT_MAX_SESSIONS) max = AGENT_MAX_SESSIONS;

    SessEntry ents[AGENT_MAX_SESSIONS];
    size_t n = 0;
    DIR *d = opendir(s->dir.p);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        size_t len = strlen(e->d_name);
        if (len < 7 || len >= sizeof ents[0].name) continue;
        if (memcmp(e->d_name + len - 6, ".jsonl", 6) != 0) continue;
        /* A file that cannot be stat'ed still belongs in the list; it sorts
         * as the oldest rather than disappearing from it. */
        SessEntry cand = {{0}, 0, 0};
        memcpy(cand.name, e->d_name, len + 1);
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, 0) == 0) {
            cand.sec = st.st_mtim.tv_sec;
            cand.nsec = st.st_mtim.tv_nsec;
        }
        size_t pos = n;
        while (pos > 0 && !sess_before(&ents[pos - 1], &cand)) pos--;
        if (pos >= max) continue;
        if (n < max) n++;
        for (size_t i = n - 1; i > pos; i--) ents[i] = ents[i - 1];
        ents[pos] = cand;
    }
    closedir(d);
    if (!n) return 0;

    out->name = arena_new(a, Str, n);
    out->path = arena_new(a, Str, n);
    out->preview = arena_new(a, Str, n);
    out->title = arena_new(a, Str, n);
    if (!out->name || !out->path || !out->preview || !out->title) return 0;
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        Buf b;
        buf_init(&b, a, s->dir.n + sizeof ents[0].name + 2);
        buf_puts(&b, s->dir);
        buf_putc(&b, '/');
        buf_puts(&b, str_c(ents[i].name));
        if (!buf_ok(&b)) continue;
        Str path = buf_finish(&b);
        Str label = sess_label(a, str_c(ents[i].name));
        if (!label.n) continue;
        out->path[kept] = path;
        out->name[kept] = label;
        out->preview[kept] = sess_preview(a, path.p);
        out->title[kept] = sess_title_read(path, a);
        kept++;
    }
    out->n = kept;
    return kept;
}

/* Remove one saved session. The path is checked against this session's own
 * directory rather than trusted: a delete reaches the file system, so it may
 * only ever name a file the picker listed, one component below the directory
 * this cwd owns. The live file is refused because it is still being appended
 * to, and a conversation that outlives its own record is worse than one the
 * reader has to end before removing. */
b8 session_delete(const Session *s, Str path) {
    if (!s->dir.n || path.n <= s->dir.n + 1 || path.n >= AGENT_MAX_PATH)
        return false;
    if (memcmp(path.p, s->dir.p, s->dir.n) || path.p[s->dir.n] != '/')
        return false;
    Str file = str_drop(path, s->dir.n + 1);
    if (memchr(file.p, '/', file.n) || str_eq(file, STR(".."))) return false;
    if (s->path.n && str_eq(path, s->path)) return false;
    return unlink(path.p) == 0;
}

/* Raw contents of a saved session, held in `scratch`. Reading is separate
 * from replaying because replaying rewinds the live conversation and
 * overwrites its storage: whether the file can be read at all has to be known
 * before anything is thrown away. */
Str session_read(Str path, Arena *scratch) {
    Str src = {0};
    if (path.n)
        file_read(scratch, path.p, AGENT_MAX_SESSION_BYTES, 0, &src, NULL);
    return src;
}

/* One string field of a saved line, copied into the conversation's arena.
 * Absent and empty are the same answer, so neither costs an allocation. */
static Str sess_field(Arena *persist, const JVal *v, Str key) {
    return str_dup_opt(persist, json_str(v, key));
}


static Str sess_thinking(Arena *persist, const JVal *v) {
    const JVal *blocks = json_get(v, STR("anthropic_thinking"));
    if (!blocks || blocks->type != J_ARR || !blocks->u.arr.n) return (Str){0};
    for (size_t i = 0; i < blocks->u.arr.n; i++) {
        const JVal *blk = &blocks->u.arr.items[i];
        if (blk->type != J_OBJ) return (Str){0};
        Str kind = json_str(blk, STR("type"));
        if (str_eq(kind, STR("thinking"))) {
            const JVal *thought = json_get(blk, STR("thinking"));
            const JVal *signature = json_get(blk, STR("signature"));
            if (!thought || thought->type != J_STR || !signature
                || signature->type != J_STR)
                return (Str){0};
        } else if (str_eq(kind, STR("redacted_thinking"))) {
            const JVal *data = json_get(blk, STR("data"));
            if (!data || data->type != J_STR) return (Str){0};
        } else {
            return (Str){0};
        }
    }
    Buf out;
    buf_init(&out, persist, 1024);
    json_write(&out, blocks);
    return buf_ok(&out) ? buf_finish(&out) : (Str){0};
}


static b8 sess_media_path(char *out, size_t cap, Str dir, Str file) {
    if (!str_starts(file, STR("media/"))) return false;
    Str name = str_drop(file, 6);
    if (!name.n || name.p[0] == '.') return false;
    for (size_t i = 0; i < name.n; i++)
        if (name.p[i] == '/' || (u8)name.p[i] < 0x20) return false;
    i32 n = snprintf(out, cap, "%.*s/media/%.*s", (i32)dir.n, dir.p,
                     (i32)name.n, name.p);
    return n > 0 && (size_t)n < cap;
}


static void sess_apply_media(const Session *s, const JVal *v, Conv *c,
                             size_t slot, Arena *persist, Arena *scratch) {
    const JVal *arr = json_get(v, STR("media"));
    if (!c->media || !arr || arr->type != J_ARR || !arr->u.arr.n) return;
    size_t off = c->media->n, kept = 0;
    for (size_t i = 0; i < arr->u.arr.n && kept < AGENT_MAX_MEDIA_PER_TURN;
         i++) {
        const JVal *e = &arr->u.arr.items[i];
        if (e->type != J_OBJ) continue;
        Str file = json_str(e, STR("file"));
        Str label = json_str(e, STR("label"));
        char path[AGENT_MAX_PATH], err[128];
        size_t id = MEDIA_NONE;
        if (sess_media_path(path, sizeof path, s->dir, file))
            id = media_add_file(c->media, persist, scratch, str_c(path), err,
                                sizeof err);
        if (id != MEDIA_NONE && label.n)
            c->media->label[id] = str_dup(persist, label);
        if (id == MEDIA_NONE)
            id = media_add_missing(c->media, persist, label,
                                   json_str(e, STR("mime")), file);
        if (id == MEDIA_NONE) break;
        kept++;
    }
    if (kept) conv_attach_media(c, slot, off, kept);
}


static b8 sess_call_answered(const Conv *c, size_t call) {
    for (size_t i = call + 1; i < c->n; i++)
        if (c->role[i] == M_TOOL
            && str_eq(c->tool_call_id[i], c->tool_call_id[call]))
            return true;
    return false;
}


static b8 sess_answer_pending(Conv *c) {
    size_t n = c->n;
    for (size_t i = 0; i < n; i++) {
        if (!conv_is_call(c, i) || sess_call_answered(c, i)) continue;
        if (conv_add_tool(c, c->tool_call_id[i],
                          STR("ERROR: interrupted before this call ran. "
                              "Call it again if it is still needed."))
            == CONV_NONE)
            return false;
    }
    return true;
}

/* Replay contents into `c`, which the caller has already rewound to the
 * system prompt. Messages are copied into `persist`; `scratch` holds each
 * parsed line and is rewound to where it started. False means the
 * conversation filled up and holds only part of the session. */
b8 session_apply(Session *s, Str src, Str path, Str name, Conv *c,
                 Arena *persist, Arena *scratch) {
    sess_set_current(s, path, name);
    telemetry_bind(s->path);
    s->written = c->n;
    if (s->cleared) session_set_cleared(s, false);

    {
        size_t mark = scratch->off;
        Str title = sess_title_read(s->path, scratch);
        size_t n = sess_title_clean(s->title_buf, sizeof s->title_buf, title);
        s->title = n ? (Str){s->title_buf, n} : (Str){0};
        s->title_tried = n != 0;
        scratch->off = mark;
    }
    if (!src.n) return false;
    size_t mark = scratch->off;

    size_t start = 0, elide = 0;
    b8 ok = true;
    for (size_t i = 0; i <= src.n; i++) {
        if (i != src.n && src.p[i] != '\n') continue;
        Str line = {src.p + start, i - start};
        start = i + 1;
        if (line.n < 2) continue;
        size_t line_mark = scratch->off;
        JVal *v = json_parse(scratch, line);
        Str role = json_str(v, STR("role"));
        if (!role.n) {
            if (str_eq(json_str(v, STR("type")), STR("elide"))) {
                const JVal *at = json_get(v, STR("start"));
                if (at && at->type == J_NUM && at->u.n > 0)
                    elide = (size_t)at->u.n;
            }
            scratch->off = line_mark;
            continue;
        }
        const JVal *ms = json_get(v, STR("ms"));
        Str text = sess_field(persist, v, STR("content"));
        Str call_id = sess_field(persist, v, STR("id"));
        Str tool = sess_field(persist, v, STR("name"));
        size_t slot;
        if (str_eq(role, STR("user"))) {
            Str out = sess_field(persist, v, STR("output"));
            slot = str_eq(tool, STR("shell")) ? conv_add_shell(c, text, out)
                                              : conv_add(c, M_USER, text);
        } else if (str_eq(role, STR("tool"))) {
            slot = conv_add_tool(c, call_id, text);
        } else if (tool.n) {
            slot = conv_add_call(c, scratch, call_id, tool, text);
        } else if (json_bool(v, STR("calls"))) {
            slot = conv_add_assistant_calls(c, text);
        } else {
            slot = conv_add(c, M_ASSISTANT, text);
        }
        if (slot != CONV_NONE && ms && ms->type == J_NUM && ms->u.n > 0)
            c->ms[slot] = ms->u.n > (f64)UINT32_MAX ? UINT32_MAX : (u32)ms->u.n;
        if (slot != CONV_NONE && c->role[slot] == M_USER)
            sess_apply_media(s, v, c, slot, persist, scratch);
        if (slot != CONV_NONE && c->role[slot] == M_ASSISTANT)
            c->anthropic_thinking[slot] = sess_thinking(persist, v);
        scratch->off = line_mark;
        if (slot == CONV_NONE) {
            ok = false;
            break;
        }
    }
    scratch->off = mark;
    /* A session that stopped short of what the marker names would elide text
     * it no longer holds, so the boundary follows what was replayed. */
    c->elide_start = elide < c->n ? elide : 0;
    s->written = c->n;
    s->elide_written = c->elide_start;
    /* The repair belongs to the file as much as to this conversation: saving
     * it now means the next resume of the same file finds it already whole,
     * rather than appending a new turn behind calls nothing answers. The
     * caller performs that save so it can report a persistence failure. */
    if (!sess_answer_pending(c)) ok = false;
    return ok;
}
