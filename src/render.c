#include "agent.h"

#include <stdio.h>
#include <string.h>

enum {
    R_ARG_LINES = 8,
    R_RESULT_LINES = 12,
    R_LINE_BYTES = 200,
    R_TARGET_BYTES = 120,
    R_PAIR_LINES = 64,
    /* NOTE: a 160-column terminal less its two body gutters. */
    R_SPLIT_COLS = 156,

    R_CMD_BYTES = 1024
};

typedef struct {
    u32 zone;
    b8 expanded;
    b8 full;
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
    return g_render.verbose || g_render.block.expanded || g_render.block.full;
}

static b8 render_in_full(Str name) {
    return str_eq(name, STR("write")) || str_eq(name, STR("patch"));
}

b8 render_unapplied(Str name, Str result) {
    if (!render_in_full(name)) return false;
    result = todo_note_strip(progress_note_strip(result));
    return str_starts(result, STR("ERROR: "))
           || str_starts(result, STR("DENIED: "));
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
static void batched_syntax(Str body, b8 grep, Str hint, Arena *scratch,
                           YhlResult *out);
static void write_patch_lines(Str patch, const YhlResult *hl, Str gutter,
                              size_t max);


static void write_clipped(Str s, size_t max, Sink sink) {
    Str head = clip(s, max);
    sink(head);
    if (head.n < s.n) sink(STR(" ..."));
}

static void write_search_pattern(Str s) {
    tui_write_styled(s, TUI_MONO);
}

static void write_search_path(Str s) {
    tui_write_styled(s, TUI_HEADING);
}

static void write_shell_description(Str s) {
    tui_write_styled(s, TUI_HEADING);
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

static void write_count(size_t n, const char *one, const char *many,
                        Sink sink) {
    char buf[64];
    i32 len = snprintf(buf, sizeof buf, "%zu %s", n, n == 1 ? one : many);
    if (len > 0) sink((Str){buf, (size_t)len});
}

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
    if (len > 0) tui_write_dim((Str){buf, (size_t)len});
}


typedef Sink (*LineStyle)(Str line);

static void write_tail(Str gutter, size_t rest, size_t shown, size_t max,
                       Sink sink) {
    char buf[64];
    i32 len;
    if (rest) {
        len = snprintf(buf, sizeof buf, "\u25be %zu more line%s\n", rest,
                       rest == 1 ? "" : "s");
    } else if (g_render.verbose || g_render.block.full) {
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
        tui_write_dim(gutter);
        write_clipped(line, bytes, put);
        put(STR("\n"));
        shown++;
    }
    write_tail(gutter, str_lines(str_drop(body, off)), shown, max,
               tui_write_dim);
}

static void write_lines(Str body, Str gutter, size_t max, size_t bytes,
                        Sink sink) {
    write_styled(body, gutter, max, bytes, sink, NULL);
}

static b8 patch_file_header(Str patch, Str line, size_t *off, Str *hint) {
    if (str_starts(line, STR("--- "))) {
        size_t peek = *off;
        Str next;
        if (!str_line(patch, &peek, &next) || !str_starts(next, STR("+++ ")))
            return false;
        Str path = str_trim(str_drop(next, 4));
        if (str_eq(path, STR("/dev/null"))) path = str_trim(str_drop(line, 4));
        const char *tab = (const char *)memchr(path.p, '\t', path.n);
        if (tab) path.n = (size_t)(tab - path.p);
        if (str_starts(path, STR("a/")) || str_starts(path, STR("b/")))
            path = str_drop(path, 2);
        *hint = path;
        *off = peek;
        return true;
    }
    static const Str prefixes[] = {
        {"*** Update File: ", sizeof("*** Update File: ") - 1},
        {"*** Add File: ", sizeof("*** Add File: ") - 1},
        {"*** Delete File: ", sizeof("*** Delete File: ") - 1}};
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) {
        if (str_starts(line, prefixes[i])) {
            *hint = str_trim(str_drop(line, prefixes[i].n));
            return true;
        }
    }
    return false;
}

static b8 patch_section(Str patch, size_t *off, Str *body, Str *hint) {
    Str line;
    b8 found = false;
    while (str_line(patch, off, &line)) {
        if (patch_file_header(patch, line, off, hint)) {
            found = true;
            break;
        }
    }
    if (!found) return false;
    size_t start = *off, end = *off;
    while (str_line(patch, off, &line)) {
        size_t peek = *off;
        Str next_hint;
        if (patch_file_header(patch, line, &peek, &next_hint)) {
            *off = end;
            break;
        }
        end = *off;
    }
    *body = (Str){patch.p + start, end - start};
    return true;
}

static Str patch_target(Str patch, char *buf, size_t cap) {
    size_t off = 0, files = 0;
    Str body, hint, first = {0};
    while (patch_section(patch, &off, &body, &hint))
        if (!files++) first = hint;
    if (files < 2) return first;
    i32 len =
        snprintf(buf, cap, "%.*s +%zu more", (i32)first.n, first.p, files - 1);
    return len > 0 ? (Str){buf, (size_t)len < cap ? (size_t)len : cap - 1}
                   : first;
}

