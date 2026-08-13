#include "agent.h"

#include <string.h>

#define MD_PREFIX_MAX 24
/* Past this an unresolved opener is taken literally, which bounds how far a
 * stray '*' holds the stream back. */
#define MD_PEND_MAX 512
#define MD_RULE_MAX 60
#define MD_LINE_MAX 256
#define MD_TABLE_ROWS 64
#define MD_TABLE_COLS 32
#define MD_TABLE_BYTES (32u << 10)
/* A table cell is laid out before it is drawn, so its inline markup is
 * resolved into styled runs first. A cell past either cap is drawn as
 * written rather than truncated. */
#define MD_CELL_MAX 512
#define MD_CELL_RUNS 32
#define MD_HL_LINES (YHL_SOURCE_MAX / 2u + 2u)

enum {
    MD_BLOCK_PLAIN, MD_BLOCK_HEAD, MD_BLOCK_QUOTE, MD_BLOCK_CODE,
    MD_BLOCK_LIST
};
enum { MD_SPAN_NONE, MD_SPAN_WAIT, MD_SPAN_DONE };

/* One resolved cell: `text` is what it draws, and run `i` styles the bytes
 * from the previous run's end to `run_end[i]`. `text` points into `buf`
 * unless the cell overflowed, when it aliases the row and carries no runs. */
typedef struct {
    Str    text;
    size_t run_end[MD_CELL_RUNS];
    u8     run_style[MD_CELL_RUNS];
    size_t runs;
    size_t n;
    b8     over;
    char   buf[MD_CELL_MAX];
} MdCell;

static struct {
    b8     raw;
    b8     muted;      /* plain prose belongs to a thinking trace */
    b8     fence;      /* inside a fenced code block */
    char   fence_mark;
    b8     in_body;    /* the line's block marker has been resolved */
    b8     skip_line;  /* the rest of the line is a marker's info string */
    i32    block;
    char   prefix[MD_PREFIX_MAX];
    size_t prefix_n;
    char   pend[MD_PEND_MAX];
    size_t pend_n;
    char   last;       /* last byte consumed, for the flanking rules */
    char   line[MD_LINE_MAX];
    size_t line_n;
    b8     line_long;
    char   held[MD_LINE_MAX];
    size_t held_n;
    b8     held_eol;
    char   table[MD_TABLE_BYTES];
    size_t table_off[MD_TABLE_ROWS];
    size_t table_len[MD_TABLE_ROWS];
    size_t table_n;
    size_t table_bytes;
    size_t table_cols;
    u8     table_align[MD_TABLE_COLS];
    MdCell *cap;                  /* inline output goes here while set */
    MdCell cell[MD_TABLE_COLS];   /* the row being drawn */
    MdCell measured;              /* one cell, while widths are taken */
    char   hl_alias[YHL_ALIAS_MAX];
    size_t hl_alias_n;
    char   hl_source[YHL_SOURCE_MAX];
    u32    hl_src_start[MD_HL_LINES];
    u32    hl_src_end[MD_HL_LINES];
    size_t hl_dst_start[MD_HL_LINES];
    size_t hl_source_n;
    size_t hl_line_n;
    u64    hl_epoch;
    b8     hl_active;
    b8     hl_line_open;
} g_md;

static b8 md_space(char c) { return c == ' ' || c == '\t'; }
static b8 md_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80;
}
static b8 md_all(Str s, char c) {
    for (size_t i = 0; i < s.n; i++) if (s.p[i] != c) return false;
    return s.n != 0;
}
static b8 md_marker(char c) {
    return c == '*' || c == '_' || c == '`' || c == '[' || c == '~'
        || c == '<' || c == '!' || c == '\\';
}

static void md_cell_put(MdCell *c, Str s, TuiStyle style);

static void md_emit(Str s, TuiStyle style) {
    if (g_md.cap) { md_cell_put(g_md.cap, s, style); return; }
    if (s.n) tui_write_styled(s, style);
}

static TuiStyle md_base(void) {
    switch (g_md.block) {
        case MD_BLOCK_HEAD:  return TUI_HEADING;
        case MD_BLOCK_QUOTE: return TUI_QUOTE;
        case MD_BLOCK_CODE:  return TUI_CODE;
        default:             return g_md.muted ? TUI_QUOTE : TUI_PLAIN;
    }
}

static void md_drain(b8 flush);

static void md_hl_abandon(void) {
    g_md.hl_active = false;
    g_md.hl_line_open = false;
    g_md.hl_source_n = 0;
    g_md.hl_line_n = 0;
}

