#include "agent.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SESSION_SLUG_MAX 200      // encoded cwd kept whole up to this
#define SESSION_PREVIEW_BYTES 60  // of the first prompt, shown when browsing
#define SESSION_PREVIEW_READ 8192 // bytes of a file scanned for that prompt

static b8 sess_unreserved(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
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
        u64 h = str_hash64(path);
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
    Str base = paths_dir(AGENT_DIR_DATA, scratch);
    if (!base.n) return false;
    char cwd[AGENT_MAX_PATH];
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
    s->title_buf[0] = '\0';
    s->title = (Str){0};
    if (!pn) { s->path = (Str){0}; s->name = (Str){0}; return; }
    memcpy(s->path_buf, path.p, pn);
    s->path_buf[pn] = '\0';
    s->path = (Str){ s->path_buf, pn };
    memcpy(s->name_buf, name.p, nn);
    s->name_buf[nn] = '\0';
    s->name = (Str){ s->name_buf, nn };
}

// "20250607-134501" becomes "2025-06-07 13:45:01"; anything else is shown raw.
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

/* A session name from arbitrary text: the first line, unwrapped, flattened
 * to one plain line and cut on a UTF-8 boundary the way sess_preview cuts a
 * preview. Zero means "not a title", which every caller reads as absent, so
 * neither a model's stray formatting nor a hand-edited sidecar can put a
 * control byte or a second line into a popup row. */