static void render_todo_row(const TodoList *l, size_t i) {
    tui_write_dim(STR("\u2502 "));
    switch (l->status[i]) {
        case TODO_DONE:
            tui_write_result(STR("\u2713 "));
            tui_write_text(clip(todo_text(l, i), R_LINE_BYTES));
            break;
        case TODO_ACTIVE:
            tui_write_tool(STR("\u25b8 "));
            tui_write_text(clip(todo_text(l, i), R_LINE_BYTES));
            break;
        default:
            tui_write_dim(STR("\u25cb "));
            tui_write_text(clip(todo_text(l, i), R_LINE_BYTES));
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
        tui_write_dim(STR("\u2502 "));
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

static void render_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded,
                        const Conv *c, size_t slot, b8 unapplied) {
    block_begin(id, expanded);
    g_render.block.full = render_in_full(name) && !unapplied;
    size_t mark = scratch->off;
    if (str_eq(name, STR("todo"))) {
        render_todo_call(args, scratch, c, slot);
        scratch->off = mark;
        block_end();
        return;
    }
    JVal *j = json_parse(scratch, args);

    if (str_eq(name, STR("batch"))) {
        const JVal *steps = json_get(j, STR("steps"));
        tui_block();
        tui_write_tool(STR("\u25c6  batch "));
        write_count(steps && steps->type == J_ARR ? steps->u.arr.n : 0, "step",
                    "steps", tui_write_tool);
        tui_write_tool(STR("\n"));
        scratch->off = mark;
        block_end();
        return;
    }

    if (str_eq(name, STR("ask_user"))) {
        render_question(json_str(j, STR("question")));
        scratch->off = mark;
        block_end();
        return;
    }

    Str path = json_str(j, STR("path"));
    if (str_eq(name, STR("page_fetch"))) path = json_str(j, STR("url"));
    Str cmd = json_str(j, STR("command"));
    Str content = json_str(j, STR("content"));
    Str patch = json_str(j, STR("patch"));
    char patch_buf[R_TARGET_BYTES + 32];
    if (patch.n) path = patch_target(patch, patch_buf, sizeof patch_buf);
    /* NOTE: grep and find were removed in 0.10. A saved session may still
     * hold their calls, so they render and elide like read. */
    Str query = str_eq(name, STR("grep"))   ? json_str(j, STR("pattern"))
                : str_eq(name, STR("find")) ? json_str(j, STR("name"))
                : str_eq(name, STR("internet_search"))
                    ? json_str(j, STR("query"))
                    : (Str){0};
    Str target = query.n ? query : path.n ? path : cmd;
    b8 search =
        query.n && (str_eq(name, STR("find")) || str_eq(name, STR("grep")));

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
    Str shell_desc = str_eq(name, STR("bash"))
                         ? str_trim(json_str(j, STR("description")))
                         : (Str){0};
    if (target_cmd && shell_desc.n) {
        size_t desc_off = 0;
        str_line(shell_desc, &desc_off, &target);
        target_cmd = false;
    }

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
        syntax_source = patch;
        batched_syntax(patch, false, (Str){0}, scratch, &syntax);
    }

    tui_block();
    tui_write_tool(STR("\u25c6  "));
    if (search)
        tui_write_text(name);
    else
        tui_write_tool(name);
    Sink target_sink = search         ? write_search_pattern
                       : shell_desc.n ? write_shell_description
                                      : tui_write_tool;
    if (target.n) {
        if (shell_desc.n)
            tui_write_dim(STR(" \u00b7 "));
        else
            tui_write_tool(STR(" "));
        if (search) target_sink(STR("\""));
        size_t bytes = target_cmd ? R_CMD_BYTES : R_TARGET_BYTES;
        g_render.block.head_more = target.n > bytes;
        Str shown = clip(target, bytes);
        size_t at = tui_transcript_pos();
        if (source_code && target_cmd) {
            tui_write_source(shown);
            add_line_syntax(&syntax, syntax_source, 0, shown, at);
        } else {
            target_sink(shown);
        }
        if (shown.n < target.n) target_sink(STR(" ..."));
        if (search) target_sink(STR("\""));
    }
    if (shell_desc.n) {
        size_t line_off = 0;
        Str line;
        for (size_t i = 0; i < R_ARG_LINES && str_line(cmd, &line_off, &line);
             i++)
            g_render.block.head_more |= line.n > R_CMD_BYTES;
    }
    if (search) {
        Str root = path.n ? path : STR(".");
        tui_write_dim(STR(" in "));
        g_render.block.head_more |= root.n > R_TARGET_BYTES;
        write_clipped(root, R_TARGET_BYTES, write_search_path);
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
        write_patch_lines(patch, &syntax, STR("\u2502 "), R_ARG_LINES);
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
    } else if (!path.n && !query.n && !str_eq(name, STR("job"))) {
        write_lines(args, STR("\u2502 "), R_ARG_LINES, R_LINE_BYTES,
                    tui_write_muted);
    } else {
        write_tail(STR("\u2502 "), 0, 0, R_ARG_LINES, tui_write_dim);
    }

    scratch->off = mark;
    if (source_code) tui_syntax_commit();
    block_end();
}


void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded,
                      const Conv *c, size_t slot) {
    b8 unapplied = false;
    if (c && slot != CONV_NONE && render_in_full(name)
        && str_eq(c->tool_name[slot], name)) {
        size_t result = conv_result_slot(c, slot);
        unapplied =
            result != CONV_NONE && render_unapplied(name, c->text[result]);
    }
    render_call(name, args, scratch, id, expanded, c, slot, unapplied);
}

