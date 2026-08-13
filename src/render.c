#include "agent.h"

#include <stdio.h>
#include <string.h>

enum {
    R_ARG_LINES    = 8,    /* input lines shown under a call's header  */
    R_RESULT_LINES = 12,   /* output lines shown under a result        */
    R_LINE_BYTES   = 200,  /* a single line's share of the transcript  */
    R_TARGET_BYTES = 120   /* the header's path or command             */
};

static b8 g_verbose;

static b8 g_expanded;
static u32 g_zone;

void render_set_verbose(b8 on) { g_verbose = on; }
b8 render_verbose(void) { return g_verbose; }

static b8 uncapped(void) { return g_verbose || g_expanded; }

static size_t line_cap(size_t max) { return uncapped() ? (size_t)-1 : max; }

static Str clip(Str s, size_t max) {
    return uncapped() ? s : str_clip_utf8(s, max);
}

typedef void (*Sink)(Str);

static void add_line_syntax(const YhlResult *hl, Str source,
                            size_t source_off, Str shown,
                            size_t transcript_off);
static void write_syntax_lines(Str body, Str source, b8 grep,
                               const YhlResult *hl, Str gutter, size_t max);
static size_t patch_batch(Str patch, char *out, size_t cap);
static void write_patch_lines(Str patch, Str source, const YhlResult *hl,
                              Str gutter, size_t max);

/* `s` up to `max`, with the ellipsis a cut earns and nothing when it does
 * not: what was left out is said in one place. */
static void write_clipped(Str s, size_t max, Sink sink) {
    Str head = clip(s, max);
    sink(head);
    if (head.n < s.n) sink(STR(" ..."));
}

/* The block being written, for the length of one render_* call. */
static void block_begin(u32 id, b8 expanded) {
    /* A block is where a re-render can put the viewport back, so it is a
     * landmark whether it was written live or replayed. */
    tui_pin(id);
    g_zone = id;
    g_expanded = expanded;
}
static void block_end(void) {
    g_zone = 0;
    g_expanded = false;
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
    i32 len = limit
        ? snprintf(buf, sizeof buf, " lines %zu-%zu", offset, offset + limit - 1)
        : snprintf(buf, sizeof buf, " from line %zu", offset);
    if (len > 0) tui_write_tool((Str){ buf, (size_t)len });
}

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
        write_clipped(line, R_LINE_BYTES, put);
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
    i32 len = snprintf(buf, cap, "%.*s +%zu more", (i32)first.n, first.p,
                       files - 1);
    return len > 0 ? (Str){ buf, (size_t)len < cap ? (size_t)len : cap - 1 }
                   : first;
}

void render_tool_call(Str name, Str args, Arena *scratch, u32 id, b8 expanded) {
    block_begin(id, expanded);
    size_t mark = scratch->off;
    JVal *j = json_parse(scratch, args);

    Str path = json_str(j, STR("path"));
    if (str_eq(name, STR("page_fetch"))) path = json_str(j, STR("url"));
    Str cmd  = json_str(j, STR("command"));
    Str content = json_str(j, STR("content"));
    Str patch = json_str(j, STR("patch"));
    char patch_buf[R_TARGET_BYTES + 32];
    Str patch_hint = {0};
    if (patch.n)
        path = patch_target(patch, patch_buf, sizeof patch_buf, &patch_hint);
    Str query = str_eq(name, STR("grep")) ? json_str(j, STR("pattern"))
              : str_eq(name, STR("find")) ? json_str(j, STR("name"))
              : str_eq(name, STR("internet_search")) ? json_str(j, STR("query"))
              : (Str){0};
    Str target = query.n ? query : path.n ? path : cmd;
    size_t cmd_off = 0;
    if (!path.n && cmd.n) str_line(cmd, &cmd_off, &target);
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
        syntax_source = (Str){ patch_source, n };
        if (n && patch_hint.n)
            highlight_request(YHL_HINT_PATH, patch_hint, syntax_source,
                              &syntax);
    }

    tui_block();
    tui_write_tool(STR("\u25c6  "));
    tui_write_tool(name);
    if (target.n) {
        tui_write_tool(STR(" "));
        Str shown = clip(target, R_TARGET_BYTES);
        size_t at = tui_transcript_pos();
        if (source_code && cmd.n) {
            tui_write_text(shown);
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
                               STR("\u2502 "), R_ARG_LINES);
        else
            write_lines(content, STR("\u2502 "), R_ARG_LINES,
                        tui_write_muted);
    } else if (patch.n) {
        /* Twice a call's usual allowance: a hunk spends most of its lines on
         * the context that locates it. */
        write_patch_lines(patch, syntax_source, &syntax, STR("\u2502 "),
                          R_ARG_LINES * 2);
    } else if (cmd.n) {
        if (source_code)
            write_syntax_lines(str_drop(cmd, cmd_off), syntax_source, false,
                               &syntax,
                               STR("\u2502 "), R_ARG_LINES);
        else
            write_lines(str_drop(cmd, cmd_off), STR("\u2502 "),
                        R_ARG_LINES, tui_write_muted);
    } else if (!path.n && !query.n) {
        write_lines(args, STR("\u2502 "), R_ARG_LINES,
                    tui_write_muted);
    }

    scratch->off = mark;
    if (source_code) tui_syntax_commit();
    block_end();
}