static size_t sess_title_clean(char *out, size_t cap, Str src) {
    if (!cap) return 0;
    for (size_t i = 0; i < src.n; i++)
        if (src.p[i] == '\n' || src.p[i] == '\r') { src.n = i; break; }
    src = str_trim(src);
    /* A model asked for a name answers with a quoted sentence often enough
     * that the quotes and the full stop are peeled together: the stop hides
     * the closing quote, so one pass would leave the opening one behind. */
    for (size_t before = 0; before != src.n;) {
        before = src.n;
        while (src.n && (src.p[src.n - 1] == '.' || src.p[src.n - 1] == ' '))
            src.n--;
        if (src.n >= 2) {
            char q = src.p[0];
            if ((q == '"' || q == '\'' || q == '`') && src.p[src.n - 1] == q)
                src = str_trim((Str){ src.p + 1, src.n - 2 });
        }
    }
    size_t n = 0;
    size_t max = cap - 1 < AGENT_MAX_TITLE ? cap - 1 : AGENT_MAX_TITLE;
    for (size_t i = 0; i < src.n && n < max; i++) {
        char c = (u8)src.p[i] < 0x20 ? ' ' : src.p[i];
        if (c == ' ' && (!n || out[n - 1] == ' ')) continue;
        out[n++] = c;
    }
    /* The cut lands on a UTF-8 boundary: a row of the picker showing half a
     * glyph is a hole in the frame. Only an incomplete last sequence goes. */
    size_t last = n;
    while (last && ((u8)out[last - 1] & 0xc0u) == 0x80u) last--;
    if (last) {
        u8 lead = (u8)out[last - 1];
        size_t len = lead < 0x80 ? 1 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3
                   : lead >= 0xc0 ? 2 : 0;
        if (!len || last - 1 + len > n) n = last - 1;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    out[n] = '\0';
    return n;
}

/* The sidecar beside a session file: its path with ".jsonl" replaced. A path
 * that is not a session file, or one that would not fit, has none. */
static size_t sess_title_path(char *out, size_t cap, Str session_path) {
    Str suffix = AGENT_TITLE_SUFFIX;
    if (session_path.n <= 6
        || memcmp(session_path.p + session_path.n - 6, ".jsonl", 6) != 0)
        return 0;
    size_t stem = session_path.n - 6;
    if (stem + suffix.n + 1 > cap) return 0;
    memcpy(out, session_path.p, stem);
    memcpy(out + stem, suffix.p, suffix.n);
    out[stem + suffix.n] = '\0';
    return stem + suffix.n;
}

Str session_title_read(Str session_path, Arena *a) {
    char path[AGENT_MAX_PATH];
    if (!sess_title_path(path, sizeof path, session_path)) return (Str){0};
    size_t mark = a->off;
    Str src = {0};
    file_read(a, path, AGENT_MAX_TITLE * 8, 0, &src, NULL);
    char buf[AGENT_MAX_TITLE + 1];
    size_t n = sess_title_clean(buf, sizeof buf, src);
    a->off = mark;
    return n ? str_dup(a, (Str){ buf, n }) : (Str){0};
}

b8 session_set_title(Session *s, Str title) {
    if (!s->path.n) return false;
    char path[AGENT_MAX_PATH];
    size_t n = sess_title_clean(s->title_buf, sizeof s->title_buf, title);
    s->title = n ? (Str){ s->title_buf, n } : (Str){0};
    if (!sess_title_path(path, sizeof path, s->path)) return false;
    if (!n) {
        unlink(path);           // an unnamed session leaves no sidecar behind
        return true;
    }
    return settings_write(str_c(path), s->title, 0600);
}

/* Start a fresh session file. The file itself is only created by the first
 * save, so an untouched session never shows up in the picker. */
b8 session_begin(Session *s) {
    s->written = 0;
    /* A reservation is a new thread, so the automatic name it has not earned
     * yet is still owed to it. */
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
        i32 pn = snprintf(path, sizeof path, "%.*s/%s",
                          (i32)s->dir.n, s->dir.p, file);
        if (pn <= 0 || (size_t)pn >= sizeof path) return false;
        if (access(path, F_OK) == 0) continue;   // same second, twice
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

/* Persist the directory entry itself, so a session file created just before
 * the power went is still named by its directory when the machine comes
 * back. Only the first save of a file needs it; later ones change no name. */
static void sess_sync_dir(Str dir) {
    char path[AGENT_MAX_PATH];
    if (dir.n >= sizeof path) return;
    memcpy(path, dir.p, dir.n);
    path[dir.n] = '\0';
    i32 fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    fsync(fd);
    close(fd);
}

/* End a line the last save left half-written. A power loss can cut an append
 * anywhere, and the torn line is dropped on load; without this the next
 * append would run onto its tail and cost the first good message after it as
 * well. `f` is open for append, so the write lands at the end regardless of
 * where reading left the stream. */
static void sess_close_line(FILE *f) {
    struct stat st;
    i32 fd = fileno(f);
    char last = '\n';
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) return;
    if (pread(fd, &last, 1, st.st_size - 1) != 1 || last == '\n') return;
    fputc('\n', f);
}

/* Append the messages produced since the last save. The system prompt is
 * left out: it comes from the configuration, which may well have changed by
 * the time the session is resumed. */
void session_save(Session *s, const Conv *c) {
    if (!s->path.n || s->written >= c->n) return;
    telemetry_bind(s->path);
    Str dir = s->dir;
    if (dir.n) paths_ensure_dir(dir);
    b8 created = access(s->path.p, F_OK) != 0;
    /* Read-write append: the file is only ever added to, but the byte before
     * the append has to be known. */
    FILE *f = fopen(s->path.p, "a+b");
    if (!f) return;
    sess_close_line(f);
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
        if (c->anthropic_thinking[i].n) {
            fputs(",\"anthropic_thinking\":", f);
            fwrite(c->anthropic_thinking[i].p, 1,
                   c->anthropic_thinking[i].n, f);
        }
        if (c->shell_out[i].n) {
            fputs(",\"output\":", f);
            sess_put_json(f, c->shell_out[i]);
        }
        /* How long the run behind the slot took, so a replayed transcript
         * says what the live one did. */
        if (c->ms[i]) fprintf(f, ",\"ms\":%u", c->ms[i]);
        fputs(",\"content\":", f);
        sess_put_json(f, c->text[i]);
        fputs("}\n", f);
    }
    /* A session is written to survive the machine losing power mid-turn, so
     * the lines reach the disk before the call returns rather than sitting in
     * the page cache behind whatever the next tool does. */
    if (fflush(f) == 0) fsync(fileno(f));
    fclose(f);
    if (created && dir.n) sess_sync_dir(dir);
    s->written = c->n;
}

/* Continue in a copy: a new file holding the conversation as it stands now,
 * which every later save appends to. The file left behind keeps exactly what
 * it had, so a fork costs the conversation it came from nothing. */