void render_batch_child_call(Str name, Str args, Arena *scratch, u32 id,
                             b8 expanded, const Conv *c, size_t slot) {
    tui_tool_nest_begin();
    render_tool_call(name, args, scratch, id, expanded, c, slot);
    tui_tool_nest_end();
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
        tui_write_dim(STR("\u2502 "));
        tui_write_text(line);
        tui_write(STR("\n"));
    }
}

void render_note(Str note) {
    tui_block();
    tui_write_dim(STR("[note to the model: "));
    tui_write_dim(note);
    tui_write_dim(STR("]\n"));
}

void render_task_header(u32 id, Str label, Str model, Str provider, b8 small,
                        b8 live) {
    char row[256];
    tui_block();
    i32 n = label.n ? snprintf(row, sizeof row, "\u25c6  task %u - %.*s\n", id,
                               (i32)str_clip_utf8(label, 64).n, label.p)
                    : snprintf(row, sizeof row, "\u25c6  task %u\n", id);
    if (n > 0) tui_write_tool((Str){row, (size_t)n});

    tui_write_dim(STR("\u2502 "));
    if (!model.n) {
        tui_write_muted(STR("the model this session uses"));
    } else {
        n = provider.n
                ? snprintf(row, sizeof row, "%.*s on %.*s", (i32)model.n,
                           model.p, (i32)provider.n, provider.p)
                : snprintf(row, sizeof row, "%.*s on this session's endpoint",
                           (i32)model.n, model.p);
        if (n > 0) tui_write_muted((Str){row, (size_t)n});
    }
    if (small) tui_write_muted(STR(" \u00b7 small model"));
    tui_write_muted(STR("\n"));
    tui_write_dim(STR("\u2502 "));
    tui_write_muted(live ? STR("in progress") : STR("finished"));
    tui_write_muted(STR(" \u00b7 Ctrl-O returns to the conversation\n"));
}

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

static u8 patch_marker(Str line, Str *fragment) {
    return patch_fragment(line, fragment) ? (u8)line.p[0] : 0;
}

static size_t patch_file_count(Str patch) {
    size_t off = 0, files = 0;
    Str body, hint;
    while (files < 2 && patch_section(patch, &off, &body, &hint)) files++;
    return files;
}

static b8 git_preamble(Str patch, Str line, size_t off) {
    if (!str_starts(line, STR("diff --git "))
        && !str_starts(line, STR("index ")))
        return false;
    static const Str meta[] = {
        {"index ", 6},   {"new file mode", 13}, {"deleted file mode", 17},
        {"old mode", 8}, {"new mode", 8},       {"similarity ", 11},
        {"rename ", 7}};
    Str next;
    for (size_t seen = 0; seen < 8 && str_line(patch, &off, &next); seen++) {
        size_t peek = off;
        Str hint;
        if (str_starts(next, STR("--- ")))
            return patch_file_header(patch, next, &peek, &hint);
        b8 known = false;
        for (size_t i = 0; !known && i < sizeof meta / sizeof *meta; i++)
            known = str_starts(next, meta[i]);
        if (!known) return false;
    }
    return false;
}

static b8 patch_next_row(Str patch, size_t *off, b8 several, Str *line,
                         Str *file) {
    while (str_line(patch, off, line)) {
        *file = (Str){0};
        size_t peek = *off;
        Str hint;
        if (patch_file_header(patch, *line, &peek, &hint)) {
            if (str_starts(*line, STR("*** Add File: "))
                || str_starts(*line, STR("*** Delete File: ")))
                return true;
            *off = peek;
            if (!several) continue;
            *file = hint;
            return true;
        }
        Str bare = str_trim(*line);
        if (str_eq(bare, STR("*** Begin Patch"))
            || str_eq(bare, STR("*** End Patch"))
            || git_preamble(patch, *line, *off))
            continue;
        return true;
    }
    return false;
}

typedef struct {
    Str del[R_PAIR_LINES], add[R_PAIR_LINES];
    size_t n, del_at, add_at;
} PatchPairs;

static void pair_block(Str patch, size_t off, PatchPairs *p) {
    size_t del = 0, add = 0;
    p->n = p->del_at = p->add_at = 0;
    Str line, fragment;
    while (str_line(patch, &off, &line)) {
        u8 marker = patch_marker(line, &fragment);
        if (marker == '-' && !add) {
            if (del == R_PAIR_LINES) return;
            p->del[del++] = fragment;
        } else if (marker == '+') {
            if (add == R_PAIR_LINES) return;
            p->add[add++] = fragment;
        } else {
            break;
        }
    }
    if (del == add) p->n = del;
}

static const Str *pair_partner(PatchPairs *p, u8 marker) {
    if (marker == '-') return p->del_at < p->n ? &p->add[p->del_at++] : NULL;
    return p->add_at < p->n ? &p->del[p->add_at++] : NULL;
}

static b8 word_byte(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x80 || u == '_' || (u >= '0' && u <= '9')
           || ((u | 0x20) >= 'a' && (u | 0x20) <= 'z');
}

static b8 inside_word(Str s, size_t at, size_t floor) {
    return at > floor && at < s.n && word_byte(s.p[at - 1])
           && word_byte(s.p[at]);
}

