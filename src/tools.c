/* tools.c: SoA tool registry and the built-in tools.
 *
 * A ToolRun receives raw JSON args, a scratch arena the agent loop resets per
 * turn, an output Buf and an error buffer.
 */
#include "yoke.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ---- argument helpers ---------------------------------------------------- */
static Str json_get_str(const JVal *args, Str key) {
    const JVal *v = json_get(args, key);
    if (!v || v->type != J_STR) return (Str){0};
    return v->u.s;
}

/* Clamping instead would run a different command, or touch a different file,
 * than the one the model asked for and the user read. */
static b8 arg_cstr(Str s, char *z, size_t cap, const char *what,
                   char *err, size_t err_cap) {
    if (!s.p) { snprintf(err, err_cap, "missing %s", what); return false; }
    if (s.n >= cap) {
        snprintf(err, err_cap, "%s too long: %zu bytes, limit %zu",
                 what, s.n, cap - 1);
        return false;
    }
    if (memchr(s.p, '\0', s.n)) {
        snprintf(err, err_cap, "%s contains a nul byte", what);
        return false;
    }
    memcpy(z, s.p, s.n); z[s.n] = '\0';
    return true;
}

/* Every size here comes from the filesystem, so each is validated before it
 * reaches an allocation. */
static b8 slurp(const char *z, Arena *scratch, Str *out,
                char *err, size_t err_cap) {
    FILE *f = fopen(z, "rb");
    if (!f) { snprintf(err, err_cap, "open %s failed", z); return false; }
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f); snprintf(err, err_cap, "stat %s failed", z); return false;
    }
    if (!S_ISREG(st.st_mode)) {
        fclose(f); snprintf(err, err_cap, "%s is not a regular file", z);
        return false;
    }
    if ((u64)st.st_size > YOKE_MAX_FILE_BYTES) {
        fclose(f);
        snprintf(err, err_cap, "%s is too large: %llu bytes, limit %u",
                 z, (unsigned long long)st.st_size, (unsigned)YOKE_MAX_FILE_BYTES);
        return false;
    }
    size_t sz = (size_t)st.st_size;
    char *buf = arena_new(scratch, char, sz + 1);
    if (!buf) {
        fclose(f); snprintf(err, err_cap, "out of memory reading %s", z);
        return false;
    }
    size_t rd = fread(buf, 1, sz, f);
    b8 failed = ferror(f) != 0;
    fclose(f);
    if (failed) { snprintf(err, err_cap, "read %s failed", z); return false; }
    buf[rd] = '\0';
    *out = (Str){ buf, rd };
    return true;
}

/* `dflt` when absent. A fractional or negative count is named rather than
 * rounded: the caller asked for something this tool cannot do. */
static b8 arg_count(const JVal *j, Str key, size_t dflt, size_t max,
                    size_t *out, char *err, size_t err_cap) {
    const JVal *v = json_get(j, key);
    if (!v || v->type == J_NULL) { *out = dflt; return true; }
    if (v->type != J_NUM || v->u.n < 1 || v->u.n != (f64)(u64)v->u.n
        || v->u.n > (f64)max) {
        snprintf(err, err_cap, "%.*s must be a whole number in 1..%zu",
                 (i32)key.n, key.p, max);
        return false;
    }
    *out = (size_t)v->u.n;
    return true;
}

/* ---- read ----
 * A page of a file rather than the file, since a whole one is charged to
 * every later turn: the default stops at YOKE_READ_LINES or YOKE_READ_BYTES
 * and says which call continues from there. */
