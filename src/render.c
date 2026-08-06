/* render.c: how a tool call and its result read in the transcript.
 *
 * The wire form of a call is a JSON blob and the wire form of a result is
 * whatever the tool printed; neither is what a reader wants to see. Every
 * tool here gets a header naming what it does to what, an optional preview of
 * the input it carries, and a result summarised by its own shape.
 */
#include "yoke.h"

#include <stdio.h>
#include <string.h>

enum {
    R_ARG_LINES    = 8,    /* input lines shown under a call's header  */
    R_RESULT_LINES = 12,   /* output lines shown under a result        */
    R_LINE_BYTES   = 200,  /* a single line's share of the transcript  */
    R_TARGET_BYTES = 120   /* the header's path or command             */
};

/* Verbose mode drops every cap below: what the tool read, wrote or printed
 * reaches the transcript whole. */
static b8 g_verbose;

void render_set_verbose(b8 on) { g_verbose = on; }
b8 render_verbose(void) { return g_verbose; }

static size_t line_cap(size_t max) { return g_verbose ? (size_t)-1 : max; }

static b8 next_line(Str s, size_t *off, Str *line) {
    if (*off >= s.n) return false;
    const char *p = s.p + *off;
    size_t left = s.n - *off;
    const char *nl = (const char *)memchr(p, '\n', left);
    size_t n = nl ? (size_t)(nl - p) : left;
    *line = (Str){ p, n };
    *off += nl ? n + 1 : n;
    return true;
}

static size_t count_lines(Str s) {
    size_t off = 0, n = 0;
    Str line;
    while (next_line(s, &off, &line)) n++;
    return n;
}

/* Cutting mid-sequence would paint a replacement glyph, so the clip backs up
 * to a leading byte. */
static Str clip(Str s, size_t max) {
    if (g_verbose || s.n <= max) return s;
    size_t n = max;
    while (n && ((unsigned char)s.p[n] & 0xc0) == 0x80) n--;
    return (Str){ s.p, n };
}

static Str str_arg(const JVal *args, Str key) {
    const JVal *v = args ? json_get(args, key) : NULL;
    return v && v->type == J_STR ? v->u.s : (Str){0};
}

typedef void (*Sink)(Str);

static void write_count(size_t n, const char *what, Sink sink) {
    char buf[64];
    i32 len = snprintf(buf, sizeof buf, "%zu %s%s\n", n, what,
                       n == 1 ? "" : "s");
    if (len > 0) sink((Str){ buf, (size_t)len });
}

/* Writes at most `max` lines of `body`, each behind `gutter`, then how many
 * lines the transcript left out. */
static void write_lines(Str body, Str gutter, size_t max, Sink sink) {
    size_t off = 0, shown = 0;
    Str line;
    while (shown < max && next_line(body, &off, &line)) {
        sink(gutter);
        Str head = clip(line, R_LINE_BYTES);
        sink(head);
        if (head.n < line.n) sink(STR(" ..."));
        sink(STR("\n"));
        shown++;
    }
    size_t rest = count_lines(str_drop(body, off));
    if (!rest) return;
    sink(gutter);
    char buf[64];
    i32 len = snprintf(buf, sizeof buf, "... %zu more line%s\n",
                       rest, rest == 1 ? "" : "s");
    if (len > 0) sink((Str){ buf, (size_t)len });
}

/* A diff is the only honest preview of an edit: the same lines the tool
 * matches, then the ones it leaves behind. */
static void write_diff(Str old_text, Str new_text) {
    write_lines(old_text, STR("\u2502 - "), line_cap(R_ARG_LINES / 2),
                tui_write_error);
    write_lines(new_text, STR("\u2502 + "), line_cap(R_ARG_LINES / 2),
                tui_write_result);
}

void render_tool_call(Str name, Str args, Arena *scratch) {
    size_t mark = scratch->off;
    JVal *j = json_parse(scratch, args);

    Str path = str_arg(j, STR("path"));
    Str cmd  = str_arg(j, STR("command"));
    Str target = path.n ? path : cmd;
    size_t cmd_off = 0;
    if (!path.n && cmd.n) next_line(cmd, &cmd_off, &target);

    tui_write(STR("\n"));
    tui_write_tool(STR("\u25c6  "));
    tui_write_tool(name);
    if (target.n) {
        tui_write_tool(STR(" "));
        Str head = clip(target, R_TARGET_BYTES);
        tui_write_tool(head);
        if (head.n < target.n) tui_write_tool(STR(" ..."));
    }
    tui_write_tool(STR("\n"));

    if (str_eq(name, STR("write"))) {
        Str content = str_arg(j, STR("content"));
        write_lines(content, STR("\u2502 "), line_cap(R_ARG_LINES),
                    tui_write_muted);
    } else if (str_eq(name, STR("edit"))) {
        write_diff(str_arg(j, STR("old_text")), str_arg(j, STR("new_text")));
    } else if (cmd.n) {
        write_lines(str_drop(cmd, cmd_off), STR("\u2502 "),
                    line_cap(R_ARG_LINES), tui_write_muted);
    } else if (!path.n) {
        /* No shape this renderer knows: the arguments as they came. */
        write_lines(args, STR("\u2502 "), line_cap(R_ARG_LINES),
                    tui_write_muted);
    }

    scratch->off = mark;
}

/* The bash tool ends its output with a bracketed status line; it is the
 * result's summary, not part of what the command printed. */
static b8 split_status(Str result, Str *body, Str *status) {
    size_t off = 0, last = 0, start = 0;
    Str line;
    while (next_line(result, &off, &line)) { last = start; start = off; }
    Str tail = { result.p + last, result.n - last };
    while (tail.n && tail.p[tail.n - 1] == '\n') tail.n--;
    if (tail.n < 2 || tail.p[0] != '[' || tail.p[tail.n - 1] != ']')
        return false;
    *status = (Str){ tail.p + 1, tail.n - 2 };
    *body = (Str){ result.p, last ? last - 1 : 0 };
    return true;
}

void render_tool_result(Str name, Str result) {
    if (str_starts(result, STR("ERROR: "))) {
        Str msg = str_drop(result, 7);
        size_t off = 0;
        Str first = msg;
        next_line(msg, &off, &first);
        tui_write_error(STR("\u2514\u2500 error: "));
        tui_write_error(clip(first, R_LINE_BYTES));
        tui_write_error(STR("\n"));
        return;
    }

    Str body = result, status = {0};
    b8 have_status = str_eq(name, STR("bash")) && split_status(result, &body,
                                                               &status);
    tui_write_result(STR("\u2514\u2500 "));
    if (have_status) {
        tui_write_result(status);
        tui_write_result(STR("\n"));
    } else if (str_eq(name, STR("read"))) {
        write_count(count_lines(result), "line", tui_write_result);
    } else {
        size_t off = 0;
        Str first = body;
        next_line(body, &off, &first);
        tui_write_result(clip(first, R_LINE_BYTES));
        tui_write_result(STR("\n"));
        body = str_drop(body, off);
    }
    write_lines(body, STR("   "), line_cap(R_RESULT_LINES), tui_write_muted);
}