/* A '!' run has no JSON to unpack: the command is the header and its trailing
 * lines the input preview. */
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
    Str shown = clip(first, R_TARGET_BYTES);
    size_t at = tui_transcript_pos();
    if (cmd.n) {
        tui_write_text(shown);
        add_line_syntax(&syntax, cmd, 0, shown, at);
    } else {
        tui_write_tool(shown);
    }
    if (shown.n < first.n) tui_write_tool(STR(" ..."));
    tui_write_tool(STR("\n"));
    if (cmd.n)
        write_syntax_lines(str_drop(cmd, off), cmd, false, &syntax,
                           STR("\u2502 "), R_ARG_LINES);
    else
        write_lines(str_drop(cmd, off), STR("\u2502 "), R_ARG_LINES,
                    tui_write_muted);

    if (cmd.n) tui_syntax_commit();
    block_end();
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

static size_t rendered_bytes(Str s) {
    size_t n = 0;
    for (size_t i = 0; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == '\t') n += 4;
        else if (c == '\r' || c < 0x20) continue;
        else n++;
    }
    return n;
}

static void add_line_syntax(const YhlResult *hl, Str source,
                            size_t source_off, Str shown, size_t transcript_off) {
    size_t shown_end = source_off + shown.n;
    for (size_t i = 0; i < hl->n; i++) {
        size_t a = hl->run[i].start;
        size_t b = hl->run[i].end;
        if (b <= source_off) continue;
        if (a >= shown_end) break;
        if (a < source_off) a = source_off;
        if (b > shown_end) b = shown_end;
        if (a >= b) continue;
        size_t dst_a = transcript_off
                     + rendered_bytes((Str){ source.p + source_off,
                                              a - source_off });
        size_t dst_b = dst_a + rendered_bytes((Str){ source.p + a, b - a });
        tui_syntax_add(dst_a, dst_b, hl->run[i].semantic);
    }
}

/* Headers and hunk coordinates are diff syntax. The byte after a line marker
 * is source syntax, including context lines whose marker is a space. */
static b8 patch_fragment(Str line, Str *fragment) {
    if (!line.n || str_starts(line, STR("+++ "))
        || str_starts(line, STR("--- "))
        || str_starts(line, STR("@@")))
        return false;
    if (line.p[0] != '+' && line.p[0] != '-' && line.p[0] != ' ')
        return false;
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
            Sink marker = line.p[0] == '+' ? tui_write_result
                        : line.p[0] == '-' ? tui_write_error
                                          : tui_write_muted;
            if (head.n) marker((Str){ head.p, 1 });
            Str fragment = str_drop(head, 1);
            if (fragment.n) {
                size_t at = tui_transcript_pos();
                tui_write_text(fragment);
                add_line_syntax(hl, source, source_off, fragment, at);
            }
            source_off += full_fragment.n + 1;
        } else if (str_starts(line, STR("+++ "))
                   || str_starts(line, STR("--- "))) {
            tui_write_tool(head);
        } else {
            tui_write_muted(head);
        }
        if (head.n < line.n) tui_write_text(STR(" ..."));
        tui_write(STR("\n"));
        shown++;
    }
    size_t rest = str_lines(str_drop(patch, off));
    char tail[64];
    i32 len;
    if (rest) {
        len = snprintf(tail, sizeof tail, "\u25be %zu more line%s\n",
                       rest, rest == 1 ? "" : "s");
    } else if (g_expanded && !g_verbose && shown > max) {
        len = snprintf(tail, sizeof tail, "\u25b4 show less\n");
    } else {
        tui_syntax_commit();
        return;
    }
    tui_zone_begin(g_zone);
    tui_write_muted(gutter);
    if (len > 0) tui_write_muted((Str){ tail, (size_t)len });
    tui_zone_end();
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
    *prefix = (Str){ line.p, found };
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
        ".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx",
        ".rs", ".go", ".py", ".pyw", ".js", ".jsx", ".mjs", ".cjs",
        ".ts", ".mts", ".cts", ".tsx", ".sh", ".bash", ".bashrc",
        ".json", ".toml", ".yaml", ".yml",
    };
    if (path.n >= STR("Cargo.lock").n
        && !memcmp(path.p + path.n - STR("Cargo.lock").n,
                   STR("Cargo.lock").p, STR("Cargo.lock").n))
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