static void md_hl_begin(Str alias) {
    md_hl_abandon();
    g_md.hl_alias_n = 0;
    if (!tui_highlight_enabled() || !alias.n || alias.n > YHL_ALIAS_MAX) return;
    for (size_t i = 0; i < alias.n; i++) {
        unsigned char c = (unsigned char)alias.p[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        g_md.hl_alias[i] = (char)c;
    }
    g_md.hl_alias_n = alias.n;
    g_md.hl_epoch = tui_transcript_epoch();
    g_md.hl_active = true;
}

static void md_hl_line_begin(void) {
    if (!g_md.hl_active) return;
    if (g_md.hl_epoch != tui_transcript_epoch() || g_md.hl_line_n == MD_HL_LINES) {
        md_hl_abandon();
        return;
    }
    size_t line = g_md.hl_line_n++;
    g_md.hl_src_start[line] = (u32)g_md.hl_source_n;
    g_md.hl_src_end[line] = (u32)g_md.hl_source_n;
    g_md.hl_dst_start[line] = tui_transcript_pos();
    g_md.hl_line_open = true;
}

static void md_hl_put(Str s) {
    if (!g_md.hl_active || !g_md.hl_line_open || !s.n) return;
    if (s.n > YHL_SOURCE_MAX - g_md.hl_source_n) {
        md_hl_abandon();
        return;
    }
    memcpy(g_md.hl_source + g_md.hl_source_n, s.p, s.n);
    g_md.hl_source_n += s.n;
}

static void md_hl_line_end(b8 newline) {
    if (!g_md.hl_active || !g_md.hl_line_open) return;
    g_md.hl_src_end[g_md.hl_line_n - 1] = (u32)g_md.hl_source_n;
    g_md.hl_line_open = false;
    if (!newline) return;
    if (g_md.hl_source_n == YHL_SOURCE_MAX) {
        md_hl_abandon();
        return;
    }
    g_md.hl_source[g_md.hl_source_n++] = '\n';
}

static size_t md_hl_rendered(size_t a, size_t b) {
    size_t n = 0;
    for (size_t i = a; i < b; i++) {
        unsigned char c = (unsigned char)g_md.hl_source[i];
        if (c == '\t') n += 4;
        else if (c == '\n' || c == '\r' || c < 0x20) continue;
        else n++;
    }
    return n;
}

static void md_hl_finish(void) {
    if (!g_md.hl_active) return;
    md_hl_line_end(false);
    if (g_md.hl_epoch != tui_transcript_epoch()) { md_hl_abandon(); return; }
    static YhlResult result;
    Str hint = { g_md.hl_alias, g_md.hl_alias_n };
    Str source = { g_md.hl_source, g_md.hl_source_n };
    if (highlight_request(YHL_HINT_MARKDOWN_ALIAS, hint, source, &result)) {
        size_t line = 0;
        for (size_t r = 0; r < result.n; r++) {
            size_t start = result.run[r].start;
            size_t end = result.run[r].end;
            while (line < g_md.hl_line_n
                   && g_md.hl_src_end[line] <= start) line++;
            for (size_t i = line; i < g_md.hl_line_n
                 && g_md.hl_src_start[i] < end; i++) {
                size_t a = start > g_md.hl_src_start[i]
                         ? start : g_md.hl_src_start[i];
                size_t b = end < g_md.hl_src_end[i] ? end : g_md.hl_src_end[i];
                if (a >= b) continue;
                size_t dst_a = g_md.hl_dst_start[i]
                             + md_hl_rendered(g_md.hl_src_start[i], a);
                size_t dst_b = dst_a + md_hl_rendered(a, b);
                tui_syntax_add(dst_a, dst_b, result.run[r].semantic);
            }
        }
        tui_syntax_commit();
    }
    md_hl_abandon();
}

static void md_push(char c) {
    if (g_md.pend_n == MD_PEND_MAX) md_drain(true);
    if (g_md.pend_n < MD_PEND_MAX) g_md.pend[g_md.pend_n++] = c;
}

/* ---- inline runs --------------------------------------------------------- */

static char md_prev(size_t i) { return i ? g_md.pend[i - 1] : g_md.last; }

/* One construct starting at `pend[i]`: emitted, still waiting for its closer,
 * or no construct at all, which makes the marker literal. */
static i32 md_run(size_t i, size_t *used) {
    const char *p = g_md.pend;
    size_t n = g_md.pend_n;
    char c = p[i];

    if (c == '\\') {
        if (i + 1 >= n) return MD_SPAN_WAIT;
        if (strchr("\\`*{}[]()#+-.!_|>~<", p[i + 1])) {
            md_emit((Str){ p + i + 1, 1 }, md_base());
            *used = 2;
            return MD_SPAN_DONE;
        }
        return MD_SPAN_NONE;
    }

    if (c == '`') {
        size_t ticks = 1;
        while (i + ticks < n && p[i + ticks] == '`') ticks++;
        if (i + ticks >= n) return MD_SPAN_WAIT;
        for (size_t j = i + ticks; j + ticks <= n; j++) {
            size_t k = 0;
            while (k < ticks && p[j + k] == '`') k++;
            if (k != ticks) continue;
            Str body = { p + i + ticks, j - i - ticks };
            if (body.n >= 2 && body.p[0] == ' '
                && body.p[body.n - 1] == ' ')
                body = (Str){ body.p + 1, body.n - 2 };
            md_emit(body, TUI_MONO);
            *used = j + ticks - i;
            return MD_SPAN_DONE;
        }
        return MD_SPAN_WAIT;
    }

    if (c == '[') {
        size_t j = i + 1;
        while (j < n && p[j] != ']') j++;
        if (j == n) return MD_SPAN_WAIT;
        if (g_md.block == MD_BLOCK_LIST && i == 0 && j == i + 2
            && (p[i + 1] == ' ' || p[i + 1] == 'x' || p[i + 1] == 'X')) {
            md_emit(p[i + 1] == ' ' ? STR("\u2610") : STR("\u2611"),
                    TUI_MARKER);
            *used = 3;
            return MD_SPAN_DONE;
        }
        if (j + 1 >= n) return MD_SPAN_WAIT;
        if (p[j + 1] != '(') return MD_SPAN_NONE;
        size_t k = j + 2;
        while (k < n && p[k] != ')') k++;
        if (k == n) return MD_SPAN_WAIT;
        md_emit((Str){ p + i + 1, j - i - 1 }, TUI_MARKER);
        md_emit(STR(" ("), TUI_QUOTE);
        md_emit((Str){ p + j + 2, k - j - 2 }, TUI_QUOTE);
        md_emit(STR(")"), TUI_QUOTE);
        *used = k - i + 1;
        return MD_SPAN_DONE;
    }

    if (c == '!') {
        if (i + 1 >= n) return MD_SPAN_WAIT;
        if (p[i + 1] != '[') return MD_SPAN_NONE;
        size_t j = i + 2;
        while (j < n && p[j] != ']') j++;
        if (j + 1 >= n) return MD_SPAN_WAIT;
        if (p[j + 1] != '(') return MD_SPAN_NONE;
        size_t k = j + 2;
        while (k < n && p[k] != ')') k++;
        if (k == n) return MD_SPAN_WAIT;
        md_emit((Str){ p + i + 2, j - i - 2 }, TUI_MARKER);
        md_emit(STR(" (image: "), TUI_QUOTE);
        md_emit((Str){ p + j + 2, k - j - 2 }, TUI_QUOTE);
        md_emit(STR(")"), TUI_QUOTE);
        *used = k - i + 1;
        return MD_SPAN_DONE;
    }

    if (c == '<') {
        size_t j = i + 1;
        while (j < n && p[j] != '>') j++;
        if (j == n) return MD_SPAN_WAIT;
        Str target = { p + i + 1, j - i - 1 };
        b8 email = memchr(target.p, '@', target.n) != NULL;
        if (!email && !str_starts(target, STR("http://"))
            && !str_starts(target, STR("https://"))
            && !str_starts(target, STR("mailto:")))
            return MD_SPAN_NONE;
        md_emit(target, TUI_MARKER);
        *used = j - i + 1;
        return MD_SPAN_DONE;
    }

    if (c == '~') {
        if (i + 1 >= n) return MD_SPAN_WAIT;
        if (p[i + 1] != '~') return MD_SPAN_NONE;
        if (i + 2 >= n) return MD_SPAN_WAIT;
        for (size_t j = i + 2; j + 1 < n; j++) {
            if (p[j] != '~' || p[j + 1] != '~') continue;
            if (j == i + 2) return MD_SPAN_NONE;
            md_emit((Str){ p + i + 2, j - i - 2 }, TUI_STRIKE);
            *used = j - i + 2;
            return MD_SPAN_DONE;
        }
        return MD_SPAN_WAIT;
    }

    /* Doubled is strong, single is emphasis. An opener is followed by a
     * non-space, which keeps "5 * 3" arithmetic, and an underscore also needs
     * a boundary before it, which keeps snake_case a name. */
    size_t run = 1;
    if (i + 1 < n && p[i + 1] == c) run = 2;
    if (i + run >= n) return MD_SPAN_WAIT;
    if (md_space(p[i + run])) return MD_SPAN_NONE;
    if (c == '_' && md_word(md_prev(i))) return MD_SPAN_NONE;
    for (size_t j = i + run + 1; j + run <= n; j++) {
        if (p[j] != c) continue;
        if (run == 2 && p[j + 1] != c) continue;
        if (md_space(p[j - 1])) continue;
        if (c == '_' && j + run < n && md_word(p[j + run])) continue;
        md_emit((Str){ p + i + run, j - i - run },
                run == 2 ? TUI_BOLD : TUI_EMPH);
        *used = j + run - i;
        return MD_SPAN_DONE;
    }
    return MD_SPAN_WAIT;
}

/* Emit as much of the pending bytes as have a decided style. `flush` ends the
 * line, so whatever is still open was never markup and reads literally. */
static void md_drain(b8 flush) {
    if (g_md.block == MD_BLOCK_CODE) {
        md_hl_put((Str){ g_md.pend, g_md.pend_n });
        md_emit((Str){ g_md.pend, g_md.pend_n }, TUI_CODE);
        if (g_md.pend_n) g_md.last = g_md.pend[g_md.pend_n - 1];
        g_md.pend_n = 0;
        return;
    }
    size_t i = 0;
    while (i < g_md.pend_n) {
        if (md_marker(g_md.pend[i])) {
            size_t used = 0;
            i32 state = md_run(i, &used);
            if (state == MD_SPAN_DONE) { i += used; continue; }
            if (state == MD_SPAN_WAIT && !flush) break;
            md_emit((Str){ g_md.pend + i, 1 }, md_base());
            i++;
            continue;
        }
        size_t k = i;
        while (k < g_md.pend_n && !md_marker(g_md.pend[k])) k++;
        md_emit((Str){ g_md.pend + i, k - i }, md_base());
        i = k;
    }
    if (!i) return;
    g_md.last = g_md.pend[i - 1];
    memmove(g_md.pend, g_md.pend + i, g_md.pend_n - i);
    g_md.pend_n -= i;
}

/* ---- block markers ------------------------------------------------------- */

/* Whether the buffered bytes can still grow into a block marker. While they
 * can, the line's shape is undecided and nothing has been painted. */
static b8 md_candidate(Str s) {
    size_t i = 0;
    while (i < s.n && s.p[i] == ' ') i++;
    if (i > 8) return false;
    Str m = str_drop(s, i);
    if (!m.n) return true;
    char c = m.p[0];
    if (g_md.fence)
        return c == g_md.fence_mark && md_all(m, c) && m.n <= 3;
    if (c == '`' || c == '~') return md_all(m, c) && m.n <= 3;
    if (c == '#') return md_all(m, '#') && m.n <= 6;
    if (c == '>') return md_all(m, '>') && m.n <= 8;
    if (c == '-' || c == '*' || c == '+' || c == '_') return md_all(m, c);
    if (c >= '0' && c <= '9') {
        size_t k = 0;
        while (k < m.n && m.p[k] >= '0' && m.p[k] <= '9') k++;
        if (k > 9) return false;
        return k == m.n || (k + 1 == m.n && (m.p[k] == '.' || m.p[k] == ')'));
    }
    return false;
}

static b8 md_ordered(Str m) {
    if (m.n < 2) return false;
    size_t k = 0;
    while (k < m.n && m.p[k] >= '0' && m.p[k] <= '9') k++;
    return k && k + 1 == m.n && (m.p[k] == '.' || m.p[k] == ')');
}

static void md_indent(size_t n) {
    static const char spaces[] = "                        ";
    if (n > sizeof spaces - 1) n = sizeof spaces - 1;
    if (n) tui_write((Str){ spaces, n });
}

static void md_rule(void) {
    char buf[MD_RULE_MAX * 3];
    size_t cells = tui_body_cols();
    if (!cells || cells > MD_RULE_MAX) cells = MD_RULE_MAX;
    for (size_t i = 0; i < cells; i++) memcpy(buf + i * 3, "\u2500", 3);
    md_emit((Str){ buf, cells * 3 }, TUI_MARKER);
}

/* Settle the line's shape and paint whatever stands in for the marker. `next`
 * is the byte after the buffered marker, 0 at the end of the line. */
static void md_resolve(char next) {
    Str p = { g_md.prefix, g_md.prefix_n };
    size_t indent = 0;
    while (indent < p.n && p.p[indent] == ' ') indent++;
    Str m = str_drop(p, indent);
    g_md.in_body = true;
    g_md.block = MD_BLOCK_PLAIN;

    if (g_md.fence) {
        g_md.block = MD_BLOCK_CODE;
        md_emit(STR("  "), TUI_CODE);
        md_hl_line_begin();
        md_hl_put(p);
        md_emit(p, TUI_CODE);
        if (next) md_push(next);
        return;
    }
    b8 space = next == ' ';
    b8 eol = next == 0;
    if (m.n <= 6 && md_all(m, '#') && (space || eol)) {
        g_md.block = MD_BLOCK_HEAD;
        return;
    }
    if (md_all(m, '>') && (space || eol)) {
        g_md.block = MD_BLOCK_QUOTE;
        md_indent(indent);
        for (size_t i = 0; i < m.n; i++)
            md_emit(STR("\u2502 "), TUI_MARKER);
        return;
    }
    if (m.n == 1 && space && (m.p[0] == '-' || m.p[0] == '*' || m.p[0] == '+')) {
        g_md.block = MD_BLOCK_LIST;
        md_indent(indent);
        md_emit(STR("\u2022 "), TUI_MARKER);
        return;
    }
    if (md_ordered(m) && space) {
        g_md.block = MD_BLOCK_LIST;
        md_indent(indent);
        md_emit(m, TUI_MARKER);
        md_emit(STR(" "), TUI_MARKER);
        return;
    }
    if (eol && m.n >= 3
        && (md_all(m, '-') || md_all(m, '*') || md_all(m, '_'))) {
        md_rule();
        return;
    }
    for (size_t i = 0; i < p.n; i++) md_push(p.p[i]);
    if (next) md_push(next);
}

static void md_prefix_byte(char c) {
    if (g_md.prefix_n < MD_PREFIX_MAX) {
        g_md.prefix[g_md.prefix_n] = c;
        if (md_candidate((Str){ g_md.prefix, g_md.prefix_n + 1 })) {
            g_md.prefix_n++;
            Str m = { g_md.prefix, g_md.prefix_n };
            size_t i = 0;
            while (i < m.n && m.p[i] == ' ') i++;
            Str fence = str_drop(m, i);
            if ((md_all(fence, '`') || md_all(fence, '~'))
                && m.n - i == 3) {
                if (g_md.fence) {
                    g_md.fence = false;
                    g_md.fence_mark = 0;
                } else {
                    g_md.fence = true;
                    g_md.fence_mark = fence.p[0];
                }
                g_md.in_body = true;
                g_md.skip_line = true;
            }
            return;
        }
    }
    md_resolve(c);
}

static void md_line_reset(void) {
    g_md.in_body = false;
    g_md.skip_line = false;
    g_md.prefix_n = 0;
    g_md.block = MD_BLOCK_PLAIN;
    g_md.last = 0;
}

static void md_line_end(void) {
    if (!g_md.in_body) md_resolve(0);
    md_drain(true);
    md_hl_line_end(g_md.fence);
    tui_write(STR("\n"));
    md_line_reset();
}

static void md_low_write(Str delta) {
    if (g_md.raw || !tui_is_fullscreen()) { tui_write(delta); return; }
    for (size_t i = 0; i < delta.n; i++) {
        char c = delta.p[i];
        if (c == '\r') continue;
        if (c == '\n') { md_line_end(); continue; }
        if (!g_md.in_body) { md_prefix_byte(c); continue; }
        if (!g_md.skip_line) md_push(c);
    }
    md_drain(false);
}

static void md_low_end(void) {
    if (g_md.raw || !tui_is_fullscreen()) return;
    if (!g_md.in_body) md_resolve(0);
    md_drain(true);
    md_line_reset();
    g_md.fence = false;
    g_md.fence_mark = 0;
}

/* ---- line-aware extensions ---------------------------------------------
 * A table is the one common shape that cannot be identified from its first
 * row alone: the delimiter below it is the proof. Short lines wait here for
 * that decision. A line too long to be useful as a terminal table falls
 * through to the streaming renderer as soon as this bounded buffer fills. */

static size_t md_cells(Str row, Str *out, size_t cap) {
    row = str_trim(row);
    if (!row.n) return 0;
    b8 have_pipe = row.p[0] == '|' || row.p[row.n - 1] == '|';
    if (row.p[0] == '|') row = str_drop(row, 1);
    if (row.n && row.p[row.n - 1] == '|') row.n--;
    size_t n = 0, start = 0;
    b8 escaped = false, code = false;
    for (size_t i = 0; i <= row.n; i++) {
        char c = i < row.n ? row.p[i] : '|';
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '`') { code = !code; continue; }
        if (c != '|' || code) continue;
        if (i < row.n) have_pipe = true;
        if (n < cap) out[n] = str_trim((Str){ row.p + start, i - start });
        n++;
        start = i + 1;
    }
    return have_pipe ? n : 0;
}

