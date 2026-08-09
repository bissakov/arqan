/* settings.c: the one file format every setting yoke owns is written in.
 *
 * A file is a list of "key = value" lines, optionally grouped under a
 * "[section]" header; a line whose first non-blank byte is '#' is a comment
 * and a value runs to the end of its line, unquoted and unescaped. The same
 * syntax serves the three files a user may look at:
 *   $XDG_CONFIG_HOME/yoke/config       theirs to edit, providers included
 *   $XDG_STATE_HOME/yoke/state         what the UI last chose
 *   $XDG_STATE_HOME/yoke/credentials   the keys alone, mode 0600
 *
 * Reads parse a whole file into a Settings table pointing into the arena copy
 * of its bytes. A write is a per-key upsert rather than a rewrite: a config
 * file is a document its owner edits, so /provider changes the lines it owns
 * and leaves the comments, the order and the unknown keys exactly as they
 * were. The result is written to a temporary file and renamed, so an
 * interrupted write leaves the previous file rather than half a line.
 */
#include "yoke.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Splits "key = value"; false for a blank line, a comment and a header. */
static b8 setting_kv(Str line, Str *k, Str *v) {
    line = str_trim(line);
    if (line.n == 0 || line.p[0] == '#' || line.p[0] == '[') return false;
    size_t eq = 0;
    while (eq < line.n && line.p[eq] != '=') eq++;
    if (eq == line.n) return false;
    *k = str_trim(str_take(line, eq));
    *v = str_trim(str_drop(line, eq + 1));
    return k->n > 0;
}

/* The name inside "[...]", empty when the line is not a header. */
static Str setting_section(Str line) {
    line = str_trim(line);
    if (line.n < 2 || line.p[0] != '[' || line.p[line.n - 1] != ']')
        return (Str){0};
    return str_trim(str_drop(str_take(line, line.n - 1), 1));
}

/* Empty for a file that is missing, unreadable or past `max`: a settings file
 * yoke cannot read is one it has no keys from, whichever it was. */
static Str settings_src(Str path, Arena *a, size_t max) {
    Str src = {0};
    if (path.n) file_read(a, path.p, max, 0, &src, NULL);
    return src;
}

static b8 settings_write_file(Str path, Str data, u32 mode) {
    char tmp[YOKE_MAX_PATH];
    i32 n = snprintf(tmp, sizeof tmp, "%.*s.tmp", (i32)path.n, path.p);
    if (n <= 0 || (size_t)n >= sizeof tmp) return false;
    i32 fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
    if (fd < 0) return false;
    b8 ok = true;
    for (size_t off = 0; ok && off < data.n;) {
        ssize_t w = write(fd, data.p + off, data.n - off);
        if (w <= 0) ok = false; else off += (size_t)w;
    }
    if (fchmod(fd, (mode_t)mode) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, path.p) == 0) return true;
    unlink(tmp);
    return false;
}

static size_t settings_parse(Settings *s, Str src) {
    s->n = 0;
    Str section = {0};
    size_t off = 0;
    Str line;
    while (str_line(src, &off, &line)) {
        Str sec = setting_section(line);
        if (sec.n) { section = sec; continue; }
        Str k, v;
        if (!setting_kv(line, &k, &v)) continue;
        if (s->n >= YOKE_MAX_SETTINGS) break;
        s->section[s->n] = section;
        s->key[s->n] = k;
        s->val[s->n] = v;
        s->n++;
    }
    return s->n;
}

b8 settings_load(Settings *s, Str path, Arena *a) {
    s->n = 0;
    Str src = settings_src(path, a, YOKE_MAX_SETTINGS_BYTES);
    if (!src.n) return false;
    settings_parse(s, src);
    return true;
}

Str settings_get(const Settings *s, Str section, Str key) {
    /* Last wins, so a key repeated in one file reads like a later assignment. */
    Str out = {0};
    for (size_t i = 0; i < s->n; i++)
        if (str_eq(s->section[i], section) && str_eq(s->key[i], key))
            out = s->val[i];
    return out;
}

size_t settings_sections(const Settings *s, Str prefix, Str *out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < s->n && n < max; i++) {
        Str sec = s->section[i];
        if (!sec.n || !str_starts(sec, prefix)) continue;
        b8 seen = false;
        for (size_t j = 0; j < n; j++) if (str_eq(out[j], sec)) seen = true;
        if (!seen) out[n++] = sec;
    }
    return n;
}

/* True while `line` is inside `section`: a header switches sections and every
 * other line belongs to whichever one preceded it. */
static b8 in_section(Str *cur, Str line, Str want) {
    Str sec = setting_section(line);
    if (sec.n) *cur = sec;
    return str_eq(*cur, want);
}

