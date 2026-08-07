/* markdown.c: renders a reply's Markdown into the transcript.
 *
 * Rendering is streaming, because a reply is read as it arrives: bytes are
 * painted as soon as their shape is known and only an unresolved marker
 * waits. Two things are buffered, both bounded by the line they sit in: the
 * bytes that may still turn out to be a block marker (a heading's hashes, a
 * fence, a list bullet), and the bytes from an opener whose closer has not
 * arrived yet, since "*fast" is only emphasis once the second star lands.
 */
#include "yoke.h"

#include <string.h>

/* A block marker plus its indent; anything longer is prose. */
#define MD_PREFIX_MAX 24
/* An unresolved emphasis run. Past it the opener is taken literally, which
 * bounds how far a stray '*' can hold the stream back. */
#define MD_PEND_MAX 512
/* Widest horizontal rule drawn, so a wide terminal does not get a rule that
 * reads as a wall. */
#define MD_RULE_MAX 60

enum { MD_BLOCK_PLAIN, MD_BLOCK_HEAD, MD_BLOCK_QUOTE, MD_BLOCK_CODE };
enum { MD_SPAN_NONE, MD_SPAN_WAIT, MD_SPAN_DONE };

static struct {
    b8     raw;
    b8     fence;      /* inside a fenced code block */
    b8     in_body;    /* the line's block marker has been resolved */
    b8     skip_line;  /* the rest of the line belongs to a marker (info string) */
    i32    block;
    char   prefix[MD_PREFIX_MAX];
    size_t prefix_n;
    char   pend[MD_PEND_MAX];
    size_t pend_n;
    char   last;       /* last byte consumed, for the flanking rules */
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
    return c == '*' || c == '_' || c == '`' || c == '[';
}

static void md_emit(Str s, TuiStyle style) {
    if (s.n) tui_write_styled(s, style);
}

static TuiStyle md_base(void) {
    switch (g_md.block) {
        case MD_BLOCK_HEAD:  return TUI_HEADING;
        case MD_BLOCK_QUOTE: return TUI_QUOTE;
        case MD_BLOCK_CODE:  return TUI_CODE;
        default:             return TUI_PLAIN;
    }
}

static void md_drain(b8 flush);

static void md_push(char c) {
    if (g_md.pend_n == MD_PEND_MAX) md_drain(true);
    if (g_md.pend_n < MD_PEND_MAX) g_md.pend[g_md.pend_n++] = c;
}

/* ---- inline runs --------------------------------------------------------- */

static char md_prev(size_t i) { return i ? g_md.pend[i - 1] : g_md.last; }

/* One construct starting at `pend[i]`: emitted (DONE), still waiting for its
 * closer (WAIT), or no construct at all (NONE, so the marker is literal). */
static i32 md_run(size_t i, size_t *used) {
    const char *p = g_md.pend;
    size_t n = g_md.pend_n;
    char c = p[i];

    if (c == '`') {
        for (size_t j = i + 1; j < n; j++) {
            if (p[j] != '`') continue;
            if (j == i + 1) return MD_SPAN_NONE;
            md_emit((Str){ p + i + 1, j - i - 1 }, TUI_MONO);
            *used = j - i + 1;
            return MD_SPAN_DONE;
        }
        return MD_SPAN_WAIT;
    }

    if (c == '[') {
        size_t j = i + 1;
        while (j < n && p[j] != ']') j++;
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

    /* '*' and '_': doubled is strong, single is emphasis. An opener is
     * followed by a non-space, which is what keeps "5 * 3" arithmetic, and an
     * underscore also needs a boundary before it, which is what keeps
     * snake_case a name. */
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

/* Emit as much of the pending bytes as their style is decided. `flush` ends
 * the line: whatever is still open was never markup and reads literally. */
static void md_drain(b8 flush) {
    if (g_md.block == MD_BLOCK_CODE) {
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

/* Can the bytes buffered so far still grow into a block marker? While they
 * can, the line's shape is undecided and nothing has been painted. */
static b8 md_candidate(Str s) {
    size_t i = 0;
    while (i < s.n && s.p[i] == ' ') i++;
    if (i > 8) return false;
    Str m = str_drop(s, i);
    if (!m.n) return true;
    char c = m.p[0];
    if (c == '`') return md_all(m, '`') && m.n <= 3;
    if (g_md.fence) return false;   /* inside a fence only its closer matters */
    if (c == '#') return md_all(m, '#') && m.n <= 6;
    if (c == '>') return m.n == 1;
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

/* Settle the line's shape, `next` being the byte after the buffered marker (0
 * at the end of the line), and paint whatever stands in for the marker. */
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
    if (m.n == 1 && m.p[0] == '>' && (space || eol)) {
        g_md.block = MD_BLOCK_QUOTE;
        md_indent(indent);
        md_emit(STR("\u2502 "), TUI_MARKER);
        return;
    }
    if (m.n == 1 && space && (m.p[0] == '-' || m.p[0] == '*' || m.p[0] == '+')) {
        md_indent(indent);
        md_emit(STR("\u2022 "), TUI_MARKER);
        return;
    }
    if (md_ordered(m) && space) {
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
            if (md_all(str_drop(m, i), '`') && m.n - i == 3) {
                /* The fence itself, and its info string, are not content. */
                g_md.fence = !g_md.fence;
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
    tui_write(STR("\n"));
    md_line_reset();
}

void md_write(Str delta) {
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

void md_end(void) {
    if (g_md.raw || !tui_is_fullscreen()) return;
    if (!g_md.in_body) md_resolve(0);
    md_drain(true);
    md_line_reset();
    g_md.fence = false;
}

void md_set_raw(b8 on) {
    md_end();
    g_md.raw = on;
}

b8 md_raw(void) { return g_md.raw; }
