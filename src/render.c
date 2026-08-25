#include "agent.h"

#include <stdio.h>
#include <string.h>

enum {
    R_ARG_LINES = 8,
    R_RESULT_LINES = 12,
    R_LINE_BYTES = 200,
    R_TARGET_BYTES = 120,

    R_CMD_BYTES = 1024
};

typedef struct {
    u32 zone;
    b8 expanded;
    b8 head_more;
} RenderBlock;
/* INVARIANT: block_begin and block_end assign this whole, so a field added
 * here resets by construction. Keep block-scoped state in here, not in
 * RenderState. */

typedef struct {
    b8 verbose;
    RenderBlock block;
} RenderState;

static RenderState g_render;

void render_set_verbose(b8 on) {
    g_render.verbose = on;
}
b8 render_verbose(void) {
    return g_render.verbose;
}

static b8 uncapped(void) {
    return g_render.verbose || g_render.block.expanded;
}

static size_t line_cap(size_t max) {
    return uncapped() ? (size_t)-1 : max;
}

static Str clip(Str s, size_t max) {
    return uncapped() ? s : str_clip_utf8(s, max);
}

typedef void (*Sink)(Str);

static void add_line_syntax(const YhlResult *hl, Str source, size_t source_off,
                            Str shown, size_t transcript_off);
static void write_syntax_lines(Str body, Str source, b8 grep,
                               const YhlResult *hl, Str gutter, size_t max,
                               size_t bytes);
static size_t patch_batch(Str patch, char *out, size_t cap);
static void write_patch_lines(Str patch, Str source, const YhlResult *hl,
                              Str gutter, size_t max);


static void write_clipped(Str s, size_t max, Sink sink) {
    Str head = clip(s, max);
    sink(head);
    if (head.n < s.n) sink(STR(" ..."));
}


static void block_begin(u32 id, b8 expanded) {
    tui_pin(id);
    g_render.block = (RenderBlock){.zone = id, .expanded = expanded};
}
static void block_end(void) {
    g_render.block = (RenderBlock){0};
}

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
    i32 len = limit ? snprintf(buf, sizeof buf, " lines %zu-%zu", offset,
                               offset + limit - 1)
                    : snprintf(buf, sizeof buf, " from line %zu", offset);
    if (len > 0) tui_write_tool((Str){buf, (size_t)len});
}

static void write_count(size_t n, const char *what, Sink sink) {
    char buf[64];
    i32 len = snprintf(buf, sizeof buf, "%zu %s%s", n, what, n == 1 ? "" : "s");
    if (len > 0) sink((Str){buf, (size_t)len});
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
        len = snprintf(buf, sizeof buf, " \u00b7 %um%02us", ms / 60000u,
                       ms / 1000u % 60u);
    if (len > 0) tui_write_muted((Str){buf, (size_t)len});
}


typedef Sink (*LineStyle)(Str line);

/* The row that closes a block: what it left out, or the way back from an
 * expansion. It is the block's click target, so a block that left nothing
 * out and can fold nothing back gets no row. A cut header counts as left
 * out even when every line fit, since otherwise nothing would offer it. */
static void write_tail(Str gutter, size_t rest, size_t shown, size_t max,
                       Sink sink) {
    char buf[64];
    i32 len;
    if (rest) {
        len = snprintf(buf, sizeof buf, "\u25be %zu more line%s\n", rest,
                       rest == 1 ? "" : "s");
    } else if (g_render.verbose) {
        return;
    } else if (g_render.block.expanded) {
        if (shown <= max && !g_render.block.head_more) return;
        len = snprintf(buf, sizeof buf, "\u25b4 show less\n");
    } else if (g_render.block.head_more) {
        len = snprintf(buf, sizeof buf, "\u25be show in full\n");
    } else {
        return;
    }
    tui_zone_begin(g_render.block.zone);
    sink(gutter);
    if (len > 0) sink((Str){buf, (size_t)len});
    tui_zone_end();
}