static b8 md_delimiter(Str line, size_t *cols, u8 *align) {
    Str cells[MD_TABLE_COLS];
    size_t n = md_cells(line, cells, MD_TABLE_COLS);
    if (!n || n > MD_TABLE_COLS) return false;
    for (size_t i = 0; i < n; i++) {
        Str s = cells[i];
        b8 left = s.n && s.p[0] == ':';
        if (left) s = str_drop(s, 1);
        b8 right = s.n && s.p[s.n - 1] == ':';
        if (right) s.n--;
        if (s.n < 3 || !md_all(s, '-')) return false;
        align[i] = left && right ? 1u : right ? 2u : 0u;
    }
    *cols = n;
    return true;
}

static b8 md_table_row(Str line) {
    Str cells[MD_TABLE_COLS];
    return md_cells(line, cells, MD_TABLE_COLS) != 0;
}

static b8 md_table_add(Str line) {
    if (g_md.table_n == MD_TABLE_ROWS
        || line.n > MD_TABLE_BYTES - g_md.table_bytes)
        return false;
    size_t at = g_md.table_bytes;
    if (line.n) memcpy(g_md.table + at, line.p, line.n);
    g_md.table_off[g_md.table_n] = at;
    g_md.table_len[g_md.table_n] = line.n;
    g_md.table_n++;
    g_md.table_bytes += line.n;
    return true;
}

