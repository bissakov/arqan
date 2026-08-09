/* render.c: how a tool call and its result read in the transcript.
 *
 * A call on the wire is a JSON blob and a result is whatever the tool
 * printed, so each gets a header naming what it does to what, a preview of
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

/* Verbose drops every cap below. */
static b8 g_verbose;

/* The block being written, set for the length of one render_tool_* call: its
 * caps are lifted while `g_expanded` holds, and the tail that folds it back
 * carries `g_zone`. */
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

/* A count argument as the tool reads it; absent or malformed is 0. */
static size_t num_arg(const JVal *args, Str key) {
    const JVal *v = args ? json_get(args, key) : NULL;
    if (!v || v->type != J_NUM || v->u.n < 1 || v->u.n > (f64)(1u << 30))
        return 0;
    return (size_t)v->u.n;
}

/* Which page a read asked for, since without it two reads of one file are the
 * same header twice. */
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
    i32 len = snprintf(buf, sizeof buf, "%zu %s%s", n, what,
                       n == 1 ? "" : "s");
    if (len > 0) sink((Str){ buf, (size_t)len });
}

/* How long the run took, closing a result's summary line: a call that took a
 * minute and one that took none read alike otherwise. A sub-second time keeps
 * its milliseconds, since that is the whole of what it has to say. */
static void write_elapsed(u32 ms) {
    if (!ms) return;
    char buf[32];
    i32 len;
    if (ms < 1000)
        len = snprintf(buf, sizeof buf, " \u00b7 %ums", ms);
    else if (ms < 60000)
        len = snprintf(buf, sizeof buf, " \u00b7 %.1fs", (f64)ms / 1000.0);
    else
        len = snprintf(buf, sizeof buf, " \u00b7 %um%02us",
                       ms / 60000u, ms / 1000u % 60u);
    if (len > 0) tui_write_muted((Str){ buf, (size_t)len });
}

/* Which sink one line is written through, for a block whose lines do not read
 * alike; NULL means the block's own. */
typedef Sink (*LineStyle)(Str line);

/* At most `max` lines of `body`, each behind `gutter`, then a tail row saying
 * what was left out. That row is the block's click target, so a block whose
 * lines all fit gets no tail either way. */
static void write_styled(Str body, Str gutter, size_t max, Sink sink,
                         LineStyle style) {
    size_t cap = line_cap(max);
    size_t off = 0, shown = 0;
    Str line;
    while (shown < cap && str_line(body, &off, &line)) {
        Sink put = style ? style(line) : sink;
        put(gutter);
        Str head = clip(line, R_LINE_BYTES);
        put(head);
        if (head.n < line.n) put(STR(" ..."));
        put(STR("\n"));
        shown++;
    }
    size_t rest = str_lines(str_drop(body, off));
    char buf[64];
    i32 len;
    if (rest) {
        len = snprintf(buf, sizeof buf, "\u25be %zu more line%s\n",
                       rest, rest == 1 ? "" : "s");
    } else if (g_expanded && !g_verbose && shown > max) {
        /* Only an unfolded block offers to fold; verbose hides nothing. */
        len = snprintf(buf, sizeof buf, "\u25b4 show less\n");
    } else {
        return;
    }
    tui_zone_begin(g_zone);
    sink(gutter);
    if (len > 0) sink((Str){ buf, (size_t)len });
    tui_zone_end();
}

static void write_lines(Str body, Str gutter, size_t max, Sink sink) {
    write_styled(body, gutter, max, sink, NULL);
}

/* A diff reads by its markers, so each line keeps the one it came with and is
 * coloured by it; a file header carries the only path in a patch and is the
 * one line of it worth the tool colour. */
static Sink diff_style(Str line) {
    if (str_starts(line, STR("+++ ")) || str_starts(line, STR("--- ")))
        return tui_write_tool;
    if (!line.n) return tui_write_muted;
    if (line.p[0] == '+') return tui_write_result;
    if (line.p[0] == '-') return tui_write_error;
    return tui_write_muted;
}

/* The file a patch is about, taken from its first header, plus how many more
 * it names: a diff carries its target in its body rather than in an argument. */