static b8 changed_span(Str line, Str other, size_t *a, size_t *b) {
    size_t shorter = line.n < other.n ? line.n : other.n;
    size_t pre = 0, suf = 0;
    while (pre < shorter && line.p[pre] == other.p[pre]) pre++;
    if (pre == line.n && pre == other.n) return false;
    while (suf < shorter - pre
           && line.p[line.n - 1 - suf] == other.p[other.n - 1 - suf])
        suf++;
    while (inside_word(line, pre, 0) || inside_word(other, pre, 0)) pre--;
    while (suf
           && (inside_word(line, line.n - suf, pre)
               || inside_word(other, other.n - suf, pre)))
        suf--;
    size_t indent = 0;
    while (indent < pre && (line.p[indent] == ' ' || line.p[indent] == '\t'))
        indent++;
    size_t kept = pre + suf - indent;
    if (!kept || kept * 4 < shorter - indent) return false;
    *a = pre;
    *b = line.n - suf;
    return true;
}

static void write_patch_line(Str patch, Str line, Str head, Str fragment_full,
                             const Str *partner, const YhlResult *hl,
                             Str gutter) {
    Sink sign = tui_write_muted, body = tui_write_dim, side = tui_write_dim,
         changed = tui_write_dim;
    if (line.p[0] == '+') {
        sign = tui_write_result;
        body = side = tui_write_diff_add;
        changed = tui_write_diff_add_changed;
    } else if (line.p[0] == '-') {
        sign = tui_write_error;
        body = side = tui_write_diff_del;
        changed = tui_write_diff_del_changed;
    }
    side(gutter);
    if (head.n) sign((Str){head.p, 1});
    Str fragment = str_drop(head, 1);
    if (fragment.n) {
        size_t at = tui_transcript_pos();
        size_t a = fragment.n, b = fragment.n;
        if (partner && changed_span(fragment_full, *partner, &a, &b)) {
            if (a > fragment.n) a = fragment.n;
            if (b > fragment.n) b = fragment.n;
        }
        body((Str){fragment.p, a});
        changed((Str){fragment.p + a, b - a});
        body(str_drop(fragment, b));
        if (line.p[0] != ' ')
            add_line_syntax(hl, patch, (size_t)(fragment_full.p - patch.p),
                            fragment, at);
    }
    if (head.n < line.n) side(STR(" ..."));
}

static void write_patch_meta(Str line, Str file, Str gutter) {
    Str head = clip(line, R_LINE_BYTES);
    tui_write_dim(gutter);
    if (file.n) {
        write_clipped(file, R_LINE_BYTES, write_search_path);
    } else if (str_starts(line, STR("+++ ")) || str_starts(line, STR("--- "))) {
        tui_write_dim((Str){head.p, 4});
        tui_write_styled(str_drop(head, 4), TUI_HEADING);
        if (head.n < line.n) tui_write_source(STR(" ..."));
    } else {
        tui_write_muted(head);
        if (head.n < line.n) tui_write_source(STR(" ..."));
    }
}

static void write_patch_unified(Str patch, const YhlResult *hl, Str gutter,
                                size_t max) {
    size_t cap = line_cap(max);
    b8 several = patch_file_count(patch) > 1;
    PatchPairs pairs;
    pairs.n = 0;
    u8 prev = 0;
    size_t off = 0, shown = 0;
    Str line, file;
    while (shown < cap && patch_next_row(patch, &off, several, &line, &file)) {
        Str head = clip(line, R_LINE_BYTES);
        Str fragment_full = {0};
        u8 marker = file.n ? 0 : patch_marker(line, &fragment_full);
        if (marker == '-' && prev != '-')
            pair_block(patch, (size_t)(line.p - patch.p), &pairs);
        else if (marker != '-' && marker != '+')
            pairs.n = 0;
        if (marker) {
            const Str *partner =
                marker == ' ' ? NULL : pair_partner(&pairs, marker);
            write_patch_line(patch, line, head, fragment_full, partner, hl,
                             gutter);
        } else {
            write_patch_meta(line, file, gutter);
        }
        tui_write(STR("\n"));
        shown++;
        prev = marker;
    }
    size_t rest = 0;
    while (patch_next_row(patch, &off, several, &line, &file)) rest++;
    write_tail(gutter, rest, shown, max, tui_write_dim);
    tui_syntax_commit();
}

typedef struct {
    Str text;
    size_t at, changed_a, changed_b;
    u8 marker;
    b8 started;
} SplitCell;

static SplitCell split_cell(u8 marker, Str fragment, const Str *partner) {
    SplitCell c = {.text = fragment, .marker = marker};
    c.changed_a = c.changed_b = fragment.n;
    if (partner
        && !changed_span(fragment, *partner, &c.changed_a, &c.changed_b))
        c.changed_a = c.changed_b = fragment.n;
    return c;
}

static b8 split_cell_done(const SplitCell *c) {
    return !c->marker || (c->started && c->at >= c->text.n);
}

static size_t cell_fit(Str s, size_t cells, size_t *used) {
    size_t i = 0, w = 0;
    while (i < s.n) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == '\t') {
            if (w + 4 > cells) break;
            w += 4;
            i++;
            continue;
        }
        if (c < 0x20) {
            i++;
            continue;
        }
        size_t run = i;
        while (run < s.n && (unsigned char)s.p[run] >= 0x20) run++;
        size_t run_cells = 0;
        i += tui_text_fit((Str){s.p + i, run - i}, cells - w, &run_cells);
        w += run_cells;
        if (i < run) break;
    }
    *used = w;
    return i;
}