static void write_syntax_lines(Str body, Str source, b8 grep,
                               const YhlResult *hl, Str gutter, size_t max) {
    size_t cap = line_cap(max);
    size_t off = 0, shown = 0, source_off = 0;
    Str line;
    while (shown < cap && str_line(body, &off, &line)) {
        tui_write_muted(gutter);
        Str head = clip(line, R_LINE_BYTES);
        if (grep) {
            Str full_prefix, full_fragment;
            if (grep_fragment(line, &full_prefix, &full_fragment)) {
                size_t prefix_n = full_prefix.n < head.n ? full_prefix.n : head.n;
                tui_write_muted((Str){ head.p, prefix_n });
                Str fragment = str_drop(head, prefix_n);
                if (fragment.n && prefix_n == full_prefix.n) {
                    size_t at = tui_transcript_pos();
                    tui_write_text(fragment);
                    add_line_syntax(hl, source, source_off, fragment, at);
                }
                source_off += full_fragment.n + 1;
            } else {
                tui_write_muted(head);
            }
        } else {
            size_t at = tui_transcript_pos();
            tui_write_text(head);
            add_line_syntax(hl, source, (size_t)(line.p - source.p), head, at);
        }
        if (head.n < line.n) tui_write(STR(" ..."));
        tui_write(STR("\n"));
        shown++;
    }
    size_t rest = str_lines(str_drop(body, off));
    char tail[64];
    i32 len;
    if (rest) {
        len = snprintf(tail, sizeof tail, "\u25be %zu more line%s\n",
                       rest, rest == 1 ? "" : "s");
    } else if (g_expanded && !g_verbose && shown > max) {
        len = snprintf(tail, sizeof tail, "\u25b4 show less\n");
    } else {
        tui_syntax_commit();
        return;
    }
    tui_zone_begin(g_zone);
    tui_write_muted(gutter);
    if (len > 0) tui_write_muted((Str){ tail, (size_t)len });
    tui_zone_end();
    tui_syntax_commit();
}

void render_tool_result(Str name, Str args, Str result, Arena *scratch,
                        u32 id, b8 expanded, u32 ms) {
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
    b8 shell = str_eq(name, STR("bash")) || str_eq(name, STR("shell"));
    b8 have_status = shell && split_status(result, &body, &status);
    size_t mark = scratch ? scratch->off : 0;
    JVal *j = scratch && args.n ? json_parse(scratch, args) : NULL;
    Str path = json_str(j, STR("path"));
    Str glob = json_str(j, STR("glob"));
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
        Str hint = path;
        if (glob.n >= 3 && glob.p[0] == '*' && glob.p[1] == '.'
            && memchr(glob.p + 1, '/', glob.n - 1) == NULL
            && memchr(glob.p + 1, '*', glob.n - 1) == NULL
            && memchr(glob.p + 1, '?', glob.n - 1) == NULL)
            hint = str_drop(glob, 1);
        if (hint.n) {
            size_t n = grep_batch(result, grep_source, sizeof grep_source);
            syntax_source = (Str){ grep_source, n };
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
                           R_RESULT_LINES);
    } else {
        write_lines(body, STR("   "), R_RESULT_LINES, tui_write_muted);
    }
    block_end();
}