static void write_styled(Str body, Str gutter, size_t max, size_t bytes,
                         Sink sink, LineStyle style) {
    size_t cap = line_cap(max);
    size_t off = 0, shown = 0;
    Str line;
    while (shown < cap && str_line(body, &off, &line)) {
        Sink put = style ? style(line) : sink;
        put(gutter);
        write_clipped(line, bytes, put);
        put(STR("\n"));
        shown++;
    }
    write_tail(gutter, str_lines(str_drop(body, off)), shown, max, sink);
}

static void write_lines(Str body, Str gutter, size_t max, size_t bytes,
                        Sink sink) {
    write_styled(body, gutter, max, bytes, sink, NULL);
}

/* The file a patch is about, taken from its first header, plus how many more
 * it names: a diff carries its target in its body rather than in an argument. */
static Str patch_target(Str patch, char *buf, size_t cap, Str *hint) {
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
    *hint = first;
    if (files < 2) return first;
    i32 len =
        snprintf(buf, cap, "%.*s +%zu more", (i32)first.n, first.p, files - 1);
    return len > 0 ? (Str){buf, (size_t)len < cap ? (size_t)len : cap - 1}
                   : first;
}

static void render_todo_row(const TodoList *l, size_t i) {
    tui_write_muted(STR("\u2502 "));
    switch (l->status[i]) {
        case TODO_DONE:
            tui_write_result(STR("\u2713 "));
            tui_write_muted(clip(todo_text(l, i), R_LINE_BYTES));
            break;
        case TODO_ACTIVE:
            tui_write_tool(STR("\u25b8 "));
            tui_write(clip(todo_text(l, i), R_LINE_BYTES));
            break;
        default:
            tui_write_muted(STR("\u25cb "));
            tui_write_muted(clip(todo_text(l, i), R_LINE_BYTES));
            break;
    }
    tui_write(STR("\n"));
}

static void render_todo_call(Str args, Arena *scratch, const Conv *c,
                             size_t slot) {
    char err[AGENT_TOOL_ERR];
    TodoList l;
    tui_block();
    if (!todo_parse(args, scratch, &l, err, sizeof err)) {
        tui_write_tool(STR("\u25c6  todo\n"));
        tui_write_muted(STR("\u2502 "));
        tui_write_error(clip(str_c(err), R_LINE_BYTES));
        tui_write(STR("\n"));
        return;
    }

    TodoList prev;
    b8 delta = todo_prev(c, slot, scratch, &prev) && todo_same_items(&l, &prev);

    char head[64];
    i32 n = snprintf(head, sizeof head, "\u25c6  todo %zu/%zu\n", todo_done(&l),
                     l.n);
    if (n > 0) tui_write_tool((Str){head, (size_t)n});

    size_t drawn = 0;
    for (size_t i = 0; i < l.n; i++) {
        if (delta && l.status[i] == prev.status[i]) continue;
        render_todo_row(&l, i);
        drawn++;
    }
    if (!drawn) {
        size_t at = todo_active(&l);
        if (at != AGENT_TODO_NONE) render_todo_row(&l, at);
    }
}