static void write_spaces(size_t n, Sink sink) {
    static const char spaces[] = "                                ";
    while (n) {
        size_t take = n < sizeof spaces - 1 ? n : sizeof spaces - 1;
        sink((Str){spaces, take});
        n -= take;
    }
}

static void write_split_cell(SplitCell *c, size_t width, Str patch,
                             const YhlResult *hl, b8 last) {
    if (!c->marker) {
        if (!last) write_spaces(width, tui_write_dim);
        return;
    }
    Sink sign = tui_write_muted, body = tui_write_dim, changed = tui_write_dim;
    if (c->marker == '+') {
        sign = tui_write_diff_add_sign;
        body = tui_write_diff_add;
        changed = tui_write_diff_add_changed;
    } else if (c->marker == '-') {
        sign = tui_write_diff_del_sign;
        body = tui_write_diff_del;
        changed = tui_write_diff_del_changed;
    }
    if (c->started)
        body(STR(" "));
    else
        sign((Str){(const char *)&c->marker, 1});
    c->started = true;
    size_t room = width - 1, used = 0;
    Str rest = str_drop(c->text, c->at);
    size_t take = cell_fit(rest, room, &used);
    b8 cut = take < rest.n && !uncapped() && room > 4;
    if (cut) take = cell_fit(rest, room - 4, &used);
    size_t from = c->at, to = c->at + take;
    size_t a = c->changed_a < from ? from
               : c->changed_a > to ? to
                                   : c->changed_a;
    size_t b = c->changed_b < a ? a : c->changed_b > to ? to : c->changed_b;
    size_t at = tui_transcript_pos();
    body((Str){c->text.p + from, a - from});
    changed((Str){c->text.p + a, b - a});
    body((Str){c->text.p + b, to - b});
    if (c->marker != ' ' && take)
        add_line_syntax(hl, patch, (size_t)(c->text.p - patch.p) + from,
                        (Str){c->text.p + from, take}, at);
    c->at = cut ? c->text.n : to;
    if (cut) {
        body(STR(" ..."));
        used += 4;
    }
    if (!take && c->at < c->text.n) c->at = c->text.n;
    if (!last || c->marker != ' ')
        write_spaces(room > used ? room - used : 0, body);
}

typedef struct {
    Str patch, gutter;
    const YhlResult *hl;
    size_t left, right, cap, shown;
} SplitView;

static void write_split_row(SplitView *v, SplitCell *left, SplitCell *right) {
    do {
        tui_write_dim(v->gutter);
        write_split_cell(left, v->left, v->patch, v->hl, false);
        tui_write_dim(STR(" \u2502 "));
        write_split_cell(right, v->right, v->patch, v->hl, true);
        tui_write(STR("\n"));
        v->shown++;
    } while (!(split_cell_done(left) && split_cell_done(right)));
}

static size_t patch_run(Str patch, size_t *off, u8 marker, size_t *start) {
    size_t n = 0;
    *start = *off;
    Str line, fragment;
    for (size_t peek = *off; str_line(patch, &peek, &line)
                             && patch_marker(line, &fragment) == marker;
         *off = peek)
        n++;
    return n;
}

static size_t write_split_block(SplitView *v, size_t *off) {
    size_t del_at, add_at;
    size_t dels = patch_run(v->patch, off, '-', &del_at);
    size_t adds = patch_run(v->patch, off, '+', &add_at);
    b8 paired = dels == adds && dels <= R_PAIR_LINES;
    size_t rows = dels > adds ? dels : adds, consumed = 0;
    for (size_t i = 0; i < rows && v->shown < v->cap; i++) {
        Str line, del = {0}, add = {0};
        u8 del_marker = 0, add_marker = 0;
        if (i < dels && str_line(v->patch, &del_at, &line))
            del_marker = patch_marker(line, &del);
        if (i < adds && str_line(v->patch, &add_at, &line))
            add_marker = patch_marker(line, &add);
        SplitCell left = split_cell(del_marker, del, paired ? &add : NULL);
        SplitCell right = split_cell(add_marker, add, paired ? &del : NULL);
        write_split_row(v, &left, &right);
        consumed += (size_t)(del_marker != 0) + (size_t)(add_marker != 0);
    }
    return consumed;
}

static void write_patch_split(Str patch, const YhlResult *hl, Str gutter,
                              size_t max, size_t left, size_t right) {
    SplitView v = {.patch = patch,
                   .gutter = gutter,
                   .hl = hl,
                   .left = left,
                   .right = right,
                   .cap = line_cap(max)};
    b8 several = patch_file_count(patch) > 1;
    size_t total = 0, consumed = 0, off = 0;
    Str line, file;
    while (patch_next_row(patch, &off, several, &line, &file)) total++;
    off = 0;
    while (v.shown < v.cap
           && patch_next_row(patch, &off, several, &line, &file)) {
        Str fragment;
        u8 marker = file.n ? 0 : patch_marker(line, &fragment);
        if (marker == '-' || marker == '+') {
            off = (size_t)(line.p - patch.p);
            consumed += write_split_block(&v, &off);
            continue;
        }
        if (marker == ' ') {
            SplitCell left = split_cell(' ', fragment, NULL);
            SplitCell right = left;
            write_split_row(&v, &left, &right);
        } else {
            write_patch_meta(line, file, gutter);
            tui_write(STR("\n"));
            v.shown++;
        }
        consumed++;
    }
    write_tail(gutter, total > consumed ? total - consumed : 0, v.shown, max,
               tui_write_dim);
    tui_syntax_commit();
}

