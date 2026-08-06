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

/* ---- read ---- */
static b8 tool_read(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    Str path = json_get_str(j, STR("path"));
    if (!path.p) { snprintf(err, err_cap, "missing path"); return false; }

    /* nul-terminate */
    char z[4096]; size_t l = path.n < sizeof z - 1 ? path.n : sizeof z - 1;
    memcpy(z, path.p, l); z[l] = '\0';

    FILE *f = fopen(z, "rb");
    if (!f) { snprintf(err, err_cap, "open %s failed", z); return false; }
    fseek(f, 0, SEEK_END); i64 sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); snprintf(err, err_cap, "ftell failed"); return false; }
    char *buf = arena_new(scratch, char, (size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    buf_puts(out, (Str){ buf, rd });
    return true;
}

/* ---- write ---- */
static b8 tool_write(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    Str path = json_get_str(j, STR("path"));
    Str content = json_get_str(j, STR("content"));
    if (!path.p || !content.p) { snprintf(err, err_cap, "missing path/content"); return false; }
    char z[4096]; size_t l = path.n < sizeof z - 1 ? path.n : sizeof z - 1;
    memcpy(z, path.p, l); z[l] = '\0';
    FILE *f = fopen(z, "wb");
    if (!f) { snprintf(err, err_cap, "open %s for write failed", z); return false; }
    fwrite(content.p, 1, content.n, f);
    fclose(f);
    buf_putf(out, "wrote %zu bytes to %s", content.n, z);
    return true;
}

/* ---- bash ---- */
static b8 tool_bash(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    Str cmd = json_get_str(j, STR("command"));
    if (!cmd.p) { snprintf(err, err_cap, "missing command"); return false; }

    char z[8192]; size_t l = cmd.n < sizeof z - 1 ? cmd.n : sizeof z - 1;
    memcpy(z, cmd.p, l); z[l] = '\0';

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
    buf_putf(out, "\n[exit %d]", WEXITSTATUS(rc));
    return true;
}

/* ---- edit (simple full-file replace) ---- */
static b8 tool_edit(Str args, Arena *scratch, Buf *out, char *err, size_t err_cap) {
    JVal *j = json_parse(scratch, args);
    if (!j) { snprintf(err, err_cap, "bad args json"); return false; }
    Str path = json_get_str(j, STR("path"));
    Str oldt = json_get_str(j, STR("old_text"));
    Str newt = json_get_str(j, STR("new_text"));
    if (!path.p || !oldt.p || !newt.p) { snprintf(err, err_cap, "missing path/old_text/new_text"); return false; }

    char z[4096]; size_t l = path.n < sizeof z - 1 ? path.n : sizeof z - 1;
    memcpy(z, path.p, l); z[l] = '\0';
    FILE *f = fopen(z, "rb");
    if (!f) { snprintf(err, err_cap, "open %s failed", z); return false; }
    fseek(f, 0, SEEK_END); i64 sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = arena_new(scratch, char, (size_t)sz + 1);
    size_t rd = fread(src, 1, (size_t)sz, f); fclose(f);
    src[rd] = '\0';
    Str s = { src, rd };

    /* find old_text */
    if (oldt.n == 0 || s.n < oldt.n) { snprintf(err, err_cap, "old_text not found"); return false; }
    const char *found = NULL;
    for (size_t i = 0; i + oldt.n <= s.n; i++) {
        if (!memcmp(s.p + i, oldt.p, oldt.n)) { found = s.p + i; break; }
    }
    if (!found) { snprintf(err, err_cap, "old_text not found"); return false; }

    FILE *o = fopen(z, "wb");
    if (!o) { snprintf(err, err_cap, "re-open %s failed", z); return false; }
    fwrite(s.p, 1, (size_t)(found - s.p), o);
    fwrite(newt.p, 1, newt.n, o);
    const char *tail = found + oldt.n;
    fwrite(tail, 1, s.n - (size_t)(tail - s.p), o);
    fclose(o);
    buf_puts(out, STR("edit applied"));
    return true;
}

/* ---- registry ---- */
void tools_init(ToolRegistry *r, Arena *persist) {
    r->defs = arena_new(persist, ToolDef, YOKE_MAX_TOOLS);
    r->n = 0;
#define ADD(nm, dsc, sch, fn) do { \
    r->defs[r->n].name = STR(nm); \
    r->defs[r->n].desc = STR(dsc); \
    r->defs[r->n].schema = STR(sch); \
    r->defs[r->n].run = fn; \
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

const ToolDef *tools_find(const ToolRegistry *r, Str name) {
    for (size_t i = 0; i < r->n; i++)
        if (str_eq(r->defs[i].name, name)) return &r->defs[i];
    return NULL;
}

void tools_write_schemas(Buf *b, const ToolRegistry *r) {
    buf_putc(b, '[');
    for (size_t i = 0; i < r->n; i++) {
        if (i) buf_putc(b, ',');
        buf_putf(b, "{\"type\":\"function\",\"function\":{\"name\":");
        buf_json_str(b, r->defs[i].name);
        buf_putf(b, ",\"description\":");
        buf_json_str(b, r->defs[i].desc);
        buf_putf(b, ",\"parameters\":%s}}", r->defs[i].schema.p);
    }
    buf_putc(b, ']');
}