void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded,
                      const Conv *c, size_t slot) {
    block_begin(id, expanded);
    size_t mark = scratch->off;
    if (str_eq(name, STR("todo"))) {
        render_todo_call(args, scratch, c, slot);
        scratch->off = mark;
        block_end();
        return;
    }
    JVal *j = json_parse(scratch, args);

    Str path = json_str(j, STR("path"));
    if (str_eq(name, STR("page_fetch"))) path = json_str(j, STR("url"));
    Str cmd = json_str(j, STR("command"));
    Str content = json_str(j, STR("content"));
    Str patch = json_str(j, STR("patch"));
    char patch_buf[R_TARGET_BYTES + 32];
    Str patch_hint = {0};
    if (patch.n)
        path = patch_target(patch, patch_buf, sizeof patch_buf, &patch_hint);
    Str query = str_eq(name, STR("grep"))   ? json_str(j, STR("pattern"))
                : str_eq(name, STR("find")) ? json_str(j, STR("name"))
                : str_eq(name, STR("internet_search"))
                    ? json_str(j, STR("query"))
                    : (Str){0};
    Str target = query.n ? query : path.n ? path : cmd;

    char job_buf[40];
    if (str_eq(name, STR("job"))) {
        const JVal *v = json_get(j, STR("id"));
        Str action = json_str(j, STR("action"));
        u64 job_id = v && v->type == J_NUM && v->u.n >= 1 ? (u64)v->u.n : 0;
        i32 n = !job_id    ? snprintf(job_buf, sizeof job_buf, "%.*s",
                                      action.n ? (i32)action.n : 4,
                                      action.n ? action.p : "list")
                : action.n ? snprintf(job_buf, sizeof job_buf, "%.*s %llu",
                                      (i32)action.n, action.p,
                                      (unsigned long long)job_id)
                           : snprintf(job_buf, sizeof job_buf, "%llu",
                                      (unsigned long long)job_id);
        if (n > 0)
            target =
                (Str){job_buf, (size_t)n < sizeof job_buf ? (size_t)n
                                                          : sizeof job_buf - 1};
    }
    size_t cmd_off = 0;
    b8 target_cmd = !path.n && cmd.n;

    char task_buf[48];
    Str task_prompt = {0};
    if (str_eq(name, STR("task"))) {
        task_prompt = json_str(j, STR("prompt"));
        Str label = json_str(j, STR("label"));
        const JVal *v = json_get(j, STR("id"));
        Str action = json_str(j, STR("action"));
        u64 task_id = v && v->type == J_NUM && v->u.n >= 1 ? (u64)v->u.n : 0;
        if (label.n) {
            target = label;
        } else if (task_id) {
            i32 n = snprintf(task_buf, sizeof task_buf, "%.*s %llu",
                             action.n ? (i32)action.n : 8,
                             action.n ? action.p : "continue",
                             (unsigned long long)task_id);
            if (n > 0)
                target = (Str){task_buf, (size_t)n < sizeof task_buf
                                             ? (size_t)n
                                             : sizeof task_buf - 1};
        } else {
            target = task_prompt;
            task_prompt = (Str){0};
        }
    }

    if (target_cmd) str_line(cmd, &cmd_off, &target);
    static YhlResult syntax;
    static char patch_source[YHL_SOURCE_MAX];
    syntax.n = 0;
    b8 source_code = false;
    Str syntax_source = content;
    if (str_eq(name, STR("write")) && path.n && content.n) {
        source_code = true;
        highlight_request(YHL_HINT_PATH, path, content, &syntax);
    } else if (str_eq(name, STR("bash")) && cmd.n) {
        source_code = true;
        syntax_source = cmd;
        highlight_request(YHL_HINT_MARKDOWN_ALIAS, STR("bash"), cmd, &syntax);
    } else if (patch.n) {
        source_code = true;
        size_t n = patch_batch(patch, patch_source, sizeof patch_source);
        syntax_source = (Str){patch_source, n};
        if (n && patch_hint.n)
            highlight_request(YHL_HINT_PATH, patch_hint, syntax_source,
                              &syntax);
    }

    tui_block();
    tui_write_tool(STR("\u25c6  "));
    tui_write_tool(name);
    if (target.n) {
        tui_write_tool(STR(" "));
        size_t bytes = target_cmd ? R_CMD_BYTES : R_TARGET_BYTES;
        g_render.block.head_more = target.n > bytes;
        Str shown = clip(target, bytes);
        size_t at = tui_transcript_pos();
        if (source_code && cmd.n) {
            tui_write_source(shown);
            add_line_syntax(&syntax, syntax_source, 0, shown, at);
        } else {
            tui_write_tool(shown);
        }
        if (shown.n < target.n) tui_write_tool(STR(" ..."));
    }
    if (str_eq(name, STR("read")) || str_eq(name, STR("page_fetch")))
        write_read_range(j);
    tui_write_tool(STR("\n"));

    if (str_eq(name, STR("write"))) {
        if (source_code)
            write_syntax_lines(content, syntax_source, false, &syntax,
                               STR("\u2502 "), R_ARG_LINES, R_LINE_BYTES);
        else
            write_lines(content, STR("\u2502 "), R_ARG_LINES, R_LINE_BYTES,
                        tui_write_muted);
    } else if (patch.n) {
        write_patch_lines(patch, syntax_source, &syntax, STR("\u2502 "),
                          R_ARG_LINES * 2);
    } else if (cmd.n) {
        if (source_code)
            write_syntax_lines(str_drop(cmd, cmd_off), syntax_source, false,
                               &syntax, STR("\u2502 "), R_ARG_LINES,
                               R_CMD_BYTES);
        else
            write_lines(str_drop(cmd, cmd_off), STR("\u2502 "), R_ARG_LINES,
                        R_CMD_BYTES, tui_write_muted);
    } else if (task_prompt.n) {
        write_lines(task_prompt, STR("\u2502 "), R_ARG_LINES, R_LINE_BYTES,
                    tui_write_muted);
    } else if (!path.n && !query.n) {
        write_lines(args, STR("\u2502 "), R_ARG_LINES, R_LINE_BYTES,
                    tui_write_muted);
    } else {
        write_tail(STR("\u2502 "), 0, 0, R_ARG_LINES, tui_write_muted);
    }

    scratch->off = mark;
    if (source_code) tui_syntax_commit();
    block_end();
}