/* ---- resolved cells ------------------------------------------------------
 * Capture points the inline renderer at a cell rather than at the transcript,
 * so a cell is measured and wrapped on the text it draws and markup never
 * lands on the screen or spends columns. */

static void md_cell_put(MdCell *c, Str s, TuiStyle style) {
    if (c->over || !s.n) return;
    if (s.n > MD_CELL_MAX - c->n) { c->over = true; return; }
    memcpy(c->buf + c->n, s.p, s.n);
    c->n += s.n;
    if (c->runs && c->run_style[c->runs - 1] == (u8)style) {
        c->run_end[c->runs - 1] = c->n;
        return;
    }
    if (c->runs == MD_CELL_RUNS) { c->over = true; return; }
    c->run_style[c->runs] = (u8)style;
    c->run_end[c->runs++] = c->n;
}

/* Resolve `src` into `c`. The streaming state the inline renderer keeps is
 * saved across this, so a table drawn mid-message leaves it untouched. */
static void md_cell_build(MdCell *c, Str src) {
    c->n = 0;
    c->runs = 0;
    c->over = false;
    char pend[MD_PEND_MAX];
    size_t pend_n = g_md.pend_n;
    i32 block = g_md.block;
    char last = g_md.last;
    if (pend_n) memcpy(pend, g_md.pend, pend_n);
    g_md.pend_n = 0;
    g_md.block = MD_BLOCK_PLAIN;
    g_md.last = 0;
    g_md.cap = c;
    for (size_t i = 0; i < src.n && !c->over; i++) {
        if (g_md.pend_n == MD_PEND_MAX) md_drain(true);
        g_md.pend[g_md.pend_n++] = src.p[i];
    }
    if (!c->over) md_drain(true);
    g_md.cap = NULL;
    g_md.pend_n = pend_n;
    if (pend_n) memcpy(g_md.pend, pend, pend_n);
    g_md.block = block;
    g_md.last = last;
    if (c->over) c->runs = 0;
    c->text = c->over ? src : (Str){ c->buf, c->n };
}

