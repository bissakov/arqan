/* tools.c — SoA tool registry + built-in tools (read, write, bash, edit).
 *
 * Each tool run() receives raw JSON args, a scratch arena, an output Buf, and
 * an error buffer. No tool allocates on the heap; everything uses the scratch
 * arena (reset per turn by the agent loop).
 */
#include "yoke.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ---- helpers: pull a "path"/"command" string arg from JSON -------------- */
static Str json_get_str(const JVal *args, Str key) {
    const JVal *v = json_get(args, key);
    if (!v || v->type != J_STR) return (Str){0};
    return v->u.s;
}

/* Copy a JSON string argument into a nul-terminated buffer, or fail.
 * Clamping instead would run a *different* command, or touch a different
 * file, than the one the model asked for and the user read. */
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

/* Slurp a file into the scratch arena. Every size here comes from the
 * filesystem, so each one is validated before it reaches an allocation. */
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

/* ---- read ---- */
static b8 tool_read(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    char z[YOKE_MAX_PATH];
    if (!arg_cstr(json_get_str(j, STR("path")), z, sizeof z, "path", err, err_cap))
        return false;

    Str body;
    if (!slurp(z, scratch, &body, err, err_cap)) return false;
    buf_puts(out, body);
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

/* ---- bash ---- */
static b8 tool_bash(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    /* One heap-free buffer for the command: a truncated shell line is a
     * different program, so anything over the limit is refused outright. */
    static char z[YOKE_MAX_COMMAND];
    if (!arg_cstr(json_get_str(j, STR("command")), z, sizeof z, "command",
                  err, err_cap))
        return false;

    FILE *p = popen(z, "r");
    if (!p) { snprintf(err, err_cap, "popen failed"); return false; }
    char block[4096];
    size_t total = 0;
    size_t n;
    while ((n = fread(block, 1, sizeof block, p)) > 0) {
        buf_put(out, block, n);
        total += n;
        if (total > (1u << 20)) { buf_puts(out, STR("\n[output truncated]\n")); break; }
    }
    i32 rc = pclose(p);
    if (rc < 0) buf_puts(out, STR("\n[exit unknown]"));
    else if (WIFSIGNALED(rc)) buf_putf(out, "\n[killed by signal %d]", WTERMSIG(rc));
    else buf_putf(out, "\n[exit %d]", WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
    return true;
}

/* ---- edit (simple full-file replace) ---- */
static b8 tool_edit(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    Str oldt = json_get_str(j, STR("old_text"));
    Str newt = json_get_str(j, STR("new_text"));
    char z[YOKE_MAX_PATH];
    if (!arg_cstr(json_get_str(j, STR("path")), z, sizeof z, "path", err, err_cap))
        return false;
    if (!oldt.p || !newt.p) { snprintf(err, err_cap, "missing old_text/new_text"); return false; }

    Str s;
    if (!slurp(z, scratch, &s, err, err_cap)) return false;

    /* find old_text */
    if (oldt.n == 0 || s.n < oldt.n) { snprintf(err, err_cap, "old_text not found"); return false; }
    const char *found = NULL;
    for (size_t i = 0; i + oldt.n <= s.n; i++) {
        if (!memcmp(s.p + i, oldt.p, oldt.n)) { found = s.p + i; break; }
    }
    if (!found) { snprintf(err, err_cap, "old_text not found"); return false; }

    FILE *o = fopen(z, "wb");
    if (!o) { snprintf(err, err_cap, "re-open %s failed", z); return false; }
    const char *tail = found + oldt.n;
    size_t head_n = (size_t)(found - s.p), tail_n = s.n - (size_t)(tail - s.p);
    b8 failed = false;
    if (head_n && fwrite(s.p, 1, head_n, o) != head_n) failed = true;
    if (newt.n && fwrite(newt.p, 1, newt.n, o) != newt.n) failed = true;
    if (tail_n && fwrite(tail, 1, tail_n, o) != tail_n) failed = true;
    if (fclose(o) != 0) failed = true;
    if (failed) { snprintf(err, err_cap, "write %s failed", z); return false; }
    buf_puts(out, STR("edit applied"));
    return true;
}

/* ---- registry ---- */
void tools_init(ToolRegistry *r, Arena *persist) {
    r->name   = arena_new(persist, Str, YOKE_MAX_TOOLS);
    r->desc   = arena_new(persist, Str, YOKE_MAX_TOOLS);
    r->schema = arena_new(persist, Str, YOKE_MAX_TOOLS);
    r->run    = arena_new(persist, ToolRun, YOKE_MAX_TOOLS);
    r->n = 0;
    if (!r->name || !r->desc || !r->schema || !r->run) {
        r->name = NULL;
        return;
    }
#define ADD(nm, dsc, sch, fn) do { \
    if (r->n >= YOKE_MAX_TOOLS) break; \
    r->name[r->n] = STR(nm); \
    r->desc[r->n] = STR(dsc); \
    r->schema[r->n] = STR(sch); \
    r->run[r->n] = fn; \
    r->n++; } while (0)

    ADD("read", "Read a file's contents.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        tool_read);
    ADD("write", "Write content to a file (overwrite).",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        tool_write);
    ADD("bash", "Run a shell command and capture stdout/stderr.",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
        tool_bash);
    ADD("edit", "Replace the first occurrence of old_text with new_text in a file.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}",
        tool_edit);
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
    return r->run[id](args, scratch, out, err, err_cap);
}

void tools_write_schemas(Buf *b, const ToolRegistry *r) {
    buf_putc(b, '[');
    if (r->name) {
        for (size_t i = 0; i < r->n; i++) {
            if (i) buf_putc(b, ',');
            buf_putf(b, "{\"type\":\"function\",\"function\":{\"name\":");
            buf_json_str(b, r->name[i]);
            buf_putf(b, ",\"description\":");
            buf_json_str(b, r->desc[i]);
            buf_putf(b, ",\"parameters\":%s}}", r->schema[i].p);
        }
    }
    buf_putc(b, ']');
}
