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

/* The block being written: its caps are lifted while `g_expanded` holds, and
 * the tail that folds it back carries `g_zone`. Both are set for the length of
 * one render_tool_* call. */
static b8 g_expanded;
static u32 g_zone;

void render_set_verbose(b8 on) { g_verbose = on; }
b8 render_verbose(void) { return g_verbose; }

static b8 uncapped(void) { return g_verbose || g_expanded; }

static size_t line_cap(size_t max) { return uncapped() ? (size_t)-1 : max; }

static Str clip(Str s, size_t max) {
    return uncapped() ? s : str_clip_utf8(s, max);
}

static Str str_arg(const JVal *args, Str key) {
    const JVal *v = args ? json_get(args, key) : NULL;
    return v && v->type == J_STR ? v->u.s : (Str){0};
}

/* A count argument as the tool reads it: absent or malformed is 0. */
static size_t num_arg(const JVal *args, Str key) {
    const JVal *v = args ? json_get(args, key) : NULL;
    if (!v || v->type != J_NUM || v->u.n < 1 || v->u.n > (f64)(1u << 30))
        return 0;
    return (size_t)v->u.n;
}

/* A read is a page of a file, and which page is as much of what it did as the
 * path is: without it two reads of one file are the same header twice. */
static void write_read_range(const JVal *args) {
    size_t offset = num_arg(args, STR("offset"));
    size_t limit = num_arg(args, STR("limit"));
    if (!offset && !limit) return;
    if (!offset) offset = 1;
    char buf[64];
    i32 len = limit
        ? snprintf(buf, sizeof buf, " lines %zu-%zu", offset, offset + limit - 1)
        : snprintf(buf, sizeof buf, " from line %zu", offset);
    if (len > 0) tui_write_tool((Str){ buf, (size_t)len });
}

typedef void (*Sink)(Str);

static void write_count(size_t n, const char *what, Sink sink) {
    char buf[64];
    i32 len = snprintf(buf, sizeof buf, "%zu %s%s\n", n, what,
                       n == 1 ? "" : "s");
    if (len > 0) sink((Str){ buf, (size_t)len });
}

/* Writes at most `max` lines of `body`, each behind `gutter`, then a tail row
 * saying what was left out. That row is the block's click target: it is where
 * an expanded block offers to fold back, so a block whose lines all fit gets
 * no tail either way. */
static void write_lines(Str body, Str gutter, size_t max, Sink sink) {
    size_t cap = line_cap(max);
    size_t off = 0, shown = 0;
    Str line;
    while (shown < cap && str_line(body, &off, &line)) {
        sink(gutter);
        Str head = clip(line, R_LINE_BYTES);
        sink(head);
        if (head.n < line.n) sink(STR(" ..."));
        sink(STR("\n"));
        shown++;
    }
    size_t rest = str_lines(str_drop(body, off));
    char buf[64];
    i32 len;
    if (rest) {
        len = snprintf(buf, sizeof buf, "\u25be %zu more line%s\n",
                       rest, rest == 1 ? "" : "s");
    } else if (g_expanded && !g_verbose && shown > max) {
        /* Only a block this reader unfolded offers to fold: under /verbose
         * nothing is hidden to begin with. */
        len = snprintf(buf, sizeof buf, "\u25b4 show less\n");
    } else {
        return;
    }
    tui_zone_begin(g_zone);
    sink(gutter);
    if (len > 0) sink((Str){ buf, (size_t)len });
    tui_zone_end();
}

/* A diff is the only honest preview of an edit: the same lines the tool
 * matches, then the ones it leaves behind. */
static void write_diff(Str old_text, Str new_text) {
    write_lines(old_text, STR("\u2502 - "), R_ARG_LINES / 2,
                tui_write_error);
    write_lines(new_text, STR("\u2502 + "), R_ARG_LINES / 2,
                tui_write_result);
}