static TuiStyle md_cell_base(b8 head) {
    return head ? TUI_BOLD : g_md.muted ? TUI_QUOTE : TUI_PLAIN;
}

/* Draw `[a, b)` of a resolved cell, run by run. A header row bolds whatever
 * the runs left at the base style and keeps the rest as it resolved. */
static void md_cell_emit(const MdCell *c, size_t a, size_t b, b8 head) {
    size_t run = 0;
    while (a < b) {
        while (run < c->runs && c->run_end[run] <= a) run++;
        size_t end = run < c->runs && c->run_end[run] < b ? c->run_end[run] : b;
        TuiStyle style = md_cell_base(head);
        if (run < c->runs) {
            TuiStyle got = (TuiStyle)c->run_style[run];
            if (!head || (got != TUI_PLAIN && got != TUI_QUOTE)) style = got;
        }
        md_emit((Str){ c->text.p + a, end - a }, style);
        a = end;
    }
}

static void md_repeat(const char *glyph, size_t bytes, size_t n) {
    for (size_t i = 0; i < n; i++)
        md_emit((Str){ glyph, bytes }, TUI_MARKER);
}

static void md_table_border(const char *left, const char *middle,
                            const char *right, const size_t *width) {
    md_emit((Str){ left, 3 }, TUI_MARKER);
    for (size_t i = 0; i < g_md.table_cols; i++) {
        md_repeat("\u2500", 3, width[i] + 2);
        md_emit((Str){ i + 1 == g_md.table_cols ? right : middle, 3 },
                TUI_MARKER);
    }
    tui_write(STR("\n"));
}