static b8 split_widths(Str patch, Str gutter, size_t *left, size_t *right) {
    if (!tui_body_cols()) return false;
    tui_width_fitted();
    size_t frame = tui_text_cells(gutter) + 3, cols = tui_row_cols();
    if (cols <= frame) return false;
    b8 several = patch_file_count(patch) > 1;
    b8 dels = false, adds = false;
    size_t widest = 0, off = 0;
    Str line, file, fragment;
    while (patch_next_row(patch, &off, several, &line, &file)) {
        u8 marker = file.n ? 0 : patch_marker(line, &fragment);
        if (!marker) continue;
        if (marker == '-') dels = true;
        if (marker == '+') adds = true;
        size_t cells = 0;
        cell_fit(fragment, SIZE_MAX, &cells);
        if (cells > widest) widest = cells;
    }
    if (!dels || !adds) return false;
    size_t inner = cols - frame;
    if (widest + 1 <= inner / 2) {
        *left = *right = widest + 1;
        return true;
    }
    if (tui_body_cols() < R_SPLIT_COLS) return false;
    *left = inner / 2;
    *right = inner - inner / 2;
    return true;
}

static void write_patch_lines(Str patch, const YhlResult *hl, Str gutter,
                              size_t max) {
    size_t left, right;
    if (split_widths(patch, gutter, &left, &right))
        write_patch_split(patch, hl, gutter, max, left, right);
    else
        write_patch_unified(patch, hl, gutter, max);
}

void render_diff_syntax(Str diff, Arena *scratch, YhlResult *out) {
    out->n = 0;
    batched_syntax(diff, false, (Str){0}, scratch, out);
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
        ".c",    ".h",    ".cc",  ".cpp", ".cxx", ".hh",   ".hpp",    ".hxx",
        ".rs",   ".go",   ".py",  ".pyw", ".js",  ".jsx",  ".mjs",    ".cjs",
        ".ts",   ".mts",  ".cts", ".tsx", ".sh",  ".bash", ".bashrc", ".json",
        ".toml", ".yaml", ".yml", ".cs",  ".csx",
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

static const JVal *ask_option(const JVal *j, Str answer) {
    const JVal *opts = json_get(j, STR("options"));
    if (!opts || opts->type != J_ARR) return NULL;
    size_t off = 0;
    Str label = answer;
    str_line(answer, &off, &label);
    for (size_t i = 0; i < opts->u.arr.n; i++) {
        const JVal *o = json_at(opts, i);
        if (label.n && str_eq(json_str(o, STR("label")), label)) return o;
    }
    return NULL;
}

static void render_ask_result(Str args, Str result, Arena *scratch, u32 ms) {
    size_t mark = scratch ? scratch->off : 0;
    JVal *j = scratch && args.n ? json_parse(scratch, args) : NULL;
    Str detail = json_str(ask_option(j, result), STR("detail"));

    size_t off = 0;
    Str answer = result;
    str_line(result, &off, &answer);
    tui_write_result(STR("\u2514\u2500 "));
    tui_write_result(answer);
    write_elapsed(ms);
    tui_write_result(STR("\n"));

    Str rest = str_drop(result, off);
    Str body = rest;
    if (detail.n && scratch) {
        Buf b;
        buf_init(&b, scratch, detail.n + rest.n + 2);
        buf_puts(&b, detail);
        if (rest.n) {
            buf_puts(&b, STR("\n"));
            buf_puts(&b, rest);
        }
        if (buf_ok(&b)) body = buf_finish(&b);
    }
    write_lines(body, STR("   "), SIZE_MAX, SIZE_MAX, tui_write_muted);
    if (scratch) scratch->off = mark;
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
        tui_write_dim(gutter);
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
               tui_write_dim);
    tui_syntax_commit();
}

static b8 render_batch_result(Str args, Str result, Arena *scratch, u32 id,
                              b8 expanded, u32 ms) {
    if (!scratch || !str_starts(result, STR("{"))) return false;
    size_t mark = scratch->off;
    const JVal *root = json_parse(scratch, result);
    const JVal *rows = json_get(root, STR("steps"));
    const JVal *call = json_parse(scratch, args);
    const JVal *steps = json_get(call, STR("steps"));
    Str status = json_str(root, STR("status"));
    if (!rows || rows->type != J_ARR || rows->u.arr.n > AGENT_MAX_BATCH_STEPS
        || !status.n) {
        scratch->off = mark;
        return false;
    }
    for (size_t i = 0; i < rows->u.arr.n; i++) {
        const JVal *row = &rows->u.arr.items[i];
        Str name = json_str(row, STR("tool"));
        if (str_eq(name, STR("batch"))) {
            scratch->off = mark;
            return false;
        }
    }
    for (size_t i = 0; i < rows->u.arr.n; i++) {
        const JVal *row = &rows->u.arr.items[i];
        Str name = json_str(row, STR("tool"));
        if (!name.n) continue;
        Buf child;
        buf_init(&child, scratch, 256);
        const JVal *input = steps && steps->type == J_ARR && i < steps->u.arr.n
                                ? json_get(&steps->u.arr.items[i], STR("args"))
                                : NULL;
        if (input)
            json_write(&child, input);
        else
            buf_puts(&child, STR("{}"));
        if (!buf_ok(&child)) break;
        Str child_args = buf_finish(&child);
        tui_tool_nest_begin();
        render_call(name, child_args, scratch, id, expanded, NULL, CONV_NONE,
                    render_unapplied(name, json_str(row, STR("result"))));
        tui_tool_nest_end();
        if (str_eq(json_str(row, STR("status")), STR("running"))) continue;
        const JVal *duration = json_get(row, STR("ms"));
        u32 child_ms = duration && duration->type == J_NUM && duration->u.n >= 0
                               && duration->u.n <= UINT32_MAX
                           ? (u32)duration->u.n
                           : 0;
        render_batch_child_result(name, child_args,
                                  json_str(row, STR("result")), scratch, id,
                                  expanded, child_ms);
    }
    const JVal *tried = json_get(root, STR("attempted"));
    const JVal *total = json_get(root, STR("total"));
    size_t n = total && total->type == J_NUM && total->u.n >= 0
                       && total->u.n <= AGENT_MAX_BATCH_STEPS
                   ? (size_t)total->u.n
                   : rows->u.arr.n;
    size_t attempted =
        tried && tried->type == J_NUM && tried->u.n >= 0 && tried->u.n <= (f64)n
            ? (size_t)tried->u.n
            : rows->u.arr.n;
    Buf summary;
    buf_init(&summary, scratch, 128);
    batch_summary(&summary, str_clip_utf8(status, 32), attempted, n);
    if (buf_ok(&summary))
        render_tool_result(STR("batch"), (Str){0}, buf_finish(&summary),
                           scratch, id, expanded, ms);
    scratch->off = mark;
    return true;
}