static b8 tool_read(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    char z[YOKE_MAX_PATH];
    if (!arg_cstr(json_get_str(j, STR("path")), z, sizeof z, "path", err, err_cap))
        return false;
    size_t first, limit;
    if (!arg_count(j, STR("offset"), 1, YOKE_MAX_FILE_BYTES, &first, err, err_cap))
        return false;
    if (!arg_count(j, STR("limit"), YOKE_READ_LINES, YOKE_MAX_FILE_BYTES,
                   &limit, err, err_cap))
        return false;

    Str body;
    if (!slurp(z, scratch, &body, err, err_cap)) return false;

    size_t off = 0;
    Str line;
    for (size_t ln = 1; ln < first; ln++) {
        if (!str_line(body, &off, &line)) {
            snprintf(err, err_cap, "%s has %zu lines, offset %zu is past its end",
                     z, ln - 1, first);
            return false;
        }
    }

    size_t start = off, shown = 0;
    while (shown < limit && off - start < YOKE_READ_BYTES
           && str_line(body, &off, &line))
        shown++;

    /* The line the byte cap lands in is dropped rather than halved, so the
     * next call resumes on a line boundary. A single line past the cap has no
     * boundary to fall back on and is cut at a UTF-8 lead byte. */
    b8 cut_mid_line = false;
    if (off - start > YOKE_READ_BYTES) {
        size_t end = start + YOKE_READ_BYTES;
        while (end > start && body.p[end - 1] != '\n') end--;
        if (end == start) {
            end = start + YOKE_READ_BYTES;
            while (end > start && ((u8)body.p[end] & 0xc0) == 0x80) end--;
            cut_mid_line = true;
        }
        off = end;
        shown = str_lines((Str){ body.p + start, off - start });
    }
    buf_put(out, body.p + start, off - start);

    if (cut_mid_line) {
        buf_putf(out, "\n[clipped: line %zu is longer than %u bytes]",
                 first, (unsigned)YOKE_READ_BYTES);
    } else if (off < body.n) {
        size_t rest = str_lines(str_drop(body, off));
        buf_putf(out, "\n[read %zu of %zu lines; continue with offset=%zu]",
                 shown, first - 1 + shown + rest, first + shown);
    }
    if (!buf_ok(out)) { snprintf(err, err_cap, "%s does not fit in memory", z); return false; }
    return true;
}

/* ---- write ---- */
static b8 tool_write(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    Str content = json_get_str(j, STR("content"));
    char z[YOKE_MAX_PATH];
    if (!arg_cstr(json_get_str(j, STR("path")), z, sizeof z, "path", err, err_cap))
        return false;
    if (!content.p) { snprintf(err, err_cap, "missing content"); return false; }
    FILE *f = fopen(z, "wb");
    if (!f) { snprintf(err, err_cap, "open %s for write failed", z); return false; }
    size_t wr = content.n ? fwrite(content.p, 1, content.n, f) : 0;
    b8 failed = wr != content.n || ferror(f) != 0;
    if (fclose(f) != 0) failed = true;
    if (failed) { snprintf(err, err_cap, "write %s failed", z); return false; }
    buf_putf(out, "wrote %zu bytes to %s", content.n, z);
    return true;
}

/* ---- bash ----
 * The child is spawned rather than popen'd because both of its output streams
 * belong in the result and neither belongs on the terminal: inherited stderr
 * would paint over the frame the TUI owns, and inherited stdin would race the
 * composer for keystrokes. */
/* A ring holding the last `cap` bytes written: `head` is the oldest, `len`
 * how many are live. */
static void ring_put(char *ring, size_t cap, size_t *head, size_t *len,
                     const char *p, size_t n) {
    if (n > cap) { p += n - cap; n = cap; }
    size_t at = (*head + *len) % cap;
    size_t first = cap - at < n ? cap - at : n;
    memcpy(ring + at, p, first);
    if (n > first) memcpy(ring, p + first, n - first);
    if (*len + n > cap) {
        *head = (at + n) % cap;
        *len = cap;
    } else {
        *len += n;
    }
}