void render_shell_call(Str cmd, u32 id, b8 expanded) {
    block_begin(id, expanded);
    size_t off = 0;
    Str first = cmd;
    str_line(cmd, &off, &first);
    static YhlResult syntax;
    syntax.n = 0;
    highlight_request(YHL_HINT_MARKDOWN_ALIAS, STR("bash"), cmd, &syntax);

    tui_block();
    tui_write_tool(STR("\u25c6  shell "));
    g_render.block.head_more = first.n > R_CMD_BYTES;
    Str shown = clip(first, R_CMD_BYTES);
    size_t at = tui_transcript_pos();
    if (cmd.n) {
        tui_write_source(shown);
        add_line_syntax(&syntax, cmd, 0, shown, at);
    } else {
        tui_write_tool(shown);
    }
    if (shown.n < first.n) tui_write_tool(STR(" ..."));
    tui_write_tool(STR("\n"));
    if (cmd.n)
        write_syntax_lines(str_drop(cmd, off), cmd, false, &syntax,
                           STR("\u2502 "), R_ARG_LINES, R_CMD_BYTES);
    else
        write_lines(str_drop(cmd, off), STR("\u2502 "), R_ARG_LINES,
                    R_CMD_BYTES, tui_write_muted);

    if (cmd.n) tui_syntax_commit();
    block_end();
}


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
    while (str_line(result, &off, &line)) {
        last = start;
        start = off;
    }
    Str tail = {result.p + last, result.n - last};
    while (tail.n && tail.p[tail.n - 1] == '\n') tail.n--;
    if (tail.n < 2 || tail.p[0] != '[' || tail.p[tail.n - 1] != ']')
        return false;
    *status = (Str){tail.p + 1, tail.n - 2};
    *body = (Str){result.p, last ? last - 1 : 0};
    return true;
}

static size_t rendered_bytes(Str s) {
    size_t n = 0;
    for (size_t i = 0; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == '\t')
            n += 4;
        else if (c == '\r' || c < 0x20)
            continue;
        else
            n++;
    }
    return n;
}