static void render_tool_result_nested(Str name, Str args, Str result,
                                      Arena *scratch, u32 id, b8 expanded,
                                      u32 ms, b8 nested) {
    if (str_eq(name, STR("batch"))
        && render_batch_result(args, result, scratch, id, expanded, ms))
        return;
    block_begin(id, expanded);
    result = todo_note_strip(progress_note_strip(result));
    b8 dim_edge = nested || str_eq(name, STR("batch"));
    if (str_eq(name, STR("batch")) && str_starts(result, STR("batch ")))
        tui_write_dim(STR("\u2502\n"));
    if (str_starts(result, STR("ERROR: "))) {
        Str msg = str_drop(result, 7);
        size_t off = 0;
        Str first = msg;
        str_line(msg, &off, &first);
        Sink edge = dim_edge ? tui_write_dim : tui_write_error;
        edge(STR("\u2514\u2500 "));
        tui_write_error(STR("error: "));
        tui_write_error(clip(first, R_LINE_BYTES));
        write_elapsed(ms);
        tui_write_error(STR("\n"));
        block_end();
        return;
    }
    g_render.block.full = render_in_full(name);

    if (str_eq(name, STR("ask")) || str_eq(name, STR("ask_user"))) {
        render_ask_result(args, result, scratch, ms);
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
    Sink edge = dim_edge ? tui_write_dim : tui_write_result;
    edge(STR("\u2514\u2500 "));
    if (have_status) {
        tui_write_result(status);
    } else if (str_eq(name, STR("read"))) {
        write_count(str_lines(result), "line", "lines", tui_write_result);
    } else if (grep) {
        write_count(grep_matches(result), "match", "matches", tui_write_result);
        if (str_eq(str_trim(body), STR("no matches"))) body = (Str){0};
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

void render_tool_result(Str name, Str args, Str result, Arena *scratch, u32 id,
                        b8 expanded, u32 ms) {
    render_tool_result_nested(name, args, result, scratch, id, expanded, ms,
                              false);
}

void render_batch_child_result(Str name, Str args, Str result, Arena *scratch,
                               u32 id, b8 expanded, u32 ms) {
    tui_tool_nest_begin();
    render_tool_result_nested(name, args, result, scratch, id, expanded, ms,
                              true);
    tui_tool_nest_end();
}

static void unbatch_syntax(const YhlResult *hl, Str body, b8 grep,
                           size_t body_off, YhlResult *out) {
    size_t off = 0, src = 0, k = 0;
    Str line;
    while (k < hl->n && str_line(body, &off, &line)) {
        Str prefix, fragment;
        if (grep ? !grep_fragment(line, &prefix, &fragment)
                 : !patch_fragment(line, &fragment))
            continue;
        size_t at = body_off + (size_t)(fragment.p - body.p);
        size_t end = src + fragment.n;
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

static void batched_syntax(Str body, b8 grep, Str hint, Arena *scratch,
                           YhlResult *out) {
    if (!scratch || (grep && !hint.n)) return;
    size_t mark = scratch->off;
    char *batch = arena_alloc(scratch, YHL_SOURCE_MAX, 1);
    YhlResult *hl = arena_alloc(scratch, sizeof *hl, alignof(YhlResult));
    if (batch && hl) {
        if (grep) {
            size_t n = grep_batch(body, batch, YHL_SOURCE_MAX);
            if (n
                && highlight_request(YHL_HINT_PATH, hint, (Str){batch, n}, hl))
                unbatch_syntax(hl, body, true, 0, out);
        } else {
            size_t off = 0, files = 0;
            Str section;
            while (files < AGENT_MAX_PATCH_FILES && out->n < YHL_RUN_MAX
                   && patch_section(body, &off, &section, &hint)) {
                files++;
                size_t n = patch_batch(section, batch, YHL_SOURCE_MAX);
                if (n
                    && highlight_request(YHL_HINT_PATH, hint, (Str){batch, n},
                                         hl))
                    unbatch_syntax(hl, section, false,
                                   (size_t)(section.p - body.p), out);
            }
        }
    }
    scratch->off = mark;
}


static void syntax_append(YhlResult *out, const YhlResult *add, size_t at,
                          size_t limit) {
    for (size_t k = 0; k < add->n && out->n < YHL_RUN_MAX; k++) {
        size_t a = add->run[k].start;
        size_t b = add->run[k].end < limit ? add->run[k].end : limit;
        if (a >= b) continue;
        if (at + b > UINT32_MAX) return;
        out->run[out->n++] =
            (YhlRun){(u32)(at + a), (u32)(at + b), add->run[k].semantic};
    }
}

static Str render_batch_text(Str args, Str result, b8 input, Arena *scratch,
                             YhlResult *syntax) {
    Str source = input ? args : result;
    const JVal *root = json_parse(scratch, source);
    const JVal *rows = json_get(root, STR("steps"));
    const JVal *call = input ? root : json_parse(scratch, args);
    const JVal *steps = json_get(call, STR("steps"));
    if (!rows || rows->type != J_ARR || rows->u.arr.n > AGENT_MAX_BATCH_STEPS)
        return source;
    for (size_t i = 0; i < rows->u.arr.n; i++)
        if (str_eq(json_str(&rows->u.arr.items[i], STR("tool")), STR("batch")))
            return source;
    YhlResult *child_syntax =
        syntax ? arena_alloc(scratch, sizeof *child_syntax, alignof(YhlResult))
               : NULL;
    Buf out;
    buf_init(&out, scratch, 4096);
    for (size_t i = 0; i < rows->u.arr.n; i++) {
        const JVal *row = &rows->u.arr.items[i];
        Str name = json_str(row, STR("tool"));
        if (!name.n) continue;
        const JVal *child = steps && steps->type == J_ARR && i < steps->u.arr.n
                                ? json_get(&steps->u.arr.items[i], STR("args"))
                                : NULL;
        Buf encoded;
        buf_init(&encoded, scratch, 256);
        if (child)
            json_write(&encoded, child);
        else
            buf_puts(&encoded, STR("{}"));
        if (!buf_ok(&encoded)) {
            if (syntax) syntax->n = 0;
            return STR("out of memory opening batch");
        }
        Str child_args = buf_finish(&encoded);
        if (out.n) buf_putc(&out, '\n');
        buf_putf(&out, "%zu. %.*s", i + 1, (i32)str_clip_utf8(name, 128).n,
                 name.p);
        Str path = json_str(child, STR("path"));
        if (path.n) {
            buf_putc(&out, ' ');
            buf_puts(&out, path);
        }
        buf_putc(&out, '\n');
        Str row_result = json_str(row, STR("result"));
        Str body = input ? render_call_text(name, child_args, scratch, NULL,
                                            child_syntax,
                                            render_unapplied(name, row_result))
                         : render_result_text(name, child_args, row_result,
                                              scratch, NULL, child_syntax);
        if (child_syntax && buf_ok(&out))
            syntax_append(syntax, child_syntax, out.n, body.n);
        buf_puts(&out, body);
        if (body.n && body.p[body.n - 1] != '\n') buf_putc(&out, '\n');
    }
    if (buf_ok(&out)) return buf_finish(&out);
    if (syntax) syntax->n = 0;
    return STR("out of memory opening batch");
}

Str render_call_text(Str name, Str args, Arena *scratch, size_t *shown,
                     YhlResult *syntax, b8 unapplied) {
    b8 full = render_in_full(name) && !unapplied;
    if (shown) *shown = full ? SIZE_MAX : R_ARG_LINES;
    if (syntax) syntax->n = 0;
    if (!scratch) return args;
    if (str_eq(name, STR("batch")))
        return render_batch_text(args, (Str){0}, true, scratch, syntax);
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
        body = patch;
        if (syntax) batched_syntax(patch, false, (Str){0}, scratch, syntax);
    } else if (cmd.n) {
        b8 described = str_eq(name, STR("bash"))
                       && str_trim(json_str(j, STR("description"))).n;
        if (shown) *shown = described ? R_ARG_LINES : R_ARG_LINES + 1;
        body = cmd;
        if (syntax && str_eq(name, STR("bash")))
            highlight_request(YHL_HINT_MARKDOWN_ALIAS, STR("bash"), cmd,
                              syntax);
    } else if (str_eq(name, STR("ask_user"))) {
        body = json_str(j, STR("question"));
    } else if (str_eq(name, STR("job"))) {
        body = (Str){0};
    } else if (!path.n && !query.n) {
        body = args;
    } else {
        body = (Str){0};
    }
    return body;
}

Str render_result_text(Str name, Str args, Str result, Arena *scratch,
                       size_t *shown, YhlResult *syntax) {
    b8 full = render_in_full(name);
    if (shown) *shown = full ? SIZE_MAX : R_RESULT_LINES;
    if (syntax) syntax->n = 0;
    result = todo_note_strip(progress_note_strip(result));
    if (str_starts(result, STR("ERROR: "))) return str_drop(result, 7);
    if (str_eq(name, STR("batch")) && scratch && str_starts(result, STR("{")))
        return render_batch_text(args, result, false, scratch, syntax);
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
    if (shown && !full) *shown = R_RESULT_LINES + 1;
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