b8 shell_capture(Str cmd, Buf *out, char *err, size_t err_cap) {
    /* A truncated shell line is a different program, so anything over the
     * limit is refused rather than clamped. */
    static char z[YOKE_MAX_COMMAND];
    if (!arg_cstr(cmd, z, sizeof z, "command", err, err_cap)) return false;

    i32 fds[2];
    if (pipe(fds) != 0) { snprintf(err, err_cap, "pipe failed"); return false; }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        snprintf(err, err_cap, "fork failed");
        return false;
    }
    if (pid == 0) {
        i32 null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) { dup2(null_fd, 0); close(null_fd); }
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        execl("/bin/sh", "sh", "-c", z, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    /* The tail is kept because a command that fails says why on its last
     * lines. The child is drained to the end either way, since closing the
     * pipe early would kill it with SIGPIPE mid-run. */
    static char ring[YOKE_SHELL_OUT_BYTES];
    size_t head = 0, len = 0, total = 0;
    char block[4096];
    for (;;) {
        ssize_t n = read(fds[0], block, sizeof block);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        total += (size_t)n;
        ring_put(ring, sizeof ring, &head, &len, block, (size_t)n);
    }
    close(fds[0]);

    if (total > len)
        buf_putf(out, "[output truncated: last %zu of %zu bytes]\n", len, total);
    buf_put(out, ring + head, len < sizeof ring ? len : sizeof ring - head);
    if (len == sizeof ring) buf_put(out, ring, head);

    i32 status = 0;
    pid_t done;
    while ((done = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {}
    if (done < 0) buf_puts(out, STR("\n[exit unknown]"));
    else if (WIFSIGNALED(status))
        buf_putf(out, "\n[killed by signal %d]", WTERMSIG(status));
    else buf_putf(out, "\n[exit %d]",
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return true;
}

static b8 tool_bash(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    return shell_capture(json_get_str(j, STR("command")), out, err, err_cap);
}

/* ---- edit ----
 * Exact replacements applied in order and written once at the end, so a call
 * that cannot be finished leaves the file as it was rather than half patched.
 */

/* Offset of the single occurrence of `needle`, or SIZE_MAX with `count` set
 * to what was found instead. An ambiguous match is refused rather than
 * resolved by position: the first occurrence is rarely the reviewed one. */
static size_t find_unique(Str hay, Str needle, size_t *count) {
    size_t at = (size_t)-1;
    *count = 0;
    if (!needle.n || hay.n < needle.n) return at;
    for (size_t i = 0; i + needle.n <= hay.n; i++) {
        if (memcmp(hay.p + i, needle.p, needle.n)) continue;
        if (!(*count)++) at = i;
        else return (size_t)-1;
    }
    return *count == 1 ? at : (size_t)-1;
}

static b8 apply_edit(Str *body, Str oldt, Str newt, Arena *scratch, size_t nth,
                     char *err, size_t err_cap) {
    if (!oldt.p || !newt.p) {
        snprintf(err, err_cap, "edit %zu: missing old_text/new_text", nth);
        return false;
    }
    size_t count;
    size_t at = find_unique(*body, oldt, &count);
    if (at == (size_t)-1) {
        if (count > 1)
            snprintf(err, err_cap, "edit %zu: old_text appears %zu times; "
                     "include the surrounding lines that make it unique",
                     nth, count);
        else
            snprintf(err, err_cap, "edit %zu: old_text not found", nth);
        return false;
    }
    Buf b;
    buf_init(&b, scratch, body->n + newt.n + 1);
    buf_put(&b, body->p, at);
    buf_puts(&b, newt);
    buf_put(&b, body->p + at + oldt.n, body->n - at - oldt.n);
    if (!buf_ok(&b)) {
        snprintf(err, err_cap, "edit %zu: out of memory", nth);
        return false;
    }
    *body = buf_finish(&b);
    return true;
}

static b8 tool_edit(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    char z[YOKE_MAX_PATH];
    if (!arg_cstr(json_get_str(j, STR("path")), z, sizeof z, "path", err, err_cap))
        return false;

    Str body;
    if (!slurp(z, scratch, &body, err, err_cap)) return false;

    const JVal *edits = json_get(j, STR("edits"));
    size_t applied = 0;
    if (edits && edits->type == J_ARR) {
        if (edits->u.arr.n > YOKE_MAX_EDITS) {
            snprintf(err, err_cap, "%zu edits, limit %u",
                     edits->u.arr.n, YOKE_MAX_EDITS);
            return false;
        }
        for (size_t i = 0; i < edits->u.arr.n; i++) {
            const JVal *e = json_at(edits, i);
            if (!apply_edit(&body, json_get_str(e, STR("old_text")),
                            json_get_str(e, STR("new_text")), scratch, i + 1,
                            err, err_cap))
                return false;
            applied++;
        }
    }
    /* The single-replacement form is the same call with one edit inline. */
    if (json_get(j, STR("old_text"))) {
        if (!apply_edit(&body, json_get_str(j, STR("old_text")),
                        json_get_str(j, STR("new_text")), scratch, applied + 1,
                        err, err_cap))
            return false;
        applied++;
    }
    if (!applied) { snprintf(err, err_cap, "no edits given"); return false; }

    FILE *o = fopen(z, "wb");
    if (!o) { snprintf(err, err_cap, "re-open %s failed", z); return false; }
    b8 failed = body.n && fwrite(body.p, 1, body.n, o) != body.n;
    if (ferror(o)) failed = true;
    if (fclose(o) != 0) failed = true;
    if (failed) { snprintf(err, err_cap, "write %s failed", z); return false; }
    buf_putf(out, "%zu edit%s applied", applied, applied == 1 ? "" : "s");
    return true;
}

/* ---- grep and find ----
 * One walk serves both, in name order so a search is reproducible and capped
 * so a wide pattern costs a page rather than the repo. The match is a literal
 * substring rather than a regex, since bash still has the shell for the rest.
 */
typedef struct {
    Buf   *out;
    Arena *names;         /* per-level directory listing, reset on the way out */
    Arena *file;          /* one file's contents, reset after each             */
    Str    pattern;       /* empty for find                                    */
    const char *glob;     /* NULL for no name filter                           */
    size_t max;
    size_t found;
    size_t skipped;       /* results past `max`                                */
    b8     ignore_case;
    b8     single;         /* the root is one file rather than a tree      */
    char   path[YOKE_MAX_PATH];
    size_t path_n;
} Walk;

static b8 mem_eq_ci(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

static b8 line_matches(Str line, Str pat, b8 ignore_case) {
    if (line.n < pat.n) return false;
    for (size_t i = 0; i + pat.n <= line.n; i++) {
        if (ignore_case ? mem_eq_ci(line.p + i, pat.p, pat.n)
                        : !memcmp(line.p + i, pat.p, pat.n))
            return true;
    }
    return false;
}

/* A relative walk starts at "./", which is two bytes of nothing on every line
 * it would be printed on. */
static const char *walk_shown(const Walk *w) {
    return w->path[0] == '.' && w->path[1] == '/' ? w->path + 2 : w->path;
}

/* A glob without a slash names a file, one with a slash names a path, where a
 * wildcard stops at a separator the way a shell's does. */
static b8 name_matches(const Walk *w, const char *base) {
    if (!w->glob) return true;
    if (!strchr(w->glob, '/')) return fnmatch(w->glob, base, 0) == 0;
    return fnmatch(w->glob, walk_shown(w), FNM_PATHNAME) == 0;
}

/* The file arena is the walk's own, since `out` grows in the scratch arena
 * while this runs and rewinding that would free the results. */
static void walk_grep_file(Walk *w) {
    struct stat st;
    if (stat(w->path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    if ((u64)st.st_size > YOKE_MAX_GREP_FILE) return;

    arena_reset(w->file);
    Str body;
    char ignored[128];
    if (slurp(w->path, w->file, &body, ignored, sizeof ignored)) {
        /* A nul byte says the file is not text, and a binary "match" is a
         * line of noise nobody can read. */
        Str head = str_take(body, 4096);
        if (!head.n || !memchr(head.p, '\0', head.n)) {
            size_t off = 0, ln = 0;
            Str line;
            while (str_line(body, &off, &line)) {
                ln++;
                if (!line_matches(line, w->pattern, w->ignore_case)) continue;
                if (w->found++ >= w->max) { w->skipped++; continue; }
                buf_putf(w->out, "%s:%zu: ", walk_shown(w), ln);
                Str shown = str_clip_utf8(str_trim(line), YOKE_GREP_LINE);
                buf_puts(w->out, shown);
                if (shown.n < str_trim(line).n) buf_puts(w->out, STR(" ..."));
                buf_putc(w->out, '\n');
            }
        }
    }
}

/* One regular file the walk reached, or the one it was pointed at. */
static void walk_file(Walk *w, const char *base) {
    if (!name_matches(w, base)) return;
    if (w->pattern.n) { walk_grep_file(w); return; }
    if (w->found++ >= w->max) w->skipped++;
    else buf_putf(w->out, "%s\n", walk_shown(w));
}

/* Appends "/name" to the walked path and restores it afterwards. */
static b8 walk_enter(Walk *w, const char *name, size_t n) {
    if (w->path_n + n + 2 >= sizeof w->path) return false;
    w->path[w->path_n] = '/';
    memcpy(w->path + w->path_n + 1, name, n);
    w->path_n += n + 1;
    w->path[w->path_n] = '\0';
    return true;
}

static b8 walk_dir(Walk *w, i32 depth) {
    if (depth > YOKE_WALK_DEPTH) return true;
    DIR *d = opendir(w->path);
    if (!d) return true;

    size_t mark = w->names->off;
    Str *ent = arena_new(w->names, Str, YOKE_WALK_ENTRIES);
    size_t n = 0;
    struct dirent *de;
    while (ent && n < YOKE_WALK_ENTRIES && (de = readdir(d))) {
        /* .git alone would be most of the walk. */
        if (de->d_name[0] == '.') continue;
        Str name = str_dup(w->names, str_c(de->d_name));
        if (!name.p) break;
        ent[n++] = name;
    }
    closedir(d);

    /* readdir order is the filesystem's, so the same tree would answer
     * differently on the next run. */
    for (size_t i = 1; i < n; i++) {
        Str key = ent[i];
        size_t k = i;
        while (k && strcmp(ent[k - 1].p, key.p) > 0) { ent[k] = ent[k - 1]; k--; }
        ent[k] = key;
    }

    b8 room = true;
    size_t base_n = w->path_n;
    for (size_t i = 0; i < n && room; i++) {
        if (!walk_enter(w, ent[i].p, ent[i].n)) continue;
        struct stat st;
        if (lstat(w->path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                room = walk_dir(w, depth + 1);
            } else if (S_ISREG(st.st_mode)) {
                walk_file(w, ent[i].p);
            }
        }
        w->path_n = base_n;
        w->path[base_n] = '\0';
    }
    w->names->off = mark;
    return room;
}

/* The root a search starts from, refused when it leaves nothing to search. */
static b8 walk_start(Walk *w, Str root, char *err, size_t err_cap) {
    char rel[YOKE_MAX_PATH];
    if (!root.n) root = STR(".");
    if (!arg_cstr(root, rel, sizeof rel, "path", err, err_cap)) return false;
    i32 len = snprintf(w->path, sizeof w->path, "%s", rel);
    if (len < 0 || (size_t)len >= sizeof w->path) {
        snprintf(err, err_cap, "path too long");
        return false;
    }
    while (len > 1 && w->path[len - 1] == '/') w->path[--len] = '\0';
    w->path_n = (size_t)len;
    struct stat st;
    if (stat(w->path, &st) != 0) {
        snprintf(err, err_cap, "%s does not exist", rel);
        return false;
    }
    /* Narrowing a query to the file it is about is the same request with a
     * smaller root. */
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
        snprintf(err, err_cap, "%s is not a file or a directory", rel);
        return false;
    }
    w->single = !S_ISDIR(st.st_mode);
    return true;
}

static b8 walk_run(Str args, Arena *scratch, Buf *out, b8 grep,
                   char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }

    static Walk w;
    w = (Walk){0};
    w.out = out;
    w.pattern = grep ? json_get_str(j, STR("pattern")) : (Str){0};
    if (grep && !w.pattern.n) {
        snprintf(err, err_cap, "missing pattern");
        return false;
    }
    const JVal *ic = json_get(j, STR("ignore_case"));
    w.ignore_case = ic && ic->type == J_BOOL && ic->u.b;

    char glob[YOKE_MAX_PATH];
    Str g = json_get_str(j, grep ? STR("glob") : STR("name"));
    if (g.n) {
        if (!arg_cstr(g, glob, sizeof glob, "glob", err, err_cap)) return false;
        w.glob = glob;
    } else if (!grep) {
        snprintf(err, err_cap, "missing name");
        return false;
    }

    if (!arg_count(j, STR("max_results"),
                   grep ? YOKE_GREP_RESULTS : YOKE_FIND_RESULTS,
                   1u << 20, &w.max, err, err_cap))
        return false;
    if (!walk_start(&w, json_get_str(j, STR("path")), err, err_cap)) return false;

    /* Carved once and never rewound past, since `out` keeps growing in
     * `scratch` above them for as long as the walk finds something. */
    void *mem = arena_alloc(scratch, YOKE_WALK_BYTES + YOKE_MAX_GREP_FILE + 1, 16);
    if (!mem) { snprintf(err, err_cap, "out of memory"); return false; }
    Arena names, file;
    arena_init(&names, mem, YOKE_WALK_BYTES);
    arena_init(&file, (char *)mem + YOKE_WALK_BYTES, YOKE_MAX_GREP_FILE + 1);
    w.names = &names;
    w.file = &file;

    b8 room = true;
    if (w.single) {
        const char *slash = strrchr(w.path, '/');
        walk_file(&w, slash ? slash + 1 : w.path);
    } else {
        room = walk_dir(&w, 0);
    }
    if (!w.found) {
        buf_putf(out, "no %s\n", grep ? "matches" : "files");
    } else if (w.skipped) {
        /* The walk finishes either way: the count the cap is judged against
         * is worth the scan, and only the results were expensive. */
        buf_putf(out, "[%zu of %zu%s shown; narrow the search or raise "
                 "max_results]\n", w.max, w.found, room ? "" : "+");
    }
    if (!buf_ok(out)) { snprintf(err, err_cap, "result does not fit in memory"); return false; }
    return true;
}

static b8 tool_grep(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    return walk_run(args, scratch, out, true, err, err_cap);
}

static b8 tool_find(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    return walk_run(args, scratch, out, false, err, err_cap);
}

/* ---- plan mode ----
 * submit_plan and ask_user are registered like any other tool so the model is
 * offered them in the usual place, but the agent loop intercepts both, since
 * each is a question put to the user and a ToolRun cannot reach the screen.
 * Reaching these bodies means the interception is gone. */
static b8 tool_agent_only(Str args, Arena *scratch, Buf *out,
                          char *err, size_t err_cap) {
    (void)args; (void)scratch; (void)out;
    snprintf(err, err_cap, "this tool is answered by the user, not run");
    return false;
}

/* ---- registry ---- */
static AgentMode g_mode;

void tools_set_mode(AgentMode mode) { g_mode = mode; }

b8 tools_available(const ToolRegistry *r, size_t id, AgentMode mode) {
    if (!r->modes || id >= r->n) return false;
    return (r->modes[id] & (mode == MODE_PLAN ? TOOL_IN_PLAN : TOOL_IN_BUILD))
           != 0;
}

void tools_init(ToolRegistry *r, Arena *persist) {
    r->name   = arena_new(persist, Str, YOKE_MAX_TOOLS);
    r->desc   = arena_new(persist, Str, YOKE_MAX_TOOLS);
    r->schema = arena_new(persist, Str, YOKE_MAX_TOOLS);
    r->run    = arena_new(persist, ToolRun, YOKE_MAX_TOOLS);
    r->modes  = arena_new(persist, u8, YOKE_MAX_TOOLS);
    r->n = 0;
    if (!r->name || !r->desc || !r->schema || !r->run || !r->modes) {
        r->name = NULL;
        return;
    }
#define ADD(nm, dsc, md, sch, fn) do { \
    if (r->n >= YOKE_MAX_TOOLS) break; \
    r->name[r->n] = STR(nm); \
    r->desc[r->n] = STR(dsc); \
    r->schema[r->n] = STR(sch); \
    r->run[r->n] = fn; \
    r->modes[r->n] = (md); \
    r->n++; } while (0)
#define BOTH (TOOL_IN_BUILD | TOOL_IN_PLAN)

    ADD("read", "Read a file, by default its first 2000 lines or 50KB. "
        "Continue past that with offset.", BOTH,
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"offset\":{\"type\":\"integer\",\"description\":\"first line, 1-based\"},"
        "\"limit\":{\"type\":\"integer\",\"description\":\"lines to read\"}},"
        "\"required\":[\"path\"]}",
        tool_read);
    ADD("grep", "Search file contents for a literal string, recursively.", BOTH,
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"file or directory to search, default .\"},"
        "\"glob\":{\"type\":\"string\",\"description\":\"only files matching, e.g. *.c\"},"
        "\"ignore_case\":{\"type\":\"boolean\"},"
        "\"max_results\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}",
        tool_grep);
    ADD("find", "List files whose name matches a glob, recursively.", BOTH,
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\","
        "\"description\":\"glob; matched against the path when it has a /\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"file or directory to search, default .\"},"
        "\"max_results\":{\"type\":\"integer\"}},\"required\":[\"name\"]}",
        tool_find);
    ADD("write", "Write content to a file (overwrite).", TOOL_IN_BUILD,
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        tool_write);
    ADD("bash", "Run a shell command and capture stdout/stderr.", BOTH,
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
        tool_bash);
    ADD("edit", "Replace exact text in a file. Each old_text must match "
        "exactly once; pass several in edits to patch one file at once.",
        TOOL_IN_BUILD,
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"},"
        "\"edits\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":"
        "{\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"}},"
        "\"required\":[\"old_text\",\"new_text\"]}}},\"required\":[\"path\"]}",
        tool_edit);
    ADD("ask_user", "Ask the user to choose between options. Mark the one you "
        "recommend; they may also answer in their own words.", TOOL_IN_PLAN,
        "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"},\"options\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},\"detail\":{\"type\":\"string\"},\"recommended\":{\"type\":\"boolean\"}},\"required\":[\"label\"]}}},\"required\":[\"question\",\"options\"]}",
        tool_agent_only);
    ADD("submit_plan", "Hand the finished plan to the user to approve.",
        TOOL_IN_PLAN,
        "{\"type\":\"object\",\"properties\":{\"plan\":{\"type\":\"string\"}},\"required\":[\"plan\"]}",
        tool_agent_only);
#undef BOTH
#undef ADD
}