static void add_line_syntax(const YhlResult *hl, Str source, size_t source_off,
                            Str shown, size_t transcript_off) {
    size_t shown_end = source_off + shown.n;
    for (size_t i = 0; i < hl->n; i++) {
        size_t a = hl->run[i].start;
        size_t b = hl->run[i].end;
        if (b <= source_off) continue;
        if (a >= shown_end) break;
        if (a < source_off) a = source_off;
        if (b > shown_end) b = shown_end;
        if (a >= b) continue;
        size_t dst_a =
            transcript_off
            + rendered_bytes((Str){source.p + source_off, a - source_off});
        size_t dst_b = dst_a + rendered_bytes((Str){source.p + a, b - a});
        tui_syntax_add(dst_a, dst_b, hl->run[i].semantic);
    }
}


static b8 patch_fragment(Str line, Str *fragment) {
    if (!line.n || str_starts(line, STR("+++ "))
        || str_starts(line, STR("--- ")) || str_starts(line, STR("@@")))
        return false;
    if (line.p[0] != '+' && line.p[0] != '-' && line.p[0] != ' ') return false;
    *fragment = str_drop(line, 1);
    return true;
}

static b8 batch_line(char *out, size_t cap, size_t *n, Str fragment) {
    if (fragment.n + 1 > cap - *n) return false;
    if (fragment.n) memcpy(out + *n, fragment.p, fragment.n);
    *n += fragment.n;
    out[(*n)++] = '\n';
    return true;
}

static size_t patch_batch(Str patch, char *out, size_t cap) {
    size_t off = 0, n = 0;
    Str line;
    while (str_line(patch, &off, &line)) {
        Str fragment;
        if (!patch_fragment(line, &fragment)) continue;
        if (!batch_line(out, cap, &n, fragment)) return 0;
    }
    return n;
}

static void write_patch_lines(Str patch, Str source, const YhlResult *hl,
                              Str gutter, size_t max) {
    size_t cap = line_cap(max);
    size_t off = 0, shown = 0, source_off = 0;
    Str line;
    while (shown < cap && str_line(patch, &off, &line)) {
        tui_write_muted(gutter);
        Str head = clip(line, R_LINE_BYTES);
        Str full_fragment;
        if (patch_fragment(line, &full_fragment)) {
            Sink marker = line.p[0] == '+'   ? tui_write_result
                          : line.p[0] == '-' ? tui_write_error
                                             : tui_write_muted;
            if (head.n) marker((Str){head.p, 1});
            Str fragment = str_drop(head, 1);
            if (fragment.n) {
                size_t at = tui_transcript_pos();
                tui_write_source(fragment);
                add_line_syntax(hl, source, source_off, fragment, at);
            }
            source_off += full_fragment.n + 1;
        } else if (str_starts(line, STR("+++ "))
                   || str_starts(line, STR("--- "))) {
            tui_write_tool(head);
        } else {
            tui_write_muted(head);
        }
        if (head.n < line.n) tui_write_source(STR(" ..."));
        tui_write(STR("\n"));
        shown++;
    }
    write_tail(gutter, str_lines(str_drop(patch, off)), shown, max,
               tui_write_muted);
    tui_syntax_commit();
}

static b8 grep_fragment(Str line, Str *prefix, Str *fragment) {
    size_t found = SIZE_MAX;
    for (size_t i = 0; i + 3 < line.n; i++) {
        if (line.p[i] != ':') continue;
        size_t k = i + 1;
        while (k < line.n && line.p[k] >= '0' && line.p[k] <= '9') k++;
        if (k == i + 1 || k + 1 >= line.n || line.p[k] != ':'
            || line.p[k + 1] != ' ')
            continue;
        found = k + 2;
    }
    if (found == SIZE_MAX) return false;
    *prefix = (Str){line.p, found};
    *fragment = str_drop(line, found);
    return true;
}

