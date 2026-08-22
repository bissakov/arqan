#include "agent.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static b8 hist_write(FILE *f, Str s) {
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        i32 rc = c == '\\' ? fputs("\\\\", f)
               : c == '\n' ? fputs("\\n", f)
               : c == '\r' ? fputs("\\r", f) : fputc(c, f);
        if (rc == EOF) return false;
    }
    return fputc('\n', f) != EOF;
}

static Str hist_unescape(Arena *a, Str s) {
    Buf b; buf_init(&b, a, s.n + 1);
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] != '\\' || i + 1 == s.n) { buf_putc(&b, s.p[i]); continue; }
        char c = s.p[++i];
        if (c == 'n') buf_putc(&b, '\n');
        else if (c == 'r') buf_putc(&b, '\r');
        else buf_putc(&b, c);
    }
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

// A full ring drops its oldest entry rather than refusing the new one.
static void hist_push(History *h, Str line) {
    if (!h->entry || !h->cap) return;
    if (h->n && str_eq(h->entry[h->n - 1], line)) return;  
    if (h->n == h->cap) {
        memmove(h->entry, h->entry + 1, (h->cap - 1) * sizeof *h->entry);
        h->n--;
    }
    h->entry[h->n++] = line;
}

/* Slide the live entries down over the gaps dropped ones left behind. The
 * arena is ours alone, so every entry sits above the one before it and a
 * destination never overtakes a source still to be read. */
static void hist_compact(History *h) {
    size_t off = h->base_off;
    for (size_t i = 0; i < h->n; i++) {
        Str e = h->entry[i];
        char *dst = (char *)h->a->base + off;
        memmove(dst, e.p, e.n + 1);
        h->entry[i] = (Str){ dst, e.n };
        off += e.n + 1;
    }
    h->a->off = off;
}

static Str hist_store(History *h, Str s) {
    Str kept = str_dup(h->a, s);
    if (kept.p) return kept;
    hist_compact(h);
    return str_dup(h->a, s);
}

b8 history_init(History *h, Arena *own, size_t cap) {
    memset(h, 0, sizeof *h);
    if (cap == 0 || !own) return false;
    h->entry = arena_new(own, Str, cap);
    if (!h->entry) return false;
    h->a = own;
    h->base_off = own->off;
    h->cap = cap;
    h->cursor = 0;
    return true;
}

/* The recall file for this workspace, `<state>/history/<cwd slug>`, so two
 * projects never offer each other's prompts. `scratch` holds the XDG base for
 * the length of the call; the result lives in `a`. Empty when the base or the
 * cwd is unavailable, which leaves recall in memory for the session. */
Str history_path(Arena *a, Arena *scratch) {
    Str base = paths_dir(AGENT_DIR_STATE, scratch);
    if (!base.n) return (Str){0};
    char slug[AGENT_SLUG_MAX + 32];
    size_t slug_n = paths_cwd_slug(slug, sizeof slug);
    if (!slug_n) return (Str){0};
    Buf b; buf_init(&b, a, base.n + slug_n + 12);
    buf_puts(&b, base);
    buf_puts(&b, STR("/history/"));
    buf_puts(&b, (Str){ slug, slug_n });
    if (!buf_ok(&b)) return (Str){0};
    Str out = buf_finish(&b);
    return out.n < AGENT_MAX_PATH ? out : (Str){0};
}

/* Versions before per-workspace recall wrote one file where the directory now
 * goes, and no directory can be created under that name. Renamed rather than
 * removed so the entries stay recoverable by hand. */
static void hist_retire_global(Str path) {
    Str dir = path;
    while (dir.n && dir.p[dir.n - 1] != '/') dir.n--;
    if (dir.n < 2 || dir.n + 8 >= AGENT_MAX_PATH) return;
    dir.n--;

    char old[AGENT_MAX_PATH], retired[AGENT_MAX_PATH];
    memcpy(old, dir.p, dir.n);
    old[dir.n] = '\0';
    struct stat st;
    if (stat(old, &st) != 0 || !S_ISREG(st.st_mode)) return;
    i32 n = snprintf(retired, sizeof retired, "%s.global", old);
    if (n > 0 && (size_t)n < sizeof retired) (void)rename(old, retired);
}

/* Reads the state file into the entry arena, with `scratch` holding the raw
 * bytes for the length of the call. A missing file is an empty history. */
void history_load(History *h, Str path, Arena *scratch) {
    if (!h->cap || !path.n) return;
    h->path = path;
    hist_retire_global(path);
    FILE *f = fopen(path.p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    i64 sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = sz > 0 && sz <= (i64)AGENT_MAX_HISTORY_BYTES
              ? arena_new(scratch, char, (size_t)sz + 1) : NULL;
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    Str src = { buf, rd };
    size_t start = 0;
    for (size_t i = 0; i <= src.n; i++) {
        if (i != src.n && src.p[i] != '\n') continue;
        Str raw = { src.p + start, i - start };
        start = i + 1;
        if (!raw.n) continue;
        Str line = hist_unescape(scratch, raw);
        if (!line.n || line.n > AGENT_MAX_HISTORY_LINE) continue;
        Str kept = hist_store(h, line);
        if (kept.n) hist_push(h, kept);
    }
    h->cursor = h->n;
    
    if (h->n == h->cap) history_rewrite(h);
}

static b8 history_write_all(FILE *f, void *ud) {
    const History *h = ud;
    for (size_t i = 0; i < h->n; i++)
        if (!hist_write(f, h->entry[i])) return false;
    return true;
}

void history_rewrite(const History *h) {
    if (h->path.n)
        (void)file_write_atomic(h->path.p, 0600, true,
                                history_write_all, (void *)h);
}

void history_add(History *h, Str line) {
    if (!h->cap) return;
    Str t = str_trim(line);
    if (!t.n || t.n > AGENT_MAX_HISTORY_LINE) { h->cursor = h->n; return; }
    b8 dup = h->n && str_eq(h->entry[h->n - 1], t);
    Str kept = dup ? h->entry[h->n - 1] : hist_store(h, t);
    if (!kept.n) { h->cursor = h->n; return; }
    hist_push(h, kept);
    h->cursor = h->n;
    if (dup || !h->path.n) return;

    Str dir = h->path;
    while (dir.n && dir.p[dir.n - 1] != '/') dir.n--;
    if (dir.n > 1) { dir.n--; paths_ensure_dir(dir); }
    FILE *f = fopen(h->path.p, "ab");
    if (!f) return;
    (void)hist_write(f, kept);
    fclose(f);
}

b8 history_prev(History *h, Str *out) {
    if (!h->n || h->cursor == 0) return false;
    h->cursor--;
    *out = h->entry[h->cursor];
    return true;
}

/* Stepping past the newest entry lands on nothing, which is the caller's cue
 * to restore the draft the recall interrupted. */
b8 history_next(History *h, Str *out) {
    if (h->cursor >= h->n) return false;
    h->cursor++;
    if (h->cursor == h->n) { *out = (Str){0}; return false; }
    *out = h->entry[h->cursor];
    return true;
}

void history_reset_cursor(History *h) { h->cursor = h->n; }
b8 history_browsing(const History *h) { return h->cursor < h->n; }