static Str patch_target(Str patch, char *buf, size_t cap) {
    size_t off = 0, files = 0;
    Str line, first = {0};
    while (str_line(patch, &off, &line)) {
        if (!str_starts(line, STR("+++ "))) continue;
        if (!files++) {
            first = str_trim(str_drop(line, 4));
            const char *tab = (const char *)memchr(first.p, '\t', first.n);
            if (tab) first.n = (size_t)(tab - first.p);
            if (str_starts(first, STR("b/"))) first = str_drop(first, 2);
        }
    }
    if (files < 2) return first;
    i32 len = snprintf(buf, cap, "%.*s +%zu more", (i32)first.n, first.p,
                       files - 1);
    return len > 0 ? (Str){ buf, (size_t)len < cap ? (size_t)len : cap - 1 }
                   : first;
}

void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded) {
    g_zone = id;
    g_expanded = expanded;
    size_t mark = scratch->off;
    JVal *j = json_parse(scratch, args);

    Str path = str_arg(j, STR("path"));
    Str cmd  = str_arg(j, STR("command"));
    Str patch = str_arg(j, STR("patch"));
    char patch_buf[R_TARGET_BYTES + 32];
    if (patch.n) path = patch_target(patch, patch_buf, sizeof patch_buf);
    /* What a search is about is what it looks for, not where it starts. */
    Str query = str_eq(name, STR("grep")) ? str_arg(j, STR("pattern"))
              : str_eq(name, STR("find")) ? str_arg(j, STR("name"))
              : (Str){0};
    Str target = query.n ? query : path.n ? path : cmd;
    size_t cmd_off = 0;
    if (!path.n && cmd.n) str_line(cmd, &cmd_off, &target);

    tui_block();
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
    } else if (patch.n) {
        /* Twice a call's usual allowance: a hunk spends most of its lines on
         * the context that locates it. */
        write_styled(patch, STR("\u2502 "), R_ARG_LINES * 2, tui_write_muted,
                     diff_style);
    } else if (cmd.n) {
        write_lines(str_drop(cmd, cmd_off), STR("\u2502 "),
                    R_ARG_LINES, tui_write_muted);
    } else if (!path.n && !query.n) {
        /* No shape this renderer knows, so the arguments as they came. */
        write_lines(args, STR("\u2502 "), R_ARG_LINES,
                    tui_write_muted);
    }

    scratch->off = mark;
    g_expanded = false;
    g_zone = 0;
}

/* A '!' run has no JSON to unpack: the command is the header and its trailing
 * lines the input preview. */
void render_shell_call(Str cmd, u32 id, b8 expanded) {
    g_zone = id;
    g_expanded = expanded;
    size_t off = 0;
    Str first = cmd;
    str_line(cmd, &off, &first);

    tui_block();
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

/* A plan is prose the model wrote, so it reads as the Markdown it is rather
 * than as a quoted argument, and is never truncated: it is what the user is
 * being asked to approve. */
void render_plan(Str plan) {
    tui_block();
    tui_write_tool(STR("\u25c6  plan\n\n"));
    md_write(plan);
    md_end();
}

void render_question(Str question) {
    tui_block();
    tui_write_tool(STR("\u25c6  ask\n"));
    size_t off = 0;
    Str line;
    while (str_line(question, &off, &line)) {
        tui_write_muted(STR("\u2502 "));
        tui_write(clip(line, R_LINE_BYTES));
        tui_write(STR("\n"));
    }
}

/* A shell run ends on a bracketed status line, which summarises the result
 * rather than being part of what the command printed. */
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

void render_tool_result(Str name, Str result, u32 id, b8 expanded, u32 ms) {
    g_zone = id;
    g_expanded = expanded;
    if (str_starts(result, STR("ERROR: "))) {
        Str msg = str_drop(result, 7);
        size_t off = 0;
        Str first = msg;
        str_line(msg, &off, &first);
        tui_write_error(STR("\u2514\u2500 error: "));
        tui_write_error(clip(first, R_LINE_BYTES));
        write_elapsed(ms);
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
    } else if (str_eq(name, STR("read"))) {
        write_count(str_lines(result), "line", tui_write_result);
    } else {
        size_t off = 0;
        Str first = body;
        str_line(body, &off, &first);
        tui_write_result(clip(first, R_LINE_BYTES));
        body = str_drop(body, off);
    }
    write_elapsed(ms);
    tui_write_result(STR("\n"));
    write_lines(body, STR("   "), R_RESULT_LINES, tui_write_muted);
    g_expanded = false;
    g_zone = 0;
}