static size_t grep_batch(Str result, char *out, size_t cap) {
    size_t off = 0, n = 0;
    Str line;
    while (str_line(result, &off, &line)) {
        Str prefix, fragment;
        if (!grep_fragment(line, &prefix, &fragment)) continue;
        if (!batch_line(out, cap, &n, fragment)) return 0;
    }
    return n;
}

static b8 source_filename(Str path) {
    static const char *const suffix[] = {
        ".c",    ".h",      ".cc",   ".cpp",  ".cxx",  ".hh",  ".hpp",
        ".hxx",  ".rs",     ".go",   ".py",   ".pyw",  ".js",  ".jsx",
        ".mjs",  ".cjs",    ".ts",   ".mts",  ".cts",  ".tsx", ".sh",
        ".bash", ".bashrc", ".json", ".toml", ".yaml", ".yml",
    };
    if (path.n >= STR("Cargo.lock").n
        && !memcmp(path.p + path.n - STR("Cargo.lock").n, STR("Cargo.lock").p,
                   STR("Cargo.lock").n))
        return true;
    for (size_t i = 0; i < sizeof suffix / sizeof suffix[0]; i++) {
        size_t n = strlen(suffix[i]);
        if (path.n >= n && !memcmp(path.p + path.n - n, suffix[i], n))
            return true;
    }
    return false;
}

static size_t grep_matches(Str result) {
    size_t off = 0, n = 0;
    Str line;
    while (str_line(result, &off, &line)) {
        Str prefix, fragment;
        if (grep_fragment(line, &prefix, &fragment)) n++;
    }
    return n;
}


static Str grep_hint(const JVal *j) {
    Str path = json_str(j, STR("path"));
    Str glob = json_str(j, STR("glob"));
    if (glob.n >= 3 && glob.p[0] == '*' && glob.p[1] == '.'
        && memchr(glob.p + 1, '/', glob.n - 1) == NULL
        && memchr(glob.p + 1, '*', glob.n - 1) == NULL
        && memchr(glob.p + 1, '?', glob.n - 1) == NULL)
        return str_drop(glob, 1);
    return path;
}

static void write_syntax_lines(Str body, Str source, b8 grep,
                               const YhlResult *hl, Str gutter, size_t max,
                               size_t bytes) {
    size_t cap = line_cap(max);
    size_t off = 0, shown = 0, source_off = 0;
    Str line;
    while (shown < cap && str_line(body, &off, &line)) {
        tui_write_muted(gutter);
        Str head = clip(line, bytes);
        if (grep) {
            Str full_prefix, full_fragment;
            if (grep_fragment(line, &full_prefix, &full_fragment)) {
                size_t prefix_n =
                    full_prefix.n < head.n ? full_prefix.n : head.n;
                tui_write_muted((Str){head.p, prefix_n});
                Str fragment = str_drop(head, prefix_n);
                if (fragment.n && prefix_n == full_prefix.n) {
                    size_t at = tui_transcript_pos();
                    tui_write_source(fragment);
                    add_line_syntax(hl, source, source_off, fragment, at);
                }
                source_off += full_fragment.n + 1;
            } else {
                tui_write_muted(head);
            }
        } else {
            size_t at = tui_transcript_pos();
            tui_write_source(head);
            add_line_syntax(hl, source, (size_t)(line.p - source.p), head, at);
        }
        if (head.n < line.n) tui_write(STR(" ..."));
        tui_write(STR("\n"));
        shown++;
    }
    write_tail(gutter, str_lines(str_drop(body, off)), shown, max,
               tui_write_muted);
    tui_syntax_commit();
}