static void md_spaces(size_t n, TuiStyle style) {
    static const char spaces[] =
        "                                                                ";
    while (n) {
        size_t take = n < sizeof spaces - 1 ? n : sizeof spaces - 1;
        md_emit((Str){ spaces, take }, style);
        n -= take;
    }
}

/* The next visual line of a cell: the bytes to draw, with `advance` bytes
 * consumed including the whitespace the break eats. Words are kept whole
 * where one fits, and a word wider than its column is split between
 * glyphs, which is the only way a narrow column can show it at all. */
static Str md_wrap_segment(Str s, size_t width, size_t *advance) {
    size_t fit = tui_text_fit(s, width, NULL);
    if (fit == s.n) { *advance = s.n; return s; }
    size_t brk = fit;
    if (!md_space(s.p[fit])) {
        size_t last = 0;   /* the last space that fits, 0 for none */
        for (size_t i = 1; i < fit; i++) if (md_space(s.p[i])) last = i;
        if (last) brk = last;
    }
    if (!brk) {            /* one glyph is already wider than the column */
        brk = 1;
        while (brk < s.n && ((u8)s.p[brk] & 0xc0u) == 0x80u) brk++;
    }
    size_t adv = brk;
    while (adv < s.n && md_space(s.p[adv])) adv++;
    *advance = adv;
    Str seg = { s.p, brk };
    while (seg.n && md_space(seg.p[seg.n - 1])) seg.n--;
    return seg;
}