void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded) {
    g_zone = id;
    g_expanded = expanded;
    size_t mark = scratch->off;
    JVal *j = json_parse(scratch, args);

    Str path = str_arg(j, STR("path"));
    Str cmd  = str_arg(j, STR("command"));
    /* What a search is about is what it looks for, not where it starts. */
    Str query = str_eq(name, STR("grep")) ? str_arg(j, STR("pattern"))
              : str_eq(name, STR("find")) ? str_arg(j, STR("name"))
              : (Str){0};
    Str target = query.n ? query : path.n ? path : cmd;
    size_t cmd_off = 0;
    if (!path.n && cmd.n) str_line(cmd, &cmd_off, &target);

    tui_write(STR("\n"));
    tui_write_tool(STR("\u25c6  "));
    tui_write_tool(name);
    if (target.n) {
        tui_write_tool(STR(" "));
        Str head = clip(target, R_TARGET_BYTES);
        tui_write_tool(head);
        if (head.n < target.n) tui_write_tool(STR(" ..."));
    }
    if (str_eq(name, STR("read"))) write_read_range(j);
    tui_write_tool(STR("\n"));

    if (str_eq(name, STR("write"))) {
        Str content = str_arg(j, STR("content"));
        write_lines(content, STR("\u2502 "), R_ARG_LINES,
                    tui_write_muted);
    } else if (str_eq(name, STR("edit"))) {
        const JVal *edits = j ? json_get(j, STR("edits")) : NULL;
        if (edits && edits->type == J_ARR) {
            for (size_t k = 0; k < edits->u.arr.n; k++) {
                const JVal *e = json_at(edits, k);
                write_diff(str_arg(e, STR("old_text")),
                           str_arg(e, STR("new_text")));
            }
        }
        if (json_get(j, STR("old_text")))
            write_diff(str_arg(j, STR("old_text")), str_arg(j, STR("new_text")));
    } else if (cmd.n) {
        write_lines(str_drop(cmd, cmd_off), STR("\u2502 "),
                    R_ARG_LINES, tui_write_muted);
    } else if (!path.n && !query.n) {
        /* No shape this renderer knows: the arguments as they came. */
        write_lines(args, STR("\u2502 "), R_ARG_LINES,
                    tui_write_muted);
    }

    scratch->off = mark;
    g_expanded = false;
    g_zone = 0;
}

/* A '!' run has no JSON to unpack: the command is the header and its trailing
 * lines the input preview, the shape a tool call gets. */
void render_shell_call(Str cmd, u32 id, b8 expanded) {
    g_zone = id;
    g_expanded = expanded;
    size_t off = 0;
    Str first = cmd;
    str_line(cmd, &off, &first);

    tui_write(STR("\n"));
    tui_write_tool(STR("\u25c6  shell "));
    Str head = clip(first, R_TARGET_BYTES);
    tui_write_tool(head);
    if (head.n < first.n) tui_write_tool(STR(" ..."));
    tui_write_tool(STR("\n"));
    write_lines(str_drop(cmd, off), STR("\u2502 "), R_ARG_LINES,
                tui_write_muted);

    g_expanded = false;
    g_zone = 0;
}

/* The plan a handover carries is prose the model wrote, so it reads as the
 * Markdown it is rather than as a quoted argument, and it is never truncated:
 * it is the thing the user is being asked to approve. */
void render_plan(Str plan) {
    tui_write(STR("\n"));
    tui_write_tool(STR("\u25c6  plan\n\n"));
    md_write(plan);
    md_end();
    tui_write(STR("\n"));
}

void render_question(Str question) {
    tui_write(STR("\n"));
    tui_write_tool(STR("\u25c6  ask\n"));
    size_t off = 0;
    Str line;
    while (str_line(question, &off, &line)) {
        tui_write_muted(STR("\u2502 "));
        tui_write(clip(line, R_LINE_BYTES));
        tui_write(STR("\n"));
    }
}

/* A shell run ends its output with a bracketed status line; it is the
 * result's summary, not part of what the command printed. */
static b8 split_status(Str result, Str *body, Str *status) {
    size_t off = 0, last = 0, start = 0;
    Str line;
    while (str_line(result, &off, &line)) { last = start; start = off; }
    Str tail = { result.p + last, result.n - last };
    while (tail.n && tail.p[tail.n - 1] == '\n') tail.n--;
    if (tail.n < 2 || tail.p[0] != '[' || tail.p[tail.n - 1] != ']')
        return false;
    *status = (Str){ tail.p + 1, tail.n - 2 };
    *body = (Str){ result.p, last ? last - 1 : 0 };
    return true;
}

void render_tool_result(Str name, Str result, u32 id, b8 expanded) {
    g_zone = id;
    g_expanded = expanded;
    if (str_starts(result, STR("ERROR: "))) {
        Str msg = str_drop(result, 7);
        size_t off = 0;
        Str first = msg;
        str_line(msg, &off, &first);
        tui_write_error(STR("\u2514\u2500 error: "));
        tui_write_error(clip(first, R_LINE_BYTES));
        tui_write_error(STR("\n"));
        g_expanded = false;
        g_zone = 0;
        return;
    }

    Str body = result, status = {0};
    b8 shell = str_eq(name, STR("bash")) || str_eq(name, STR("shell"));
    b8 have_status = shell && split_status(result, &body, &status);
    tui_write_result(STR("\u2514\u2500 "));
    if (have_status) {
        tui_write_result(status);
        tui_write_result(STR("\n"));
    } else if (str_eq(name, STR("read"))) {
        write_count(str_lines(result), "line", tui_write_result);
    } else {
        size_t off = 0;
        Str first = body;
        str_line(body, &off, &first);
        tui_write_result(clip(first, R_LINE_BYTES));
        tui_write_result(STR("\n"));
        body = str_drop(body, off);
    }
    write_lines(body, STR("   "), R_RESULT_LINES, tui_write_muted);
    g_expanded = false;
    g_zone = 0;
}