void render_tool_result(Str name, Str args, Str result, Arena *scratch, u32 id,
                        b8 expanded, u32 ms) {
    block_begin(id, expanded);
    if (str_starts(result, STR("ERROR: "))) {
        Str msg = str_drop(result, 7);
        size_t off = 0;
        Str first = msg;
        str_line(msg, &off, &first);
        tui_write_error(STR("\u2514\u2500 error: "));
        tui_write_error(clip(first, R_LINE_BYTES));
        write_elapsed(ms);
        tui_write_error(STR("\n"));
        block_end();
        return;
    }

    Str body = result, status = {0};

    b8 shell = str_eq(name, STR("bash")) || str_eq(name, STR("shell"))
               || str_eq(name, STR("job"));
    b8 have_status = shell && split_status(result, &body, &status);
    size_t mark = scratch ? scratch->off : 0;
    JVal *j = scratch && args.n ? json_parse(scratch, args) : NULL;
    Str path = json_str(j, STR("path"));
    static YhlResult hl;
    hl.n = 0;
    b8 source_code = false;
    b8 grep = str_eq(name, STR("grep"));
    char grep_source[AGENT_TOOL_RESULT_BYTES];
    Str syntax_source = result;
    if (str_eq(name, STR("read")) && path.n) {
        source_code = true;
        highlight_request(YHL_HINT_PATH, path, result, &hl);
    } else if (grep) {
        Str hint = grep_hint(j);
        if (hint.n) {
            size_t n = grep_batch(result, grep_source, sizeof grep_source);
            syntax_source = (Str){grep_source, n};
            source_code = n && source_filename(hint);
            if (source_code)
                highlight_request(YHL_HINT_PATH, hint, syntax_source, &hl);
        }
    }
    if (scratch) scratch->off = mark;
    tui_write_result(STR("\u2514\u2500 "));
    if (have_status) {
        tui_write_result(status);
    } else if (str_eq(name, STR("read"))) {
        write_count(str_lines(result), "line", tui_write_result);
    } else if (grep) {
        write_count(grep_matches(result), "match", tui_write_result);
    } else {
        size_t off = 0;
        Str first = body;
        str_line(body, &off, &first);
        tui_write_result(clip(first, R_LINE_BYTES));
        body = str_drop(body, off);
    }
    write_elapsed(ms);
    tui_write_result(STR("\n"));
    if (source_code) {
        write_syntax_lines(body, syntax_source, grep, &hl, STR("   "),
                           R_RESULT_LINES, R_LINE_BYTES);
    } else {
        write_lines(body, STR("   "), R_RESULT_LINES, R_LINE_BYTES,
                    tui_write_muted);
    }
    block_end();
}

/* Runs measured over batched fragments, mapped back onto the text they were
 * cut from: the batch keeps the fragments in line order, so one walk pairs
 * them. A run crossing the newline the batch separates fragments with is
 * split, since the bytes between them are not source. */
static void unbatch_syntax(const YhlResult *hl, Str body, b8 grep,
                           YhlResult *out) {
    out->n = 0;
    size_t off = 0, src = 0, k = 0;
    Str line;
    while (k < hl->n && str_line(body, &off, &line)) {
        Str prefix, fragment;
        if (grep ? !grep_fragment(line, &prefix, &fragment)
                 : !patch_fragment(line, &fragment))
            continue;
        size_t at = (size_t)(fragment.p - body.p), end = src + fragment.n;
        while (k < hl->n && hl->run[k].start < end) {
            size_t a = hl->run[k].start, b = hl->run[k].end;
            if (b > end) b = end;
            if (a < src) a = src;
            if (a < b && out->n < YHL_RUN_MAX)
                out->run[out->n++] =
                    (YhlRun){(u32)(at + a - src), (u32)(at + b - src),
                             hl->run[k].semantic};
            if (hl->run[k].end > end) break;
            k++;
        }
        src = end + 1;
    }
}

/* Highlight the fragments of a patch or a grep result, which are source
 * inside lines that are not. Both temporaries live in `scratch`, which the
 * caller rewinds; the runs land in `out` in `body` coordinates. */