/* One row, over as many visual lines as its widest cell wraps onto. */
static void md_table_line(Str line, const size_t *width, b8 head) {
    Str cells[MD_TABLE_COLS];
    size_t n = md_cells(line, cells, MD_TABLE_COLS);
    size_t at[MD_TABLE_COLS] = {0};
    for (size_t i = 0; i < g_md.table_cols; i++)
        md_cell_build(&g_md.cell[i], i < n ? cells[i] : (Str){0});
    TuiStyle style = md_cell_base(head);
    b8 more;
    do {
        more = false;
        md_emit(STR("\u2502"), TUI_MARKER);
        for (size_t i = 0; i < g_md.table_cols; i++) {
            const MdCell *c = &g_md.cell[i];
            size_t start = at[i], took = 0;
            if (start < c->text.n) {
                size_t advance = 0;
                Str seg = md_wrap_segment(str_drop(c->text, start),
                                          width[i], &advance);
                took = seg.n;
                at[i] = start + advance;
                if (at[i] < c->text.n) more = true;
            }
            size_t used = tui_text_cells((Str){ c->text.p + start, took });
            if (used > width[i]) used = width[i];
            size_t pad = width[i] - used;
            size_t left = g_md.table_align[i] == 2 ? pad
                        : g_md.table_align[i] == 1 ? pad / 2 : 0;
            md_spaces(left + 1, style);
            md_cell_emit(c, start, start + took, head);
            md_spaces(pad - left + 1, style);
            md_emit(STR("\u2502"), TUI_MARKER);
        }
        tui_write(STR("\n"));
    } while (more);
}

/* Columns are laid out the way a browser lays a table out: natural widths
 * while the row fits the transcript, then the widest columns give way one
 * cell at a time until it does and their text wraps. A terminal too narrow
 * to hold a cell per column keeps the natural widths, since a table drawn
 * one glyph wide says less than the wrapped rows do. */
static void md_table_fit(size_t *width, size_t cols) {
    size_t body = tui_body_cols();
    size_t frame = cols * 3 + 1;   /* "| " before every cell, "|" at the end */
    if (!body || cols > MD_TABLE_COLS || body < frame + cols) return;
    size_t budget = body - frame, total = 0, widest = 0;
    for (size_t i = 0; i < cols; i++) {
        total += width[i];
        if (width[i] > widest) widest = width[i];
    }
    if (total <= budget) return;
    /* The largest cap every column can be clipped to and still fit. */
    size_t lo = 1, hi = widest;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2, sum = 0;
        for (size_t i = 0; i < cols; i++)
            sum += width[i] < mid ? width[i] : mid;
        if (sum <= budget) lo = mid; else hi = mid - 1;
    }
    b8 clipped[MD_TABLE_COLS] = {0};
    size_t used = 0;
    for (size_t i = 0; i < cols; i++) {
        clipped[i] = width[i] > lo;
        if (clipped[i]) width[i] = lo;
        used += width[i];
    }
    /* The cap divides unevenly, so the cells it left over go to the clipped
     * columns and the row reaches the edge. */
    for (size_t i = 0; i < cols && used < budget; i++)
        if (clipped[i]) { width[i]++; used++; }
}

static void md_table_flush(void) {
    if (!g_md.table_n) return;
    size_t width[MD_TABLE_COLS] = {0};
    for (size_t r = 0; r < g_md.table_n; r++) {
        Str cells[MD_TABLE_COLS];
        Str line = { g_md.table + g_md.table_off[r], g_md.table_len[r] };
        size_t n = md_cells(line, cells, MD_TABLE_COLS);
        if (n > g_md.table_cols) n = g_md.table_cols;
        for (size_t i = 0; i < n; i++) {
            md_cell_build(&g_md.measured, cells[i]);
            size_t w = tui_text_cells(g_md.measured.text);
            if (w > width[i]) width[i] = w;
        }
    }
    for (size_t i = 0; i < g_md.table_cols; i++)
        if (!width[i]) width[i] = 1;
    md_table_fit(width, g_md.table_cols);
    md_table_border("\u250c", "\u252c", "\u2510", width);
    md_table_line((Str){ g_md.table + g_md.table_off[0],
                         g_md.table_len[0] }, width, true);
    md_table_border("\u251c", "\u253c", "\u2524", width);
    for (size_t r = 1; r < g_md.table_n; r++)
        md_table_line((Str){ g_md.table + g_md.table_off[r],
                             g_md.table_len[r] }, width, false);
    md_table_border("\u2514", "\u2534", "\u2518", width);
    g_md.table_n = 0;
    g_md.table_bytes = 0;
    g_md.table_cols = 0;
}

static Str md_trim_atx_closer(Str line) {
    if (g_md.fence) return line;
    size_t i = 0;
    while (i < line.n && line.p[i] == ' ') i++;
    if (i > 3 || i == line.n || line.p[i] != '#') return line;
    size_t hashes = i;
    while (hashes < line.n && line.p[hashes] == '#') hashes++;
    if (hashes - i > 6 || (hashes < line.n && line.p[hashes] != ' '))
        return line;
    size_t end = line.n;
    while (end && md_space(line.p[end - 1])) end--;
    size_t closer = end;
    while (closer > hashes && line.p[closer - 1] == '#') closer--;
    if (closer == end || closer == 0 || !md_space(line.p[closer - 1]))
        return line;
    while (closer && md_space(line.p[closer - 1])) closer--;
    line.n = closer;
    return line;
}