b8 session_fork(Session *s, const Conv *c) {
    /* A fork is the same thread continuing, so it carries the name over: the
     * title is copied out before the reservation clears it. */
    char title[AGENT_MAX_TITLE + 1];
    size_t title_n = s->title.n < sizeof title ? s->title.n : 0;
    if (title_n) memcpy(title, s->title.p, title_n);
    if (!session_begin(s)) return false;
    session_save(s, c);
    b8 ok = s->written >= c->n;
    if (title_n) session_set_title(s, (Str){ title, title_n });
    return ok;
}

static b8 export_put(FILE *f, Str s) {
    return !s.n || fwrite(s.p, 1, s.n, f) == s.n;
}

static b8 export_text_section(FILE *f, const char *heading, Str text) {
    if (fprintf(f, "\n## %s\n\n", heading) < 0 || !export_put(f, text))
        return false;
    return text.n && text.p[text.n - 1] == '\n' ? true : fputc('\n', f) != EOF;
}

/* A fence longer than any run in the body keeps arbitrary tool text from
 * closing its own block. */
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
    for (size_t i = 0; i < fence; i++) if (fputc('`', f) == EOF) return false;
    if (!export_put(f, language) || fputc('\n', f) == EOF || !export_put(f, body))
        return false;
    if ((!body.n || body.p[body.n - 1] != '\n') && fputc('\n', f) == EOF)
        return false;
    for (size_t i = 0; i < fence; i++) if (fputc('`', f) == EOF) return false;
    return fputc('\n', f) != EOF;
}

static Str export_call_name(const Conv *c, size_t result) {
    for (size_t i = result; i-- > 0;)
        if (conv_is_call(c, i)
            && str_eq(c->tool_call_id[i], c->tool_call_id[result]))
            return c->tool_name[i];
    return STR("tool");
}

static b8 export_tool_section(FILE *f, const char *kind, Str name,
                              Str language, Str body) {
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
            case M_SYSTEM:
                break;
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
                           && !export_text_section(f, "Assistant", c->text[i])) {
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

static b8 export_auto_path(char *path, size_t cap, char *err, size_t err_cap) {
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm)) {
        snprintf(err, err_cap, "could not create an automatic file name");
        return false;
    }
    char stamp[32];
    i32 n = snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (n <= 0 || (size_t)n >= sizeof stamp) return false;
    for (i32 suffix = 1; suffix <= 999; suffix++) {
        n = suffix == 1
          ? snprintf(path, cap, AGENT_NAME "-session-%s.md", stamp)
          : snprintf(path, cap, AGENT_NAME "-session-%s-%d.md", stamp, suffix);
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

b8 session_export_markdown(const Conv *c, Str requested,
                           char *path, size_t path_cap,
                           char *err, size_t err_cap) {
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

    char tmp[AGENT_MAX_PATH];
    i32 n = snprintf(tmp, sizeof tmp, "%s." AGENT_NAME "-tmp", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        snprintf(err, err_cap, "export path is too long");
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, err_cap, "could not open export file: %s", strerror(errno));
        return false;
    }
    b8 ok = export_markdown(f, c);
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) == 0) return true;
    i32 saved = errno;
    unlink(tmp);
    snprintf(err, err_cap, "could not write export file: %s", strerror(saved));
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
        Str line = { src.p + start, i - start };
        start = i + 1;
        if (line.n < 2) continue;
        JVal *v = json_parse(a, line);
        if (!str_eq(json_str(v, STR("role")), STR("user"))) continue;
        out = json_str(v, STR("content"));
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

/* One directory entry while the list is being ordered: the file name and
 * when it was last written. */