static void batched_syntax(Str body, b8 grep, Str hint, Arena *scratch,
                           YhlResult *out) {
    if (!hint.n || !scratch) return;
    char *batch = arena_alloc(scratch, YHL_SOURCE_MAX, 1);
    YhlResult *hl = arena_alloc(scratch, sizeof *hl, alignof(YhlResult));
    if (!batch || !hl) return;
    size_t n = grep ? grep_batch(body, batch, YHL_SOURCE_MAX)
                    : patch_batch(body, batch, YHL_SOURCE_MAX);
    if (!n) return;
    if (highlight_request(YHL_HINT_PATH, hint, (Str){batch, n}, hl))
        unbatch_syntax(hl, body, grep, out);
}


Str render_call_text(Str name, Str args, Arena *scratch, size_t *shown,
                     YhlResult *syntax) {
    if (shown) *shown = R_ARG_LINES;
    if (syntax) syntax->n = 0;
    if (!scratch) return args;
    JVal *j = json_parse(scratch, args);
    Str path = json_str(j, STR("path"));
    if (str_eq(name, STR("page_fetch"))) path = json_str(j, STR("url"));
    Str cmd = json_str(j, STR("command"));
    Str content = json_str(j, STR("content"));
    Str patch = json_str(j, STR("patch"));
    Str query = str_eq(name, STR("grep"))   ? json_str(j, STR("pattern"))
                : str_eq(name, STR("find")) ? json_str(j, STR("name"))
                : str_eq(name, STR("internet_search"))
                    ? json_str(j, STR("query"))
                    : (Str){0};
    Str body;
    if (str_eq(name, STR("write"))) {
        body = content;
        if (syntax && path.n && content.n)
            highlight_request(YHL_HINT_PATH, path, content, syntax);
    } else if (patch.n) {
        if (shown) *shown = R_ARG_LINES * 2;
        body = patch;
        if (syntax) {
            char buf[R_TARGET_BYTES + 32];
            Str hint = {0};
            (void)patch_target(patch, buf, sizeof buf, &hint);
            batched_syntax(patch, false, hint, scratch, syntax);
        }
    } else if (cmd.n) {
        if (shown) *shown = R_ARG_LINES + 1;
        body = cmd;
        if (syntax && str_eq(name, STR("bash")))
            highlight_request(YHL_HINT_MARKDOWN_ALIAS, STR("bash"), cmd,
                              syntax);
    } else if (!path.n && !query.n) {
        body = args;
    } else {
        body = (Str){0};
    }
    return body;
}

Str render_result_text(Str name, Str args, Str result, Arena *scratch,
                       size_t *shown, YhlResult *syntax) {
    if (shown) *shown = R_RESULT_LINES;
    if (syntax) syntax->n = 0;
    if (str_starts(result, STR("ERROR: "))) return str_drop(result, 7);
    Str body = result, status = {0};
    b8 shell = str_eq(name, STR("bash")) || str_eq(name, STR("shell"));
    if (shell && split_status(result, &body, &status)) return body;
    b8 read_tool = str_eq(name, STR("read"));
    b8 grep = str_eq(name, STR("grep"));
    if (read_tool || grep) {
        JVal *j =
            syntax && scratch && args.n ? json_parse(scratch, args) : NULL;
        if (read_tool && j) {
            Str path = json_str(j, STR("path"));
            if (path.n) highlight_request(YHL_HINT_PATH, path, result, syntax);
        } else if (grep && j) {
            Str hint = grep_hint(j);
            if (source_filename(hint))
                batched_syntax(result, true, hint, scratch, syntax);
        }
        return result;
    }
    if (shown) *shown = R_RESULT_LINES + 1;
    return body;
}

Str render_shell_text(Str cmd, size_t *shown, YhlResult *syntax) {
    if (shown) *shown = R_ARG_LINES + 1;
    if (syntax) {
        syntax->n = 0;
        if (cmd.n)
            highlight_request(YHL_HINT_MARKDOWN_ALIAS, STR("bash"), cmd,
                              syntax);
    }
    return cmd;
}