static void md_regular(Str line, b8 eol) {
    line = md_trim_atx_closer(line);
    md_low_write(line);
    if (eol) md_low_write(STR("\n"));
}

static void md_held_flush(void) {
    if (!g_md.held_n) return;
    md_regular((Str){ g_md.held, g_md.held_n }, g_md.held_eol);
    g_md.held_n = 0;
    g_md.held_eol = false;
}

static void md_complete_line(Str line, b8 eol) {
    if (g_md.fence) {
        size_t i = 0;
        while (i < line.n && line.p[i] == ' ') i++;
        b8 closes = i <= 8 && line.n - i >= 3
                 && line.p[i] == g_md.fence_mark
                 && line.p[i + 1] == g_md.fence_mark
                 && line.p[i + 2] == g_md.fence_mark
                 && (line.n == i + 3 || line.p[i + 3] != g_md.fence_mark);
        md_regular(line, eol);
        if (closes) md_hl_finish();
        return;
    }
    size_t fence_i = 0;
    while (fence_i < line.n && line.p[fence_i] == ' ') fence_i++;
    if (fence_i <= 8 && line.n - fence_i >= 3
        && (line.p[fence_i] == '`' || line.p[fence_i] == '~')
        && line.p[fence_i + 1] == line.p[fence_i]
        && line.p[fence_i + 2] == line.p[fence_i]
        && (line.n == fence_i + 3 || line.p[fence_i + 3] != line.p[fence_i])) {
        size_t info = fence_i + 3;
        while (info < line.n && md_space(line.p[info])) info++;
        size_t end = info;
        while (end < line.n && !md_space(line.p[end])) end++;
        md_hl_begin((Str){ line.p + info, end - info });
    }
    if (g_md.table_n) {
        if (md_table_row(line) && md_table_add(line)) return;
        md_table_flush();
    }
    if (g_md.held_n) {
        size_t cols = 0;
        u8 align[MD_TABLE_COLS] = {0};
        Str header_cells[MD_TABLE_COLS];
        size_t header_cols = md_cells((Str){ g_md.held, g_md.held_n },
                                      header_cells, MD_TABLE_COLS);
        if (md_delimiter(line, &cols, align) && cols == header_cols) {
            g_md.table_cols = cols;
            memcpy(g_md.table_align, align, cols * sizeof align[0]);
            if (md_table_add((Str){ g_md.held, g_md.held_n })) {
                g_md.held_n = 0;
                g_md.held_eol = false;
                return;
            }
            g_md.table_cols = 0;
        }
        md_held_flush();
    }
    if (md_table_row(line) && line.n < sizeof g_md.held) {
        memcpy(g_md.held, line.p, line.n);
        g_md.held_n = line.n;
        g_md.held_eol = eol;
        return;
    }
    md_regular(line, eol);
}

void md_write(Str delta) {
    if (g_md.raw || !tui_is_fullscreen()) {
        if (g_md.muted && tui_is_fullscreen()) tui_write_muted(delta);
        else tui_write(delta);
        return;
    }
    for (size_t i = 0; i < delta.n; i++) {
        char c = delta.p[i];
        if (c == '\r') continue;
        if (g_md.line_long) {
            if (c == '\n') {
                md_line_end();
                g_md.line_long = false;
                continue;
            }
            /* A line past MD_LINE_MAX is written through as it arrives, so
             * it is handed over a run at a time: md_low_write drains to the
             * transcript once per call, and a byte per call turns a long
             * line, a minified blob or a wide log record into one drain per
             * character. The run stops at the bytes the loop above treats
             * specially, which keeps them on their existing paths. */
            size_t run = i;
            while (run < delta.n && delta.p[run] != '\n' && delta.p[run] != '\r')
                run++;
            md_low_write((Str){ delta.p + i, run - i });
            i = run - 1;   /* the loop's ++ lands on the stopper or the end */
            continue;
        }
        if (c == '\n') {
            md_complete_line((Str){ g_md.line, g_md.line_n }, true);
            g_md.line_n = 0;
        } else if (g_md.line_n < sizeof g_md.line) {
            g_md.line[g_md.line_n++] = c;
        } else {
            md_table_flush();
            md_held_flush();
            md_low_write((Str){ g_md.line, g_md.line_n });
            g_md.line_n = 0;
            g_md.line_long = true;
            md_low_write((Str){ delta.p + i, 1 });
        }
    }
}

void md_end(void) {
    if (g_md.raw || !tui_is_fullscreen()) return;
    if (g_md.line_long) {
        md_low_end();
        g_md.line_long = false;
    } else if (g_md.line_n) {
        md_complete_line((Str){ g_md.line, g_md.line_n }, false);
    }
    g_md.line_n = 0;
    md_table_flush();
    md_held_flush();
    if (g_md.fence) md_hl_finish();
    md_low_end();
}

void md_set_muted(b8 on) {
    if (g_md.muted == on) return;
    md_end();
    g_md.muted = on;
}

void md_set_raw(b8 on) {
    md_end();
    g_md.raw = on;
}

b8 md_raw(void) { return g_md.raw; }