typedef struct {
    char   name[64];
    time_t sec;
    long   nsec;
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

/* Saved sessions for this directory, most recently written first. Only `max`
 * of them are kept, so a directory with a thousand files still costs one
 * pass and a fixed table. */
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
        if (pos >= max) continue;               // older than everything kept
        if (n < max) n++;
        for (size_t i = n - 1; i > pos; i--) ents[i] = ents[i - 1];
        ents[pos] = cand;
    }
    closedir(d);
    if (!n) return 0;

    out->name    = arena_new(a, Str, n);
    out->path    = arena_new(a, Str, n);
    out->preview = arena_new(a, Str, n);
    out->title   = arena_new(a, Str, n);
    if (!out->name || !out->path || !out->preview || !out->title) return 0;
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        Buf b; buf_init(&b, a, s->dir.n + sizeof ents[0].name + 2);
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
        /* Read back rather than remembered: the sidecar is a file a reader
         * may have edited, so it is cleaned again on the way into a row. */
        out->title[kept] = session_title_read(path, a);
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
    b8 ok = unlink(path.p) == 0;
    /* Best effort: a name left behind names nothing, and failing to remove
     * it does not change whether the session was removed. */
    char title[AGENT_MAX_PATH];
    if (sess_title_path(title, sizeof title, path)) unlink(title);
    return ok;
}

/* Raw contents of a saved session, held in `scratch`. Reading is separate
 * from replaying because replaying rewinds the live conversation and
 * overwrites its storage: whether the file can be read at all has to be known
 * before anything is thrown away. */
Str session_read(Str path, Arena *scratch) {
    Str src = {0};
    if (path.n) file_read(scratch, path.p, AGENT_MAX_SESSION_BYTES, 0, &src,
                          NULL);
    return src;
}

/* One string field of a saved line, copied into the conversation's arena.
 * Absent and empty are the same answer, so neither costs an allocation. */
static Str sess_field(Arena *persist, const JVal *v, Str key) {
    return str_dup_opt(persist, json_str(v, key));
}

/* Session files are editable external input. Only validated thinking block
 * arrays may later be spliced into an Anthropic request. Canonicalizing them
 * through the serializer also prevents raw JSON from becoming request syntax. */
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
            if (!thought || thought->type != J_STR
                || !signature || signature->type != J_STR) return (Str){0};
        } else if (str_eq(kind, STR("redacted_thinking"))) {
            const JVal *data = json_get(blk, STR("data"));
            if (!data || data->type != J_STR) return (Str){0};
        } else {
            return (Str){0};
        }
    }
    Buf out; buf_init(&out, persist, 1024);
    json_write(&out, blocks);
    return buf_ok(&out) ? buf_finish(&out) : (Str){0};
}

/* Whether any slot answers the call carried in `call`. */
static b8 sess_call_answered(const Conv *c, size_t call) {
    for (size_t i = call + 1; i < c->n; i++)
        if (c->role[i] == M_TOOL
            && str_eq(c->tool_call_id[i], c->tool_call_id[call]))
            return true;
    return false;
}

/* Answer the calls the session died between asking and running. The round
 * that asked is on disk before any tool starts, so a session cut mid-round
 * replays with calls no result follows, which every provider refuses. A
 * result saying the call never ran keeps the conversation valid and leaves
 * the model to decide whether the work is still wanted; nothing is replayed
 * as having run. False when there was no room to answer them all. */
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
    telemetry_bind(s->path);          // the record of the session reopened
    s->written = c->n;
    /* A resumed session is not a fresh one to name automatically, so a name
     * it already has ends the question. One that has none may still earn it
     * from the turn that follows. */
    {
        size_t mark = scratch->off;
        Str title = session_title_read(s->path, scratch);
        size_t n = sess_title_clean(s->title_buf, sizeof s->title_buf, title);
        s->title = n ? (Str){ s->title_buf, n } : (Str){0};
        s->title_tried = n != 0;
        scratch->off = mark;
    }
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
        Str role = json_str(v, STR("role"));
        if (!role.n) { scratch->off = line_mark; continue; }
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
            c->ms[slot] = ms->u.n > (f64)UINT32_MAX ? UINT32_MAX
                                                    : (u32)ms->u.n;
        if (slot != CONV_NONE && c->role[slot] == M_ASSISTANT)
            c->anthropic_thinking[slot] = sess_thinking(persist, v);
        scratch->off = line_mark;
        if (slot == CONV_NONE) { ok = false; break; }
    }
    scratch->off = mark;
    s->written = c->n;
    /* The repair belongs to the file as much as to this conversation: saving
     * it now means the next resume of the same file finds it already whole,
     * rather than appending a new turn behind calls nothing answers. */
    if (!sess_answer_pending(c)) ok = false;
    session_save(s, c);
    return ok;
}