b8 settings_set(Str path, Str section, const Str *keys, const Str *vals,
                size_t n, u32 mode, Arena *scratch) {
    if (!path.n || n == 0 || n > YOKE_MAX_SET_KEYS) return false;
    size_t mark = scratch->off;
    Str src = settings_src(path, scratch, YOKE_MAX_SETTINGS_BYTES);
    b8 done[YOKE_MAX_SET_KEYS] = {0};

    /* An existing file keeps the mode its owner gave it; `mode` is what a new
     * one is created with. A credentials file others can read is refused
     * before it reaches here, so preserving a mode never widens one. */
    struct stat st;
    if (stat(path.p, &st) == 0) mode = (u32)(st.st_mode & 0777);

    Buf b;
    buf_init(&b, scratch, src.n + 512);

    /* Every existing line is copied through; one inside `section` whose key is
     * being set is replaced in place, so the file keeps its shape. */
    Str cur = {0}, line;
    size_t off = 0;
    /* The unnamed section is the head of the file, which always exists: a
     * key added to it belongs above the first header rather than under it. */
    b8 seen_section = section.n == 0, section_open = false;
    size_t tail = 0;   /* bytes written when `section` was last still open   */
    while (str_line(src, &off, &line)) {
        b8 mine = in_section(&cur, line, section);
        if (mine) { seen_section = true; section_open = true; }
        else if (section_open) { section_open = false; }
        Str k, v;
        if (mine && setting_kv(line, &k, &v)) {
            size_t at = n;
            for (size_t i = 0; i < n; i++)
                if (!done[i] && str_eq(keys[i], k)) { at = i; break; }
            if (at < n) {
                done[at] = true;
                if (vals[at].n) {
                    buf_puts(&b, keys[at]);
                    buf_puts(&b, STR(" = "));
                    buf_puts(&b, vals[at]);
                    buf_putc(&b, '\n');
                }
                if (section_open) tail = b.n;
                continue;
            }
        }
        buf_puts(&b, line);
        buf_putc(&b, '\n');
        if (section_open) tail = b.n;
    }

    /* What was not replaced is appended: to the end of its section when it has
     * one, else under a header opened for it. */
    Buf add;
    buf_init(&add, scratch, 256);
    for (size_t i = 0; i < n; i++) {
        if (done[i] || !vals[i].n) continue;
        buf_puts(&add, keys[i]);
        buf_puts(&add, STR(" = "));
        buf_puts(&add, vals[i]);
        buf_putc(&add, '\n');
    }
    if (!buf_ok(&add) || !buf_ok(&b)) { scratch->off = mark; return false; }
    Str extra = buf_finish(&add);

    Buf out;
    buf_init(&out, scratch, b.n + extra.n + section.n + 8);
    if (extra.n && seen_section) {
        buf_put(&out, b.p, tail);
        buf_puts(&out, extra);
        buf_put(&out, b.p + tail, b.n - tail);
    } else {
        buf_put(&out, b.p, b.n);
        if (extra.n) {
            if (section.n) {
                if (out.n && out.p[out.n - 1] != '\n') buf_putc(&out, '\n');
                buf_putc(&out, '[');
                buf_puts(&out, section);
                buf_puts(&out, STR("]\n"));
            }
            buf_puts(&out, extra);
        }
    }
    if (!buf_ok(&out)) { scratch->off = mark; return false; }
    b8 ok = settings_write_file(path, buf_finish(&out), mode);
    scratch->off = mark;
    return ok;
}

b8 settings_set_one(Str path, Str section, Str key, Str val, u32 mode,
                    Arena *scratch) {
    return settings_set(path, section, &key, &val, 1, mode, scratch);
}

/* ---- the state file ------------------------------------------------------
 * What the UI last chose, in one place: the model, the provider and whether
 * telemetry records. It is yoke's memory rather than the user's file, but it
 * is written in the same syntax so there is one thing to learn.
 */
Str state_get(Str key, Arena *out, Arena *scratch) {
    size_t mark = scratch->off;
    Str val = {0};
    Str path = paths_file(YOKE_DIR_STATE, STR("state"), scratch);
    Settings s;
    if (path.n && settings_load(&s, path, scratch)) {
        Str v = settings_get(&s, (Str){0}, key);
        if (v.n) val = str_dup(out, v);
    }
    /* One arena for both is allowed, and rewinding it would take the answer
     * with the file it was read from. */
    if (out != scratch) scratch->off = mark;
    return val;
}

b8 state_set(Str key, Str val, Arena *scratch) {
    Str dir = paths_dir(YOKE_DIR_STATE, scratch);
    Str path = paths_file(YOKE_DIR_STATE, STR("state"), scratch);
    if (!dir.n || !path.n || !paths_ensure_dir(dir)) return false;
    return settings_set_one(path, (Str){0}, key, val, 0600, scratch);
}

/* An older yoke kept each of these in a file of its own, holding a word.
 * They are folded into the state file and removed, so the state directory
 * holds files rather than a key per file, and a name freed this way is
 * available to whatever wants it next. A value the state file already has
 * wins, since it is the one the UI has been writing since. */
void state_sweep(Arena *scratch) {
    static const char *keys[] = { "model", "provider", "telemetry" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        size_t mark = scratch->off;
        Str key = str_c(keys[i]);
        Str path = paths_file(YOKE_DIR_STATE, key, scratch);
        struct stat st;
        if (path.n && stat(path.p, &st) == 0 && S_ISREG(st.st_mode)) {
            Str src = settings_src(path, scratch, 4096), val = {0};
            size_t off = 0;
            if (str_line(src, &off, &val)) val = str_trim(val);
            Str have = state_get(key, scratch, scratch);
            if (val.n && !have.n)
                state_set(key, val, scratch);
            unlink(path.p);
        }
        scratch->off = mark;
    }
}