size_t tools_find(const ToolRegistry *r, Str name) {
    if (!r->name || !name.p) return TOOL_NONE;
    for (size_t i = 0; i < r->n; i++)
        if (str_eq(r->name[i], name)) return i;
    return TOOL_NONE;
}

b8 tools_run(const ToolRegistry *r, size_t id, Str args, Arena *scratch,
             Buf *out, char *err, size_t err_cap) {
    if (!r->run || id >= r->n) {
        snprintf(err, err_cap, "unknown tool");
        return false;
    }
    /* A schema offered earlier in the conversation is still in the model's
     * context, so plan mode's read-only promise has to hold here rather than
     * only in what is advertised now. */
    if (!tools_available(r, id, g_mode)) {
        snprintf(err, err_cap, "%.*s is not available in plan mode",
                 (int)r->name[id].n, r->name[id].p);
        return false;
    }
    return r->run[id](args, scratch, out, err, err_cap);
}

void tools_write_schemas(Buf *b, const ToolRegistry *r) {
    buf_putc(b, '[');
    if (r->name) {
        b8 first = true;
        for (size_t i = 0; i < r->n; i++) {
            if (!tools_available(r, i, g_mode)) continue;
            if (!first) buf_putc(b, ',');
            first = false;
            buf_putf(b, "{\"type\":\"function\",\"function\":{\"name\":");
            buf_json_str(b, r->name[i]);
            buf_putf(b, ",\"description\":");
            buf_json_str(b, r->desc[i]);
            buf_putf(b, ",\"parameters\":%s}}", r->schema[i].p);
        }
    }
    buf_putc(b, ']');
}
