/* tui.c: dependency-free fullscreen terminal UI.
 *
 * The tty path owns the alternate screen and repaints from an in-memory
 * transcript, so redraw, Ctrl-L and resize are deterministic and provider
 * output never writes through the UI and corrupts the composer. Pipes keep
 * the conventional line-oriented behaviour.
 */
#include "yoke.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#define TUI_TRANSCRIPT_CAP (8u << 20)
#define TUI_MAX_ROWS 4096
#define TUI_BODY_GUTTER 2
#define TUI_OUT_CAP (1u << 16)   /* one frame's escapes, written in one go */
#define TUI_SEL_ROWS 512         /* screen rows mirrored for selection      */
#define TUI_SEL_ROW_BYTES 2048   /* visible bytes kept per screen row       */
#define TUI_SEL_BYTES (1u << 16) /* clipboard payload cap                   */
#define TUI_POPUP_ROWS 8         /* completion entries shown at once         */
#define TUI_PICK_SEARCH_MIN 10   /* entries above which a picker searches    */
#define TUI_PATH_ENTS 256        /* paths the '@' popup keeps for one word    */
#define TUI_PATH_SLOT 512        /* longest path one of them holds            */
#define TUI_PATH_DEPTH 12        /* directories a fuzzy match descends through */
#define TUI_PATH_SCAN 20000      /* entries one keystroke's walk may look at  */
#define TUI_IGNORE_PATS 512      /* ignore patterns in force for one listing  */
#define TUI_IGNORE_BUF (1u << 14)
#define TUI_PICK_QUERY 64        /* longest search a picker accepts          */
#define TUI_ASK_MAX 1024         /* longest answer tui_ask accepts           */
/* Markdown claims one span per emphasis run, so this counts words of a reply
 * rather than messages. */
#define TUI_MAX_SPANS 4096
#define TUI_CKPTS 4096           /* wrapped-row checkpoints into the transcript */
#define TUI_MAX_ZONES 512        /* clickable transcript ranges kept          */

/* Styling is optional: NO_COLOR and dumb terminals get the same layout
 * undecorated. */
#define S_RESET       "\033[0m"
#define S_PANEL_BG    "\033[48;5;236m"
#define S_TEXT        "\033[38;5;253m"
#define S_MUTED       "\033[38;5;245m"
#define S_CYAN        "\033[1;38;5;81m"
#define S_BLUE        "\033[1;38;5;75m"
#define S_GREEN       "\033[1;38;5;114m"
#define S_YELLOW      "\033[1;38;5;221m"
#define S_RED         "\033[1;38;5;203m"
#define S_PURPLE      "\033[1;38;5;177m"
#define S_BOLD        "\033[1m"
#define S_ITALIC      "\033[3m"
#define S_MONO        "\033[38;5;180m"
#define S_USER_BG     "\033[48;5;238m"
#define S_CODE_BG     "\033[48;5;235m"
#define S_POPUP_BG    "\033[48;5;237m"
#define S_POPUP_SEL   "\033[48;5;24m"
/* A clickable row reads like a link, and brightens under the pointer. */
#define S_LINK        "\033[4;38;5;81m"
#define S_LINK_HOVER  "\033[1;4;38;5;81m"

typedef struct {
    struct termios original_termios;
    struct sigaction original_winch;
    b8 raw;
    b8 tty;
    b8 fullscreen;
    b8 editing;
    b8 busy;          /* a turn is running: edits allowed, submit is not */
    b8 input_eof;     /* terminal went away: stop polling a dead fd       */
    b8 color;
    /* Between a bracketed paste's start and end markers every byte is text:
     * a newline in it is a line break in the draft rather than a submit. */
    b8 pasting;
    b8 paste_cr;      /* a CR just seen, so a CRLF pair costs one break     */
    Str model;
    Str provider;
    AgentMode mode;   /* named on the status line; Shift+Tab switches it */
    Str base_url;     /* what provider is derived from when no name is set */
    /* A modal question owns the composer. Its answer is not a message, so it
     * reaches neither the transcript nor the prompt history. */
    b8 ask;
    b8 ask_secret;
    char cwd_buf[4096];
    Str cwd;
    size_t context_tokens;
    b8 context_known;
    char status[32];
    char transcript[TUI_TRANSCRIPT_CAP];
    size_t transcript_n;
    /* Newlines written but not committed. The transcript never ends with one,
     * so the air between two blocks is decided in tui_block alone instead of
     * being split between the block that ended and the one that starts. */
    size_t pend_nl;
    size_t trail_nl;   /* newlines already committed after the last content */
    b8 wrote_any;      /* content reached stdout; only the plain-output path */
    size_t scroll_rows;
    /* Wrapped-row index over the transcript: a frame needs the total row
     * count and the byte offset of the first visible row, and deriving both
     * by walking the scrollback costs the size of the session. The scan is
     * incremental and checkpoints start the painter near the visible rows. */
    size_t wrap_cols;        /* width the index was built for; 0 = invalid  */
    size_t wrap_scanned;     /* transcript bytes already accounted for      */
    size_t wrap_rows;        /* row index reached at wrap_scanned           */
    size_t wrap_col;         /* display column reached at wrap_scanned      */
    size_t ckpt_off[TUI_CKPTS];
    size_t ckpt_n;
    size_t ckpt_step;        /* rows between checkpoints; doubles on overflow */
    /* Byte ranges carrying a style of their own: a row whose text starts
     * inside one is painted with it. They live beside the transcript rather
     * than as markup in it, so model or tool output cannot forge one. */
    size_t span_a[TUI_MAX_SPANS];
    size_t span_b[TUI_MAX_SPANS];
    u8     span_k[TUI_MAX_SPANS];
    size_t span_n;
    /* Byte ranges a click acts on, each carrying the caller's id, kept beside
     * the transcript like spans. */
    size_t zone_a[TUI_MAX_ZONES];
    size_t zone_b[TUI_MAX_ZONES];
    u32    zone_id[TUI_MAX_ZONES];
    size_t zone_n;
    u32    zone_open;        /* id of the range being written, 0 when none  */
    size_t zone_open_a;
    u32    click_down;       /* zone the press landed in                    */
    u32    click_id;         /* zone the completed click landed in           */
    u32    hover_id;         /* zone under the pointer, 0 when elsewhere    */
    /* A re-render rebuilds the transcript, so a zone's place on screen is
     * remembered as the rows below it and restored after the replay. */
    u32    anchor_id;
    size_t anchor_below;
    size_t anchor_scroll;
    /* Raised by Esc mid-turn, so a request is cancelled the way SIGINT does
     * it. */
    volatile sig_atomic_t *interrupt;
    f64 last_paint;
    size_t painted_rows;
    size_t painted_cols;
    u64 row_hash[TUI_MAX_ROWS];
    b8 frame_valid;
    size_t bar_first, bar_total, bar_visible;
    b8 bar_valid;
    /* The composer outlives a single tui_readline: text typed while a turn is
     * running is still here when the next prompt opens. */
    char input[YOKE_LINE_BUF];
    size_t input_n;
    size_t input_cur;
    /* The registered command table plus the filtered view of it on screen. */
    const TuiCmd *cmds;
    size_t cmd_n;
    const TuiAlias *aliases;
    size_t alias_n;
    u16 comp_idx[YOKE_MAX_POPUP];      /* matches, as indices into cmds       */
    /* The popup also completes a filesystem path: while `path_mode` is set it
     * is offering these entries instead of the command table, listed from the
     * directory the word at the cursor names. */
    b8 path_mode;
    TuiCmd path_ents[TUI_PATH_ENTS];
    char path_slot[TUI_PATH_ENTS][TUI_PATH_SLOT];
    u8 path_rank[TUI_PATH_ENTS];
    u16 path_depth[TUI_PATH_ENTS];
    u16 path_ord[TUI_PATH_ENTS];     /* slots in the order they are shown   */
    size_t path_n;
    size_t path_at;                  /* offset of the '@' being completed   */
    b8 show_ignored;                 /* offer what an ignore file excludes  */
    /* The patterns in force for the directory being listed, gathered from
     * every .gitignore and .ignore above it. NUL-terminated for fnmatch, each
     * with the length of the path prefix it is relative to. */
    const char *ig_pat[TUI_IGNORE_PATS];
    u8 ig_flag[TUI_IGNORE_PATS];
    u16 ig_base[TUI_IGNORE_PATS];
    size_t ig_n;
    char ig_buf[TUI_IGNORE_BUF];
    size_t ig_buf_n;
    size_t comp_n;
    size_t comp_sel;
    b8 pick_end;                     /* the running picker selects from the end */
    b8 comp_dismissed;               /* Esc/Tab closed it until text changes */
    /* Esc at an idle composer with nothing to dismiss arms a rewind, which the
     * next Esc runs; any other key disarms it. */
    b8 esc_armed;
    /* A one-line answer to the last command, stacked above the popup. The
     * transcript is the conversation, and "nothing to pick from" is not part
     * of it. */
    char notice[160];
    size_t notice_n;
    b8 needs_provider;
    /* Prompt history recall: `draft` holds the text the first Up displaced. */
    History *hist;
    char draft[YOKE_LINE_BUF];
    size_t draft_n;
    /* What the frame put on screen, one entry per row: what selection
     * highlights and copies, so any painted cell is selectable without
     * re-deriving its source. */
    char row_text[TUI_SEL_ROWS][TUI_SEL_ROW_BYTES];
    u16 row_text_n[TUI_SEL_ROWS];
    u16 row_text_w[TUI_SEL_ROWS];
    /* Where each painted transcript row starts, SIZE_MAX for a row from
     * anywhere else: this turns a click's cell into a byte offset. */
    size_t row_src[TUI_SEL_ROWS];
    b8 sel_active;    /* a range is highlighted                            */
    b8 sel_drag;      /* the button is still down                          */
    size_t sel_ar, sel_ac;   /* anchor cell (0-based row, column)          */
    size_t sel_br, sel_bc;   /* head cell                                  */
    char sel_text[TUI_SEL_BYTES];
    f64 copy_notice;  /* status hint deadline after an OSC 52 copy         */
    char out[TUI_OUT_CAP];
    size_t out_n;
} TuiState;

static TuiState g_tui;
static volatile sig_atomic_t g_winch = 0;

static void on_winch(i32 sig) { (void)sig; g_winch = 1; }

/* A frame is assembled here and handed to the terminal as one write. Painting
 * escape by escape lets the terminal display half-drawn frames, which is what
 * flicker looks like while typing quickly. */
static void flush_out(void) {
    size_t off = 0;
    while (off < g_tui.out_n) {
        ssize_t w = write(STDOUT_FILENO, g_tui.out + off, g_tui.out_n - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        break;   /* closed or full pipe: drop the frame rather than spin */
    }
    g_tui.out_n = 0;
}

static void put_raw(const char *s, size_t n) {
    while (n) {
        size_t room = TUI_OUT_CAP - g_tui.out_n;
        if (!room) { flush_out(); room = TUI_OUT_CAP; }
        size_t take = n < room ? n : room;
        memcpy(g_tui.out + g_tui.out_n, s, take);
        g_tui.out_n += take;
        s += take; n -= take;
    }
}

static void put_str(const char *s) { put_raw(s, strlen(s)); }
static void style(const char *s) { if (g_tui.color) put_str(s); }

/* Defined with the selection machinery below; cup() needs them here. */
static void snap_seek(size_t row, size_t col);
static void put_text(const char *s, size_t n);
static void nl_commit(void);

static Str provider_from_url(Str url) {
    size_t start = 0;
    for (size_t i = 0; i + 2 < url.n; i++) {
        if (url.p[i] == ':' && url.p[i + 1] == '/' && url.p[i + 2] == '/') {
            start = i + 3;
            break;
        }
    }
    size_t end = start;
    while (end < url.n && url.p[end] != '/' && url.p[end] != ':') end++;
    Str host = {url.p + start, end - start};
    if (str_eq(host, STR("api.openai.com"))) return STR("openai");
    if (str_eq(host, STR("openrouter.ai"))) return STR("openrouter");
    if (str_eq(host, STR("localhost")) || str_eq(host, STR("127.0.0.1")))
        return STR("local");
    return host.n ? host : STR("unknown");
}

static void capture_cwd(void) {
    char full[sizeof g_tui.cwd_buf];
    if (!getcwd(full, sizeof full)) {
        memcpy(g_tui.cwd_buf, "?", 2);
        g_tui.cwd = STR("?");
        return;
    }
    const char *home = getenv("HOME");
    size_t home_n = home ? strlen(home) : 0;
    if (home_n && !strncmp(full, home, home_n)
        && (full[home_n] == '/' || full[home_n] == '\0')) {
        i32 n = snprintf(g_tui.cwd_buf, sizeof g_tui.cwd_buf, "~%s", full + home_n);
        g_tui.cwd = (Str){g_tui.cwd_buf, n > 0 ? (size_t)n : 0};
    } else {
        size_t n = strlen(full);
        memcpy(g_tui.cwd_buf, full, n + 1);
        g_tui.cwd = (Str){g_tui.cwd_buf, n};
    }
}

static void screen_size(size_t *rows, size_t *cols) {
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *rows = ws.ws_row > 0 ? (size_t)ws.ws_row : 24;
        *cols = ws.ws_col > 0 ? (size_t)ws.ws_col : 80;
    } else {
        *rows = 24;
        *cols = 80;
    }
    if (*rows < 4) *rows = 4;
    if (*rows > TUI_MAX_ROWS) *rows = TUI_MAX_ROWS;
    if (*cols < 8) *cols = 8;
}

static void cup(size_t row, size_t col) {
    char seq[48];
    i32 n = snprintf(seq, sizeof seq, "\033[%zu;%zuH", row, col);
    if (n > 0) put_raw(seq, (size_t)n);
    snap_seek(row, col);
}

/* Invalid input is deliberately one cell and one byte, so arbitrary tool
 * output cannot wedge the renderer. */
static size_t glyph(const char *s, size_t n, i32 *width) {
    if (n == 0) { *width = 0; return 0; }
    u8 c = (u8)s[0];
    if (c < 0x80) { *width = 1; return 1; }
    mbstate_t st = {0};
    wchar_t wc = 0;
    size_t used = mbrtowc(&wc, s, n, &st);
    if (used == (size_t)-1 || used == (size_t)-2 || used == 0) {
        *width = 1;
        return 1;
    }
    i32 w = wcwidth(wc);
    *width = w < 0 ? 1 : w;
    return used;
}

static size_t prev_glyph(const char *s, size_t at) {
    if (at == 0) return 0;
    size_t p = at - 1;
    while (p > 0 && ((u8)s[p] & 0xc0u) == 0x80u) p--;
    return p;
}

static size_t next_glyph(const char *s, size_t n, size_t at) {
    if (at >= n) return n;
    i32 width = 0;
    size_t used = glyph(s + at, n - at, &width);
    (void)width;
    return at + (used ? used : 1);
}

/* ---- screen capture + mouse selection -----------------------------------
 * put_text() mirrors every visible glyph into the per-row snapshot and
 * inverts the cells the selection covers, so selection works over whatever is
 * on screen and copying never has to know which buffer a row came from.
 */
static size_t g_cap_row;   /* 1-based row being captured, 0 = not capturing */
static size_t g_cap_col;   /* 0-based column the next glyph lands on        */

/* Byte offset of the first glyph at or after `cell`; *reached is the column
 * landed on, since a wide glyph can straddle the requested one. */
static size_t row_byte_at(size_t r, size_t cell, size_t *reached) {
    const char *p = g_tui.row_text[r];
    size_t n = g_tui.row_text_n[r], bytes = 0, col = 0;
    while (bytes < n && col < cell) {
        i32 width = 0;
        size_t used = glyph(p + bytes, n - bytes, &width);
        col += width > 0 ? (size_t)width : 0;
        bytes += used;
    }
    *reached = col;
    return bytes;
}

static void snap_seek(size_t row, size_t col) {
    g_cap_row = row && row <= TUI_SEL_ROWS ? row : 0;
    g_cap_col = col ? col - 1 : 0;
    if (!g_cap_row) return;
    size_t r = g_cap_row - 1;
    if (g_cap_col < g_tui.row_text_w[r]) {
        /* Moving left means the painter is about to rewrite the tail. */
        size_t reached = 0;
        size_t bytes = row_byte_at(r, g_cap_col, &reached);
        g_tui.row_text_n[r] = (u16)bytes;
        g_tui.row_text_w[r] = (u16)reached;
    }
    /* Cells the painter skipped over are blanks on screen. */
    while (g_tui.row_text_w[r] < g_cap_col
           && (size_t)g_tui.row_text_n[r] + 1 < TUI_SEL_ROW_BYTES) {
        g_tui.row_text[r][g_tui.row_text_n[r]++] = ' ';
        g_tui.row_text_w[r]++;
    }
}

static void snap_put(const char *s, size_t used, size_t width) {
    if (g_cap_row) {
        size_t r = g_cap_row - 1;
        if ((size_t)g_tui.row_text_n[r] + used < TUI_SEL_ROW_BYTES
            && (size_t)g_tui.row_text_w[r] + width < 0xffffu) {
            memcpy(g_tui.row_text[r] + g_tui.row_text_n[r], s, used);
            g_tui.row_text_n[r] = (u16)((size_t)g_tui.row_text_n[r] + used);
            g_tui.row_text_w[r] = (u16)((size_t)g_tui.row_text_w[r] + width);
        }
    }
    g_cap_col += width;
}

/* Anchor and head in document order, end column exclusive: the cell under
 * the pointer is part of the selection. */
static void sel_norm(size_t *r0, size_t *c0, size_t *r1, size_t *c1) {
    b8 forward = g_tui.sel_ar < g_tui.sel_br
              || (g_tui.sel_ar == g_tui.sel_br && g_tui.sel_ac <= g_tui.sel_bc);
    *r0 = forward ? g_tui.sel_ar : g_tui.sel_br;
    *c0 = forward ? g_tui.sel_ac : g_tui.sel_bc;
    *r1 = forward ? g_tui.sel_br : g_tui.sel_ar;
    *c1 = (forward ? g_tui.sel_bc : g_tui.sel_ac) + 1;
}

static void sel_row_range(size_t screen_row, size_t *c0, size_t *c1) {
    *c0 = 0; *c1 = 0;
    if (!g_tui.sel_active || !screen_row) return;
    size_t r0, s0, r1, s1;
    sel_norm(&r0, &s0, &r1, &s1);
    size_t r = screen_row - 1;
    if (r < r0 || r > r1) return;
    *c0 = r == r0 ? s0 : 0;
    *c1 = r == r1 ? s1 : (size_t)-1;   /* through the end of the row */
}

/* The one place visible text reaches the terminal. */
static void put_text(const char *s, size_t n) {
    size_t sel_c0 = 0, sel_c1 = 0;
    sel_row_range(g_cap_row, &sel_c0, &sel_c1);
    /* Styles are re-emitted between calls and can drop reverse video, so the
     * flag is per call rather than per row. */
    b8 reverse = false;
    for (size_t i = 0; i < n;) {
        i32 width = 0;
        size_t used = glyph(s + i, n - i, &width);
        b8 selected = g_cap_col >= sel_c0 && g_cap_col < sel_c1;
        if (selected != reverse) {
            put_str(selected ? "\033[7m" : "\033[27m");
            reverse = selected;
        }
        put_raw(s + i, used);
        snap_put(s + i, used, width > 0 ? (size_t)width : 0);
        i += used;
    }
    if (reverse) put_str("\033[27m");
}


/* Paint the blanks past a row's text, so a multi-row highlight reads as one
 * block instead of a ragged right edge. */
static void paint_sel_tail(size_t screen_row, size_t screen_cols) {
    size_t c0, c1;
    sel_row_range(screen_row, &c0, &c1);
    if (c1 > screen_cols) c1 = screen_cols;
    size_t start = g_cap_col > c0 ? g_cap_col : c0;
    if (start >= c1) return;
    cup(screen_row, start + 1);
    for (size_t i = start; i < c1; i++) put_text(" ", 1);
}

static size_t sel_extract(char *out, size_t cap) {
    if (!g_tui.sel_active || cap == 0) return 0;
    size_t r0, c0, r1, c1, n = 0;
    sel_norm(&r0, &c0, &r1, &c1);
    if (r1 >= TUI_SEL_ROWS) r1 = TUI_SEL_ROWS - 1;
    for (size_t r = r0; r <= r1; r++) {
        size_t reached = 0;
        size_t a = row_byte_at(r, r == r0 ? c0 : 0, &reached);
        size_t b = row_byte_at(r, r == r1 ? c1 : (size_t)-1, &reached);
        while (b > a && g_tui.row_text[r][b - 1] == ' ') b--;  /* padding */
        for (size_t i = a; i < b && n + 1 < cap; i++)
            out[n++] = g_tui.row_text[r][i];
        if (r < r1 && n + 1 < cap) out[n++] = '\n';
    }
    while (n && out[n - 1] == '\n') n--;
    return n;
}

static void b64_put(const u8 *p, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "abcdefghijklmnopqrstuvwxyz0123456789+/";
    char q[4];
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        u32 v = (u32)p[i] << 16 | (u32)p[i + 1] << 8 | (u32)p[i + 2];
        q[0] = t[v >> 18 & 63]; q[1] = t[v >> 12 & 63];
        q[2] = t[v >> 6 & 63];  q[3] = t[v & 63];
        put_raw(q, 4);
    }
    if (i < n) {
        b8 two = i + 1 < n;
        u32 v = (u32)p[i] << 16 | (two ? (u32)p[i + 1] << 8 : 0u);
        q[0] = t[v >> 18 & 63]; q[1] = t[v >> 12 & 63];
        q[2] = two ? t[v >> 6 & 63] : '='; q[3] = '=';
        put_raw(q, 4);
    }
}

/* OSC 52 lands the copy in the user's own clipboard even across ssh, with no
 * helper process. */
static void sel_copy(void) {
    size_t n = sel_extract(g_tui.sel_text, sizeof g_tui.sel_text);
    if (!n) return;
    put_str("\033]52;c;");
    b64_put((const u8 *)g_tui.sel_text, n);
    put_str("\a");
    flush_out();
    g_tui.copy_notice = yoke_now_seconds() + 2.0;
}

static void sel_clear(void) {
    if (!g_tui.sel_active && !g_tui.sel_drag) return;
    g_tui.sel_active = false;
    g_tui.sel_drag = false;
    g_tui.bar_valid = false;   /* the bar column may have been inverted */
}

static void sel_point(i32 mouse_row, i32 mouse_col, size_t *row, size_t *col) {
    size_t rows = g_tui.painted_rows ? g_tui.painted_rows : 1;
    size_t cols = g_tui.painted_cols ? g_tui.painted_cols : 1;
    if (rows > TUI_SEL_ROWS) rows = TUI_SEL_ROWS;
    size_t r = mouse_row > 1 ? (size_t)mouse_row - 1 : 0;
    size_t c = mouse_col > 1 ? (size_t)mouse_col - 1 : 0;
    *row = r < rows ? r : rows - 1;
    *col = c < cols ? c : cols - 1;
}

static void sel_begin(i32 mouse_row, i32 mouse_col) {
    sel_point(mouse_row, mouse_col, &g_tui.sel_ar, &g_tui.sel_ac);
    g_tui.sel_br = g_tui.sel_ar;
    g_tui.sel_bc = g_tui.sel_ac;
    g_tui.sel_active = false;   /* a plain click just drops the old range */
    g_tui.sel_drag = true;
    g_tui.bar_valid = false;
}

static void sel_extend(i32 mouse_row, i32 mouse_col) {
    if (!g_tui.sel_drag) return;
    sel_point(mouse_row, mouse_col, &g_tui.sel_br, &g_tui.sel_bc);
    g_tui.sel_active = g_tui.sel_br != g_tui.sel_ar
                    || g_tui.sel_bc != g_tui.sel_ac;
    g_tui.bar_valid = false;
}

static void sel_finish(void) {
    if (g_tui.sel_drag && g_tui.sel_active) sel_copy();
    g_tui.sel_drag = false;
}

/* Count visual rows and, optionally, locate a byte cursor. Newlines and soft
 * wrapping both start a new visual row. */
static size_t text_rows(Str s, size_t cols, size_t prompt_cells,
                        size_t cursor, size_t *cursor_row, size_t *cursor_col) {
    size_t row = 0, col = prompt_cells;
    if (cursor_row && cursor == 0) { *cursor_row = row; *cursor_col = col; }
    for (size_t i = 0; i < s.n;) {
        if (s.p[i] == '\n') {
            i++;
            row++; col = 0;
        } else {
            i32 width = 0;
            size_t used = glyph(s.p + i, s.n - i, &width);
            size_t w = width > 0 ? (size_t)width : 0;
            if (w && col > 0 && col + w > cols) { row++; col = 0; }
            i += used;
            col += w;
        }
        if (cursor_row && i == cursor) { *cursor_row = row; *cursor_col = col; }
    }
    if (cursor_row && cursor == s.n) { *cursor_row = row; *cursor_col = col; }
    return row + 1;
}

static void put_safe_clipped(Str s, size_t max_cells, size_t *used_cells) {
    size_t cells = 0;
    for (size_t i = 0; i < s.n;) {
        unsigned char c = (unsigned char)s.p[i];
        if (c < 0x20 || c == 0x7f) { i++; continue; }
        i32 width = 0;
        size_t used = glyph(s.p + i, s.n - i, &width);
        size_t w = width > 0 ? (size_t)width : 0;
        if (cells + w > max_cells) break;
        put_text(s.p + i, used);
        cells += w; i += used;
    }
    if (used_cells) *used_cells += cells;
}

static void pad_row(size_t used, size_t cols) {
    while (used++ < cols) put_raw(" ", 1);
}

enum {
    ROW_PLAIN = 1, ROW_COMPOSER, ROW_STATUS,
    ROW_USER, ROW_REASON, ROW_TOOL, ROW_RESULT, ROW_ERROR, ROW_NOTICE,
    ROW_POPUP, ROW_WELCOME_ART, ROW_WELCOME_TEXT,
    ROW_HEADING, ROW_CODE, ROW_QUOTE,      /* block: the row is theirs   */
    ROW_ZONE, ROW_ZONE_HOVER,              /* block: a clickable row      */
    ROW_BOLD, ROW_EMPH, ROW_MONO, ROW_MARKER  /* inline: bytes are theirs */
};

/* A block style owns every row it touches; an inline style owns its bytes. */
static b8 kind_is_block(u8 kind) { return kind && kind < ROW_BOLD; }

/* Block styles keep the background the row was padded with, so they carry no
 * reset of their own. */
static const char *kind_style(u8 kind) {
    switch (kind) {
        case ROW_USER:         return S_USER_BG S_TEXT;
        case ROW_REASON:       return S_MUTED;
        case ROW_TOOL:         return S_YELLOW;
        case ROW_RESULT:       return S_GREEN;
        case ROW_ERROR:        return S_RED;
        case ROW_NOTICE:       return S_YELLOW;
        case ROW_WELCOME_ART:  return S_CYAN;
        case ROW_WELCOME_TEXT: return S_MUTED;
        case ROW_HEADING:      return S_CYAN;
        case ROW_CODE:         return S_CODE_BG S_TEXT;
        case ROW_ZONE:         return S_LINK;
        case ROW_ZONE_HOVER:   return S_POPUP_BG S_LINK_HOVER;
        case ROW_QUOTE:        return S_MUTED;
        case ROW_BOLD:         return S_BOLD S_TEXT;
        case ROW_EMPH:         return S_ITALIC S_MUTED;
        case ROW_MONO:         return S_MONO;
        case ROW_MARKER:       return S_BLUE;
        case ROW_PLAIN:        return S_TEXT;
        default:               return NULL;
    }
}

static u64 hash_add(u64 h, const void *data, size_t n) {
    const u8 *p = (const u8 *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static u64 row_hash(Str prefix, Str text, u8 kind) {
    u64 h = UINT64_C(1469598103934665603);
    h = hash_add(h, &kind, sizeof kind);
    /* The composer's prompt is dimmed while busy and its placeholder depends
     * on the question, so both belong in the row's identity. */
    if (kind == ROW_COMPOSER) {
        h = hash_add(h, &g_tui.busy, sizeof g_tui.busy);
        h = hash_add(h, &g_tui.ask, sizeof g_tui.ask);
    }
    h = hash_add(h, prefix.p, prefix.n);
    return hash_add(h, text.p, text.n);
}

static b8 row_changed(size_t row, u64 hash, b8 force) {
    size_t index = row - 1;
    if (!force && g_tui.row_hash[index] == hash) return false;
    g_tui.row_hash[index] = hash;
    return true;
}

static char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* Command names are ASCII, so this is enough to make "/CL" offer "/clear". */
static b8 str_starts_ci(Str s, Str prefix) {
    if (s.n < prefix.n) return false;
    for (size_t i = 0; i < prefix.n; i++)
        if (lower_ascii(s.p[i]) != lower_ascii(prefix.p[i])) return false;
    return true;
}

static b8 str_contains_ci(Str s, Str needle) {
    if (needle.n > s.n) return false;
    for (size_t i = 0; i + needle.n <= s.n; i++)
        if (str_starts_ci(str_drop(s, i), needle)) return true;
    return false;
}

/* ---- styled transcript spans --------------------------------------------- */
/* Reasoning arrives as many tiny deltas, so an append that continues the
 * previous span extends it rather than claiming a slot. */
static void span_add(size_t a, size_t b, u8 kind) {
    if (g_tui.span_n && g_tui.span_k[g_tui.span_n - 1] == kind
        && g_tui.span_b[g_tui.span_n - 1] == a) {
        g_tui.span_b[g_tui.span_n - 1] = b;
        return;
    }
    if (g_tui.span_n == TUI_MAX_SPANS) {
        memmove(g_tui.span_a, g_tui.span_a + 1,
                sizeof g_tui.span_a - sizeof g_tui.span_a[0]);
        memmove(g_tui.span_b, g_tui.span_b + 1,
                sizeof g_tui.span_b - sizeof g_tui.span_b[0]);
        memmove(g_tui.span_k, g_tui.span_k + 1,
                sizeof g_tui.span_k - sizeof g_tui.span_k[0]);
        g_tui.span_n--;
    }
    g_tui.span_a[g_tui.span_n] = a;
    g_tui.span_b[g_tui.span_n] = b;
    g_tui.span_k[g_tui.span_n] = kind;
    g_tui.span_n++;
}

/* Scrollback dropped `delta` bytes off the front. */
static void spans_shift(size_t delta) {
    size_t w = 0;
    for (size_t i = 0; i < g_tui.span_n; i++) {
        if (g_tui.span_b[i] <= delta) continue;
        g_tui.span_a[w] = g_tui.span_a[i] > delta ? g_tui.span_a[i] - delta : 0;
        g_tui.span_b[w] = g_tui.span_b[i] - delta;
        g_tui.span_k[w] = g_tui.span_k[i];
        w++;
    }
    g_tui.span_n = w;
}

/* An overflow drops the oldest: a click target scrolled out of the session is
 * one nobody can reach anyway. */
static void zone_add(size_t a, size_t b, u32 id) {
    if (a >= b) return;
    if (g_tui.zone_n == TUI_MAX_ZONES) {
        memmove(g_tui.zone_a, g_tui.zone_a + 1,
                sizeof g_tui.zone_a - sizeof g_tui.zone_a[0]);
        memmove(g_tui.zone_b, g_tui.zone_b + 1,
                sizeof g_tui.zone_b - sizeof g_tui.zone_b[0]);
        memmove(g_tui.zone_id, g_tui.zone_id + 1,
                sizeof g_tui.zone_id - sizeof g_tui.zone_id[0]);
        g_tui.zone_n--;
    }
    g_tui.zone_a[g_tui.zone_n] = a;
    g_tui.zone_b[g_tui.zone_n] = b;
    g_tui.zone_id[g_tui.zone_n] = id;
    g_tui.zone_n++;
}

static void zones_shift(size_t delta) {
    size_t w = 0;
    for (size_t i = 0; i < g_tui.zone_n; i++) {
        if (g_tui.zone_b[i] <= delta) continue;
        g_tui.zone_a[w] = g_tui.zone_a[i] > delta ? g_tui.zone_a[i] - delta : 0;
        g_tui.zone_b[w] = g_tui.zone_b[i] - delta;
        g_tui.zone_id[w] = g_tui.zone_id[i];
        w++;
    }
    g_tui.zone_n = w;
}

/* The zone covering `off`, or 0 where the transcript is not clickable. Zones
 * are appended in order and never overlap, so this bisects on their ends. */
static u32 zone_at_off(size_t off) {
    if (off == SIZE_MAX) return 0;
    size_t lo = 0, hi = g_tui.zone_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_tui.zone_b[mid] <= off) lo = mid + 1; else hi = mid;
    }
    return lo < g_tui.zone_n && off >= g_tui.zone_a[lo] ? g_tui.zone_id[lo] : 0;
}

/* The first span that can still cover `off`, bisected as zones are. */
static size_t span_first(size_t off) {
    size_t lo = 0, hi = g_tui.span_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_tui.span_b[mid] <= off) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* The style a row starting at `off` takes, or 0 when it is plain. */
static u8 span_kind(size_t off) {
    size_t i = span_first(off);
    return i < g_tui.span_n && off >= g_tui.span_a[i] ? g_tui.span_k[i] : 0;
}

/* The style covering `off` and the offset it stops at, never past `limit`. */
static u8 span_run(size_t off, size_t limit, size_t *end) {
    size_t i = span_first(off);
    u8 kind = 0;
    size_t stop = limit;
    if (i < g_tui.span_n) {
        if (off >= g_tui.span_a[i]) {
            kind = g_tui.span_k[i];
            if (g_tui.span_b[i] < stop) stop = g_tui.span_b[i];
        } else if (g_tui.span_a[i] < stop) {
            stop = g_tui.span_a[i];
        }
    }
    *end = stop;
    return kind;
}

/* A run that gained or lost emphasis has to redraw even though its bytes did
 * not move. */
static u64 hash_spans(u64 h, size_t off, size_t n) {
    for (size_t i = span_first(off);
         i < g_tui.span_n && g_tui.span_a[i] < off + n; i++) {
        h = hash_add(h, &g_tui.span_a[i], sizeof g_tui.span_a[i]);
        h = hash_add(h, &g_tui.span_b[i], sizeof g_tui.span_b[i]);
        h = hash_add(h, &g_tui.span_k[i], sizeof g_tui.span_k[i]);
    }
    return h;
}

/* A row whose bytes carry inline styles, one style per run. */
static void paint_runs(Str text, size_t off) {
    for (size_t i = 0; i < text.n;) {
        size_t end = 0;
        u8 kind = span_run(off + i, off + text.n, &end);
        size_t take = end - (off + i);
        style(S_RESET);
        style(kind_style(kind ? kind : ROW_PLAIN));
        put_text(text.p + i, take);
        i += take;
    }
}

static u8 display_kind(u8 kind, Str text) {
    if (kind != ROW_PLAIN) return kind;
    if (text.n >= 2 && text.p[0] == '[' && text.p[text.n - 1] == ']')
        return ROW_NOTICE;
    return kind;
}

/* `text_off` is where the row's bytes sit in the transcript, or SIZE_MAX when
 * they carry no inline styles. */
static void update_text_row(size_t screen_row, Str prefix, Str text,
                            size_t screen_col, size_t screen_cols,
                            u8 kind, size_t text_off, b8 force) {
    kind = display_kind(kind, text);
    if (kind != ROW_PLAIN) text_off = SIZE_MAX;
    u64 hash = row_hash(prefix, text, kind);
    if (text_off != SIZE_MAX) hash = hash_spans(hash, text_off, text.n);
    /* Highlighting is part of a row's appearance, so it is in the diff. */
    size_t sel_c0, sel_c1;
    sel_row_range(screen_row, &sel_c0, &sel_c1);
    hash = hash_add(hash, &sel_c0, sizeof sel_c0);
    hash = hash_add(hash, &sel_c1, sizeof sel_c1);
    if (!row_changed(screen_row, hash, force)) return;
    /* The erase takes the scrollbar cell with it. */
    g_tui.bar_valid = false;
    cup(screen_row, 1);
    put_str(S_RESET "\033[2K");

    if (kind == ROW_COMPOSER || kind == ROW_USER || kind == ROW_CODE) {
        /* The whole row carries the panel colour, so a user turn or a fenced
         * code block reads as a block of screen rather than a prefixed line. */
        style(kind == ROW_USER ? S_USER_BG
            : kind == ROW_CODE ? S_CODE_BG : S_PANEL_BG);
        pad_row(0, screen_cols);
        cup(screen_row, screen_col);
    } else {
        cup(screen_row, screen_col);
    }

    if (kind == ROW_COMPOSER && prefix.n) {
        /* The marker is the mode: a line bound for the shell says so in red. */
        const char *mark = prefix.p[0] == '!' ? S_PANEL_BG S_RED
                                              : S_PANEL_BG S_CYAN;
        style(g_tui.busy ? S_PANEL_BG S_MUTED : mark);
        put_text(prefix.p, prefix.n);
        style(S_PANEL_BG S_TEXT);
    } else if (prefix.n) {
        /* The welcome rows centre themselves with a prefix of spaces; styling
         * it like the text keeps a selection from splitting at the padding. */
        if (kind == ROW_WELCOME_ART) style(S_CYAN);
        else if (kind == ROW_WELCOME_TEXT) style(S_MUTED);
        put_text(prefix.p, prefix.n);
    }

    /* The notice row above a question already says what is being asked, so a
     * placeholder would only argue with it. */
    if (kind == ROW_COMPOSER && prefix.n && text.n == 0 && !g_tui.ask) {
        size_t gutter = screen_col - 1;
        size_t body = screen_cols > gutter * 2 ? screen_cols - gutter * 2 : 0;
        size_t room = body > 2 ? body - 2 : 0;
        style(S_PANEL_BG S_MUTED);
        put_safe_clipped(prefix.p[0] == '!' ? STR("Run a shell command...")
                                            : STR("Message yoke..."),
                         room, NULL);
    } else if (kind == ROW_ZONE || kind == ROW_ZONE_HOVER) {
        /* The indent belongs to the block, not to the click target: styling
         * it would draw a bar across the transcript instead of a label. */
        size_t lead = 0;
        while (lead < text.n && text.p[lead] == ' ') lead++;
        put_text(text.p, lead);
        style(kind_style(kind));
        put_text(text.p + lead, text.n - lead);
        style(S_RESET);
    } else if (text_off != SIZE_MAX) {
        paint_runs(text, text_off);
    } else {
        const char *s = kind_style(kind);
        if (s) style(s);
        put_text(text.p, text.n);
    }
    paint_sel_tail(screen_row, screen_cols);
    style(S_RESET);
}

/* ---- wrapped-row index ---------------------------------------------------
 * A row always starts at column 0, so a checkpoint is the byte offset it
 * begins at and scanning resumes from one with no other state. Anything that
 * rewrites existing transcript bytes drops the index.
 */
static void wrap_invalidate(void) {
    g_tui.wrap_cols = 0;
    g_tui.wrap_scanned = 0;
    g_tui.wrap_rows = 0;
    g_tui.wrap_col = 0;
    g_tui.ckpt_n = 0;
    g_tui.ckpt_step = 64;
}

static void ckpt_record(size_t row, size_t off) {
    if (!g_tui.ckpt_step) g_tui.ckpt_step = 64;
    if (row % g_tui.ckpt_step || row / g_tui.ckpt_step != g_tui.ckpt_n) return;
    if (g_tui.ckpt_n == TUI_CKPTS) {
        /* Keep every second one at twice the spacing, which bounds the index
         * without bounding the scrollback. */
        for (size_t i = 0; i * 2 < g_tui.ckpt_n; i++)
            g_tui.ckpt_off[i] = g_tui.ckpt_off[i * 2];
        g_tui.ckpt_n = (g_tui.ckpt_n + 1) / 2;
        g_tui.ckpt_step *= 2;
        if (row % g_tui.ckpt_step || row / g_tui.ckpt_step != g_tui.ckpt_n) return;
    }
    g_tui.ckpt_off[g_tui.ckpt_n++] = off;
}

/* Bring the index up to date with the transcript; returns the total rows. */
static size_t wrap_scan(size_t cols) {
    if (cols != g_tui.wrap_cols || g_tui.wrap_scanned > g_tui.transcript_n)
        wrap_invalidate();
    if (!g_tui.wrap_cols) {
        g_tui.wrap_cols = cols;
        ckpt_record(0, 0);
    }
    const char *s = g_tui.transcript;
    size_t n = g_tui.transcript_n;
    size_t i = g_tui.wrap_scanned, row = g_tui.wrap_rows, col = g_tui.wrap_col;
    while (i < n) {
        if (s[i] == '\n') {
            i++;
            row++; col = 0;
            ckpt_record(row, i);
            continue;
        }
        i32 width = 0;
        size_t used = glyph(s + i, n - i, &width);
        size_t w = width > 0 ? (size_t)width : 0;
        if (w && col > 0 && col + w > cols) {
            row++; col = 0;
            ckpt_record(row, i);
        }
        i += used;
        col += w;
    }
    g_tui.wrap_scanned = i;
    g_tui.wrap_rows = row;
    g_tui.wrap_col = col;
    return row + 1;
}

/* Byte offset to start painting from for `row`, and the row it lands on. */
static size_t wrap_seek(size_t row, size_t *at_row) {
    size_t k = g_tui.ckpt_step ? row / g_tui.ckpt_step : 0;
    if (g_tui.ckpt_n == 0) { *at_row = 0; return 0; }
    if (k >= g_tui.ckpt_n) k = g_tui.ckpt_n - 1;
    *at_row = k * g_tui.ckpt_step;
    return g_tui.ckpt_off[k];
}

/* A composed line starting with '!' runs in the shell instead of reaching
 * the model, which the composer's marker announces. */
static b8 composer_shell(void) {
    return !g_tui.ask && g_tui.input_n > 0 && g_tui.input[0] == '!';
}

/* `base_off` is where `s` starts inside the transcript, so spans still line
 * up when the painter is handed a slice. */
static void update_text_rows(Str s, size_t base_off, size_t cols,
                             size_t prompt_cells,
                             size_t first_row, size_t visible_rows,
                             size_t screen_row, size_t screen_col,
                             size_t screen_cols, u8 kind, b8 force) {
    size_t row = 0, col = prompt_cells, start = 0;
    size_t painted = 0;
    for (size_t i = 0;;) {
        b8 end = i == s.n;
        b8 newline = !end && s.p[i] == '\n';
        b8 wrap = false;
        size_t used = 0, width = 0;
        if (!end && !newline) {
            i32 glyph_width = 0;
            used = glyph(s.p + i, s.n - i, &glyph_width);
            width = glyph_width > 0 ? (size_t)glyph_width : 0;
            wrap = width && col > 0 && col + width > cols;
        }

        if (end || newline || wrap) {
            if (row >= first_row && row < first_row + visible_rows) {
                Str prefix = (Str){0};
                if (row == 0 && prompt_cells)
                    prefix = composer_shell() ? STR("! ") : STR("› ");
                /* Only the transcript carries spans. */
                u8 row_kind = kind;
                size_t text_off = SIZE_MAX;
                if (kind == ROW_PLAIN) {
                    u8 sk = span_kind(base_off + start);
                    if (kind_is_block(sk)) row_kind = sk;
                    else text_off = base_off + start;
                    size_t sr = screen_row + row - first_row - 1;
                    if (sr < TUI_SEL_ROWS) g_tui.row_src[sr] = base_off + start;
                    /* A clickable row says so, louder under the pointer. */
                    u32 zone = zone_at_off(base_off + start);
                    if (zone) {
                        row_kind = zone == g_tui.hover_id ? ROW_ZONE_HOVER
                                                          : ROW_ZONE;
                        text_off = SIZE_MAX;
                    }
                }
                update_text_row(screen_row + row - first_row, prefix,
                                (Str){s.p + start, i - start}, screen_col,
                                screen_cols, row_kind, text_off, force);
                painted++;
            }
            if (end) break;
            row++;
            col = 0;
            if (newline) { i++; start = i; }
            else start = i;   /* the wrapped glyph belongs to the new row */
            continue;
        }
        i += used;
        col += width;
    }

    /* Rows below short content are the frame's too. */
    while (painted < visible_rows) {
        update_text_row(screen_row + painted, (Str){0}, (Str){0}, screen_col,
                        screen_cols, kind, SIZE_MAX, force);
        painted++;
    }
}

/* ---- slash-command completion popup -------------------------------------
 * A plain list of rows painted directly above the composer, so it takes part
 * in the same row-hash diff as everything else and needs no cursor save or
 * overlay bookkeeping.
 */
/* What the popup is listing: the command table, or the directory entries the
 * '@' completion built. */
static const TuiCmd *popup_items(void) {
    return g_tui.path_mode ? g_tui.path_ents : g_tui.cmds;
}

static void update_popup_row(size_t screen_row, Str name, Str desc,
                             b8 selected, size_t name_cells,
                             size_t screen_col, size_t screen_cols,
                             size_t body_cols, b8 force) {
    u64 hash = row_hash(name, desc, ROW_POPUP);
    hash = hash_add(hash, &selected, sizeof selected);
    hash = hash_add(hash, &name_cells, sizeof name_cells);
    size_t sel_c0, sel_c1;
    sel_row_range(screen_row, &sel_c0, &sel_c1);
    hash = hash_add(hash, &sel_c0, sizeof sel_c0);
    hash = hash_add(hash, &sel_c1, sizeof sel_c1);
    if (!row_changed(screen_row, hash, force)) return;
    /* The erase takes the scrollbar cell with it. */
    g_tui.bar_valid = false;

    const char *bg = selected ? S_POPUP_SEL : S_POPUP_BG;
    cup(screen_row, 1);
    put_str(S_RESET "\033[2K");
    style(bg);
    pad_row(0, screen_cols);
    cup(screen_row, screen_col);

    size_t used = 0;
    style(bg);
    style(selected ? S_CYAN : S_TEXT);
    put_safe_clipped(selected ? STR("\u203a ") : STR("  "), body_cols, &used);
    put_safe_clipped(name, body_cols > used ? body_cols - used : 0, &used);
    while (used < name_cells && used < body_cols) { put_text(" ", 1); used++; }
    style(bg);
    style(S_MUTED);
    if (used < body_cols) put_safe_clipped(desc, body_cols - used, &used);
    paint_sel_tail(screen_row, screen_cols);
    style(S_RESET);
}

/* Display cells a string occupies, control bytes aside. */
static size_t text_cells(Str s) {
    size_t cells = 0;
    for (size_t b = 0; b < s.n;) {
        i32 width = 0;
        b += glyph(s.p + b, s.n - b, &width);
        cells += width > 0 ? (size_t)width : 0;
    }
    return cells;
}

/* Cells taken by the widest visible name, so descriptions line up. */
static size_t popup_name_cells(size_t first, size_t rows) {
    size_t widest = 0;
    for (size_t i = first; i < first + rows && i < g_tui.comp_n; i++) {
        size_t cells = text_cells(popup_items()[g_tui.comp_idx[i]].name);
        if (cells > widest) widest = cells;
    }
    return widest + 4;   /* "\u203a " marker plus a two-cell gap */
}

/* The popup slot answering a command that opened no popup. */
static void update_notice_row(size_t screen_row, Str text, size_t screen_col,
                              size_t screen_cols, size_t body_cols, b8 force) {
    u64 hash = row_hash(STR("notice"), text, ROW_POPUP);
    size_t sel_c0, sel_c1;
    sel_row_range(screen_row, &sel_c0, &sel_c1);
    hash = hash_add(hash, &sel_c0, sizeof sel_c0);
    hash = hash_add(hash, &sel_c1, sizeof sel_c1);
    if (!row_changed(screen_row, hash, force)) return;
    /* The erase takes the scrollbar cell with it. */
    g_tui.bar_valid = false;

    cup(screen_row, 1);
    put_str(S_RESET "\033[2K");
    style(S_POPUP_BG);
    pad_row(0, screen_cols);
    cup(screen_row, screen_col);
    style(S_POPUP_BG S_YELLOW);
    size_t used = 0;
    put_safe_clipped(STR("  "), body_cols, &used);
    if (used < body_cols) put_safe_clipped(text, body_cols - used, &used);
    paint_sel_tail(screen_row, screen_cols);
    style(S_RESET);
}

static void paint_completions(size_t top_row, size_t rows, size_t screen_col,
                              size_t screen_cols, size_t body_cols, b8 force) {
    if (!rows) return;
    /* Keep the selection on screen when there are more matches than room. */
    size_t first = g_tui.comp_sel >= rows ? g_tui.comp_sel - rows + 1 : 0;
    size_t name_cells = popup_name_cells(first, rows);
    for (size_t i = 0; i < rows; i++) {
        const TuiCmd *cmd = &popup_items()[g_tui.comp_idx[first + i]];
        update_popup_row(top_row + i, cmd->name, cmd->desc,
                         first + i == g_tui.comp_sel, name_cells, screen_col,
                         screen_cols, body_cols, force);
    }
}

/* ---- welcome screen ------------------------------------------------------
 * Shown centred in the transcript region until the first output lands. The
 * art rows are centred as one block so the glyphs stay aligned; the prose
 * rows are centred on their own.
 */
typedef struct { Str text; b8 art; } WelcomeLine;

/* -Wpedantic rejects STR()'s compound literal as a static initializer. */
#define WLINE(lit, is_art) { { (lit), sizeof(lit) - 1 }, (is_art) }
static const WelcomeLine k_welcome[] = {
    WLINE("             _",         true),
    WLINE(" _   _  ___ | | _____",  true),
    WLINE("| | | |/ _ \\| |/ / _ \\", true),
    WLINE("| |_| | (_) |   <  __/", true),
    WLINE(" \\__, |\\___/|_|\\_\\___|", true),
    WLINE(" |___/",                 true),
    WLINE("",              false),
    WLINE("yoke " YOKE_VERSION " · a tiny terminal coding agent", false),
    WLINE("",              false),
    WLINE("type a message and press Enter to begin",          false),
};

#define WELCOME_LINES (sizeof k_welcome / sizeof k_welcome[0])

/* Replaces the closing prose line while no endpoint is configured. */
static Str welcome_text(size_t i) {
    if (g_tui.needs_provider && i + 1 == WELCOME_LINES)
        return NO_PROVIDER_HINT;
    return k_welcome[i].text;
}

static size_t welcome_widest(b8 art_only) {
    size_t widest = 0;
    for (size_t i = 0; i < WELCOME_LINES; i++) {
        if (art_only && !k_welcome[i].art) continue;
        size_t cells = text_cells(welcome_text(i));
        if (cells > widest) widest = cells;
    }
    return widest;
}

/* The art is only worth its transcript rows when it fits whole with a row of
 * air above and below. */
static b8 welcome_fits(size_t body_cols, size_t transcript_rows) {
    return welcome_widest(false) <= body_cols
        && WELCOME_LINES + 2 <= transcript_rows;
}

/* Centred on the whole body region rather than on what the overlays left of
 * it, so opening one does not shift the art upward. */
static void paint_welcome(size_t body_rows, size_t transcript_rows,
                          size_t body_col, size_t body_cols,
                          size_t screen_cols, b8 force) {
    static char blanks[256];
    if (blanks[0] != ' ') memset(blanks, ' ', sizeof blanks);
    size_t top = (body_rows - WELCOME_LINES) / 2;
    size_t max_top = transcript_rows - WELCOME_LINES - 1;
    if (top > max_top) top = max_top;
    size_t art_pad = (body_cols - welcome_widest(true)) / 2;
    for (size_t row = 1; row <= transcript_rows; row++) {
        if (row <= top || row > top + WELCOME_LINES) {
            update_text_row(row, (Str){0}, (Str){0}, body_col, screen_cols,
                            ROW_PLAIN, SIZE_MAX, force);
            continue;
        }
        const WelcomeLine *line = &k_welcome[row - top - 1];
        Str text = welcome_text(row - top - 1);
        size_t pad = line->art ? art_pad
                   : (body_cols - text_cells(text)) / 2;
        if (pad > sizeof blanks) pad = sizeof blanks;
        update_text_row(row, (Str){blanks, text.n ? pad : 0}, text,
                        body_col, screen_cols,
                        line->art ? ROW_WELCOME_ART : ROW_WELCOME_TEXT,
                        SIZE_MAX, force);
    }
}

/* The bar has a column of its own, so it sits outside the row-hash diff and
 * keeps a one-line cache instead. */
static void paint_scrollbar(size_t first_row, size_t total_rows,
                            size_t visible_rows, size_t screen_col, b8 force) {
    if (!force && g_tui.bar_valid && g_tui.bar_first == first_row
        && g_tui.bar_total == total_rows && g_tui.bar_visible == visible_rows)
        return;
    g_tui.bar_first = first_row;
    g_tui.bar_total = total_rows;
    g_tui.bar_visible = visible_rows;
    g_tui.bar_valid = true;

    b8 scrollable = total_rows > visible_rows;
    size_t thumb_rows = visible_rows;
    size_t thumb_top = 0;
    if (scrollable) {
        thumb_rows = visible_rows * visible_rows / total_rows;
        if (thumb_rows < 1) thumb_rows = 1;
        size_t travel = visible_rows - thumb_rows;
        size_t scroll_range = total_rows - visible_rows;
        thumb_top = scroll_range ? first_row * travel / scroll_range : 0;
    }
    for (size_t i = 0; i < visible_rows; i++) {
        cup(i + 1, screen_col);
        /* Inheriting the row's style is a highlight bleeding past it. */
        style(S_RESET);
        if (!scrollable) {
            put_str(" ");
        } else if (i >= thumb_top && i < thumb_top + thumb_rows) {
            style(S_CYAN); put_str("┃");
        } else {
            style(S_MUTED); put_str("│");
        }
    }
    style(S_RESET);
}

static Str format_context_size(char *buf, size_t cap) {
    size_t n = g_tui.context_tokens;
    i32 written;
    if (!g_tui.context_known) {
        written = snprintf(buf, cap, "-");
    } else {
        written = snprintf(buf, cap, "%zu", n);
    }
    size_t len = written > 0 ? (size_t)written : 0;
    if (len >= cap) len = cap ? cap - 1 : 0;
    return (Str){buf, len};
}

static void repaint(void) {
    if (!g_tui.fullscreen) return;
    g_tui.last_paint = yoke_now_seconds();

    size_t rows, cols;
    screen_size(&rows, &cols);
    b8 force = !g_tui.frame_valid || g_winch
             || rows != g_tui.painted_rows || cols != g_tui.painted_cols;
    /* Rows not painted from the transcript carry no offset, so the mapping is
     * rebuilt rather than aged. */
    memset(g_tui.row_src, 0xff, sizeof g_tui.row_src);
    g_winch = 0;
    if (force) {
        /* A cleared or resized screen has no cells left to select. */
        sel_clear();
        put_str("\033[?25l\033[H\033[2J");
        memset(g_tui.row_hash, 0, sizeof g_tui.row_hash);
        memset(g_tui.row_text_n, 0, sizeof g_tui.row_text_n);
        memset(g_tui.row_text_w, 0, sizeof g_tui.row_text_w);
    } else {
        put_str("\033[?25l");
    }
    size_t gutter = cols >= 24 ? TUI_BODY_GUTTER : 1;
    size_t body_cols = cols - gutter * 2;
    size_t body_col = gutter + 1;
    size_t cursor_row = 0, cursor_col = 2;
    Str input = { g_tui.input, g_tui.input_n };
    size_t input_cur = g_tui.input_cur;
    /* One dot per byte, so the cursor column still lands where the caret is. */
    char mask[TUI_ASK_MAX];
    if (g_tui.ask_secret) {
        size_t n = input.n < sizeof mask ? input.n : sizeof mask;
        memset(mask, '*', n);
        input = (Str){ mask, n };
        if (input_cur > n) input_cur = n;
    }
    if (composer_shell()) {
        /* The '!' is the prompt marker, not text, so the composer shows the
         * command alone. */
        input = str_drop(input, 1);
        if (input_cur) input_cur--;
    }
    size_t input_rows = text_rows(input, body_cols, 2, input_cur,
                                  &cursor_row, &cursor_col);
    size_t composer_padding = rows >= 6 ? 1 : 0;
    /* A blank row keeps the status line visually outside the composer box. */
    size_t status_gap = composer_padding;
    /* And one keeps the transcript off whatever sits below it, which is the
     * one gap the transcript cannot write for itself. */
    size_t body_gap = composer_padding;
    size_t composer_cap = rows / 3;
    if (composer_cap < 1) composer_cap = 1;
    if (composer_cap > 8) composer_cap = 8;
    size_t composer_rows = input_rows < composer_cap ? input_rows : composer_cap;
    size_t chrome_rows = 1 + composer_padding * 2 + status_gap + body_gap;
    size_t max_composer = rows > chrome_rows ? rows - chrome_rows : 1;
    if (max_composer > 1) max_composer--;   /* keep one transcript row */
    if (composer_rows > max_composer) composer_rows = max_composer;
    size_t body_rows = rows > composer_rows + chrome_rows
                     ? rows - composer_rows - chrome_rows : 1;
    /* Overlays stack upward from the composer and cover the bottom of the
     * transcript rather than pushing it up, so opening one hides the last
     * rows and leaves every other where the reader last saw it. One row
     * always stays uncovered. */
    size_t popup_rows = g_tui.comp_n < TUI_POPUP_ROWS
                      ? g_tui.comp_n : TUI_POPUP_ROWS;
    size_t notice_rows = g_tui.notice_n ? 1 : 0;
    size_t overlay_cap = body_rows > 1 ? body_rows - 1 : 0;
    if (popup_rows > overlay_cap) popup_rows = overlay_cap;
    if (notice_rows + popup_rows > overlay_cap)
        notice_rows = overlay_cap - popup_rows;
    size_t overlay_rows = notice_rows + popup_rows;
    size_t transcript_rows = body_rows - overlay_rows;

    /* Pinned to the bottom unless PageUp moved the viewport. The window is
     * the whole body region, overlays included. */
    size_t all_rows = wrap_scan(body_cols);
    size_t max_scroll = all_rows > body_rows ? all_rows - body_rows : 0;
    if (g_tui.scroll_rows > max_scroll) g_tui.scroll_rows = max_scroll;
    size_t first = all_rows > body_rows + g_tui.scroll_rows
                 ? all_rows - body_rows - g_tui.scroll_rows : 0;
    if (g_tui.transcript_n == 0 && welcome_fits(body_cols, transcript_rows))
        paint_welcome(body_rows, transcript_rows, body_col, body_cols, cols,
                      force);
    else {
        /* Start from the checkpoint nearest the first visible row. */
        size_t at_row = 0;
        size_t off = wrap_seek(first, &at_row);
        Str slice = { g_tui.transcript + off, g_tui.transcript_n - off };
        update_text_rows(slice, off, body_cols, 0, first - at_row,
                         transcript_rows, 1, body_col, cols, ROW_PLAIN, force);
    }
    paint_scrollbar(first, all_rows, transcript_rows, cols, force);
    if (body_gap)
        update_text_row(transcript_rows + 1, (Str){0}, (Str){0}, body_col,
                        cols, ROW_PLAIN, SIZE_MAX, force);

    /* The overlays, in that order. */
    size_t overlay_top = transcript_rows + body_gap + 1;
    if (notice_rows)
        update_notice_row(overlay_top,
                          (Str){ g_tui.notice, g_tui.notice_n }, body_col, cols,
                          body_cols, force);
    paint_completions(overlay_top + notice_rows, popup_rows, body_col,
                      cols, body_cols, force);

    /* Composer, including one quiet row of breathing room on each side. */
    size_t input_first = cursor_row >= composer_rows
                       ? cursor_row - composer_rows + 1 : 0;
    size_t composer_top_row = overlay_top + overlay_rows;
    size_t composer_screen_row = composer_top_row + composer_padding;
    if (composer_padding)
        update_text_row(composer_top_row, (Str){0}, (Str){0}, body_col,
                        cols, ROW_COMPOSER, SIZE_MAX, force);
    update_text_rows(input, 0, body_cols, 2, input_first, composer_rows,
                     composer_screen_row, body_col, cols, ROW_COMPOSER, force);
    if (composer_padding)
        update_text_row(composer_screen_row + composer_rows, (Str){0}, (Str){0},
                        body_col, cols, ROW_COMPOSER, SIZE_MAX, force);

    /* The status line: the bottom row, separated from the composer panel by a
     * blank row and carrying no panel background. */
    size_t status_row = composer_screen_row + composer_rows + composer_padding
                      + status_gap;
    if (status_gap)
        update_text_row(status_row - status_gap, (Str){0}, (Str){0}, body_col,
                        cols, ROW_PLAIN, SIZE_MAX, force);
    const char *status = g_tui.status[0] ? g_tui.status : "ready";
    b8 copied = g_tui.copy_notice > g_tui.last_paint;
    size_t status_sel_c0, status_sel_c1;
    sel_row_range(status_row, &status_sel_c0, &status_sel_c1);
    u64 status_hash = row_hash(g_tui.model, g_tui.provider, ROW_STATUS);
    status_hash = hash_add(status_hash, &copied, sizeof copied);
    status_hash = hash_add(status_hash, &g_tui.mode, sizeof g_tui.mode);
    status_hash = hash_add(status_hash, &status_sel_c0, sizeof status_sel_c0);
    status_hash = hash_add(status_hash, &status_sel_c1, sizeof status_sel_c1);
    status_hash = hash_add(status_hash, g_tui.cwd.p, g_tui.cwd.n);
    status_hash = hash_add(status_hash, status, strlen(status));
    status_hash = hash_add(status_hash, &g_tui.context_tokens,
                           sizeof g_tui.context_tokens);
    status_hash = hash_add(status_hash, &g_tui.context_known,
                           sizeof g_tui.context_known);
    status_hash = hash_add(status_hash, &cols, sizeof cols);
    if (row_changed(status_row, status_hash, force)) {
        cup(status_row, 1); put_str(S_RESET "\033[2K");
        cup(status_row, body_col);
        char context_buf[48];
        Str context = format_context_size(context_buf, sizeof context_buf);
        Str cwd = g_tui.cwd;
        if (body_cols < 72) {
            for (size_t i = cwd.n; i > 0; i--) {
                if (cwd.p[i - 1] == '/' && i < cwd.n) {
                    cwd = (Str){cwd.p + i, cwd.n - i};
                    break;
                }
            }
        }
        size_t used = 0;
        const char *status_style = S_BLUE;
        if (!strcmp(status, "ready")) status_style = S_GREEN;
        else if (strstr(status, "error")) status_style = S_RED;
        else if (!strcmp(status, "thinking")) status_style = S_PURPLE;
        Str separator = STR(" · ");
        size_t separator_cells = 3;
        style(status_style);
        put_safe_clipped(STR("● "), body_cols, &used);
        /* Spelled out rather than only coloured, since the bullet says
         * nothing on a NO_COLOR terminal, and first so a narrow screen clips
         * it last. */
        if (used < body_cols)
            put_safe_clipped(str_c(status), body_cols - used, &used);
        if (body_cols - used >= separator_cells) {
            style(S_MUTED);
            put_safe_clipped(separator, body_cols - used, &used);
            style(S_TEXT);
            put_safe_clipped(g_tui.model, body_cols - used, &used);
        }
        if (body_cols - used >= separator_cells) {
            style(S_MUTED);
            put_safe_clipped(separator, body_cols - used, &used);
            /* Plan mode is the exceptional one and is coloured as such. */
            style(g_tui.mode == MODE_PLAN ? S_YELLOW : S_TEXT);
            put_safe_clipped(g_tui.mode == MODE_PLAN ? STR("plan")
                                                     : STR("build"),
                             body_cols - used, &used);
        }
        if (body_cols - used >= separator_cells) {
            style(S_MUTED);
            put_safe_clipped(separator, body_cols - used, &used);
            style(S_TEXT);
            put_safe_clipped(g_tui.provider, body_cols - used, &used);
        }
        if (body_cols - used >= separator_cells) {
            style(S_MUTED);
            put_safe_clipped(separator, body_cols - used, &used);
            style(S_TEXT);
            put_safe_clipped(cwd, body_cols - used, &used);
        }
        if (body_cols - used >= separator_cells) {
            style(S_MUTED);
            put_safe_clipped(separator, body_cols - used, &used);
            style(S_TEXT);
            put_safe_clipped(context, body_cols - used, &used);
        }
        if (copied && body_cols - used >= separator_cells) {
            style(S_MUTED);
            put_safe_clipped(separator, body_cols - used, &used);
            style(S_GREEN);
            put_safe_clipped(STR("copied"), body_cols - used, &used);
        }
        paint_sel_tail(status_row, cols);
        style(S_RESET);
    }

    if (g_tui.editing) {
        size_t screen_cursor_row = composer_screen_row + cursor_row - input_first;
        size_t screen_cursor_col = gutter + cursor_col + 1;
        if (screen_cursor_col > cols) screen_cursor_col = cols;
        cup(screen_cursor_row, screen_cursor_col);
        put_str("\033[?25h");
    }
    g_tui.painted_rows = rows;
    g_tui.painted_cols = cols;
    g_tui.frame_valid = true;
    flush_out();
}

/* Log lines become transcript notices while the alternate screen is up, so a
 * curl failure reads like the rest of the conversation instead of tearing a
 * hole in the frame. */
static void tui_log_sink(i32 level, Str msg, void *ud) {
    (void)ud;
    static const char *tags[] = {"debug", "info", "warn", "error"};
    tui_printf("\n[%s: %.*s]\n", tags[level], (i32)msg.n, msg.p);
}

void tui_start(Str model, Str base_url, b8 missing_key, size_t tool_count,
               b8 plain) {
    if (g_tui.raw) return;
    memset(&g_tui, 0, sizeof g_tui);
    g_tui.tty = !plain && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    g_tui.model = model;
    g_tui.base_url = base_url;
    g_tui.provider = provider_from_url(base_url);
    tui_set_status("ready");
    capture_cwd();
    const char *term = getenv("TERM");
    g_tui.color = getenv("NO_COLOR") == NULL
               && (!term || strcmp(term, "dumb"));
    (void)setlocale(LC_CTYPE, "");

    if (!g_tui.tty) {
        g_tui.raw = true;
        if (plain) return;
        char banner[512];
        i32 n = snprintf(banner, sizeof banner,
                         "yoke %s · model=%.*s base=%.*s tools=%zu\n",
                         YOKE_VERSION, (i32)model.n, model.p,
                         (i32)base_url.n, base_url.p, tool_count);
        if (n > 0) put_raw(banner, (size_t)n < sizeof banner
                                   ? (size_t)n : sizeof banner - 1);
        if (missing_key) put_str("warn: no API key set\n");
        flush_out();
        return;
    }

    if (tcgetattr(STDIN_FILENO, &g_tui.original_termios) != 0) {
        g_tui.tty = false;
        g_tui.raw = true;
        return;
    }
    struct termios raw = g_tui.original_termios;
    raw.c_lflag &= (tcflag_t)~(tcflag_t)(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(tcflag_t)(IXON | IXOFF | ICRNL | INLCR | ISTRIP);
    raw.c_oflag |= (OPOST | ONLCR);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        g_tui.tty = false;
        g_tui.raw = true;
        return;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGWINCH, &sa, &g_tui.original_winch);

    g_tui.raw = true;
    g_tui.fullscreen = true;
    yoke_log_set_sink(tui_log_sink, NULL);
    /* The composer is always live: it owns the cursor for the whole session. */
    g_tui.editing = true;
    /* 1003 reports motion with no button held, which is what a hovered click
     * target needs, and 1006 keeps coordinates exact past column 223, which
     * in-app text selection needs. Shift still falls through to the
     * terminal's own selection. */
    put_str("\033[?1049h\033[?7l\033[?25l\033[?1003h\033[?1006h\033[?2004h");
    repaint();
}

void tui_stop(void) {
    /* The last row's newline is held back like every other, and nothing
     * follows it here that would commit it. */
    if (!g_tui.fullscreen && g_tui.wrote_any && !g_tui.trail_nl) {
        put_raw("\n", 1);
        g_tui.trail_nl = 1;
        flush_out();
    }
    if (!g_tui.raw) return;
    yoke_log_set_sink(NULL, NULL);
    if (g_tui.fullscreen) {
        put_str("\033[?2004l\033[?1006l\033[?1003l\033[?25h\033[?7h"
                "\033[?1049l");
        flush_out();
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_tui.original_termios);
        (void)sigaction(SIGWINCH, &g_tui.original_winch, NULL);
    }
    g_tui.fullscreen = false;
    g_tui.editing = false;
    g_tui.raw = false;
}

void tui_set_busy(b8 busy) {
    if (g_tui.busy == busy) return;
    g_tui.busy = busy;
    repaint();
}

b8 tui_is_fullscreen(void) { return g_tui.fullscreen; }

size_t tui_body_cols(void) {
    if (!g_tui.fullscreen) return 0;
    size_t rows, cols;
    screen_size(&rows, &cols);
    size_t gutter = cols >= 24 ? TUI_BODY_GUTTER : 1;
    return cols - gutter * 2;
}

i32 tui_input_fd(void) {
    return g_tui.fullscreen && !g_tui.input_eof ? STDIN_FILENO : -1;
}

/* Wrappers for callers outside main.c. */
void tui_enter_raw(void) { tui_start((Str){0}, (Str){0}, false, 0, false); }
void tui_exit_raw(void) { tui_stop(); }

void tui_set_status(const char *status) {
    /* Copied, not aliased, so a caller can build it on the stack. */
    size_t n = 0;
    while (status[n] && n + 1 < sizeof g_tui.status) n++;
    memcpy(g_tui.status, status, n);
    g_tui.status[n] = '\0';
    repaint();
}

void tui_set_model(Str model) {
    g_tui.model = model;
    repaint();
}

void tui_set_provider(Str name) {
    g_tui.provider = name.n ? name : provider_from_url(g_tui.base_url);
    repaint();
}

void tui_set_mode(AgentMode mode) {
    g_tui.mode = mode;
    repaint();
}

void tui_needs_provider(b8 on) {
    g_tui.needs_provider = on;
    repaint();
}

b8 tui_copy(Str text) {
    if (!text.n || text.n > TUI_SEL_BYTES) return false;
    put_str("\033]52;c;");
    b64_put((const u8 *)text.p, text.n);
    put_str("\a");
    flush_out();
    g_tui.copy_notice = yoke_now_seconds() + 2.0;
    repaint();
    return true;
}

void tui_set_context_tokens(size_t tokens) {
    g_tui.context_tokens = tokens;
    g_tui.context_known = true;
    repaint();
}

void tui_notice(Str msg) {
    if (!g_tui.fullscreen) {   /* no popup slot to answer in */
        if (msg.n) { tui_block(); tui_write(msg); tui_write(STR("\n")); }
        return;
    }
    size_t n = msg.n < sizeof g_tui.notice ? msg.n : sizeof g_tui.notice;
    if (n) memcpy(g_tui.notice, msg.p, n);
    g_tui.notice_n = n;
    repaint();
}

void tui_clear_transcript(void) {
    g_tui.notice_n = 0;
    g_tui.transcript_n = 0;
    g_tui.pend_nl = 0;
    g_tui.trail_nl = 0;
    g_tui.span_n = 0;
    g_tui.zone_n = 0;
    g_tui.zone_open = 0;
    g_tui.hover_id = 0;
    wrap_invalidate();
    g_tui.scroll_rows = 0;
    g_tui.frame_valid = false;
    repaint();
}

void tui_zone_begin(u32 id) {
    if (!g_tui.fullscreen || !id) return;
    nl_commit();   /* a zone starts at its first row, not at the air above it */
    g_tui.zone_open = id;
    g_tui.zone_open_a = g_tui.transcript_n;
}

void tui_zone_end(void) {
    if (!g_tui.zone_open) return;
    zone_add(g_tui.zone_open_a, g_tui.transcript_n, g_tui.zone_open);
    g_tui.zone_open = 0;
}

/* Rows from `off` to the end, which is what a zone's place on screen is
 * measured against: a re-render is free to change everything above it. */
static size_t rows_below(size_t off) {
    size_t cols = tui_body_cols();
    if (!cols || off >= g_tui.transcript_n) return 0;
    Str tail = { g_tui.transcript + off, g_tui.transcript_n - off };
    return text_rows(tail, cols, 0, 0, NULL, NULL);
}

static size_t zone_start(u32 id) {
    for (size_t i = g_tui.zone_n; i-- > 0;)
        if (g_tui.zone_id[i] == id) return g_tui.zone_a[i];
    return SIZE_MAX;
}

void tui_anchor_zone(u32 id) {
    size_t off = zone_start(id);
    g_tui.anchor_id = off == SIZE_MAX ? 0 : id;
    g_tui.anchor_below = g_tui.anchor_id ? rows_below(off) : 0;
    g_tui.anchor_scroll = g_tui.scroll_rows;
}

void tui_restore_anchor(void) {
    u32 id = g_tui.anchor_id;
    g_tui.anchor_id = 0;
    /* A viewport pinned to the bottom stays there. */
    if (!id || !g_tui.anchor_scroll) return;
    size_t off = zone_start(id);
    if (off == SIZE_MAX) return;
    size_t below = rows_below(off);
    g_tui.scroll_rows = below > g_tui.anchor_below
                      ? g_tui.anchor_scroll + (below - g_tui.anchor_below)
                      : g_tui.anchor_scroll
                        - (g_tui.anchor_below - below < g_tui.anchor_scroll
                           ? g_tui.anchor_below - below : g_tui.anchor_scroll);
    repaint();
}

void tui_clear(void) {
    g_tui.context_tokens = 0;
    g_tui.context_known = false;
    tui_clear_transcript();
}

/* Append committed bytes. `s` carries no newline the layout has not decided
 * on, since those are held in `pend_nl` until content follows them. */
static void transcript_put(Str s) {
    /* Transcript output answers whatever the notice was about. */
    g_tui.notice_n = 0;
    /* New output shifts the rows a highlight was drawn over; only a live drag
     * keeps it, since the pointer is still choosing the range. */
    if (!g_tui.sel_drag) sel_clear();

    /* Keep the newest half if the bounded scrollback fills. */
    if (s.n >= TUI_TRANSCRIPT_CAP) {
        s.p += s.n - (TUI_TRANSCRIPT_CAP - 1);
        s.n = TUI_TRANSCRIPT_CAP - 1;
        g_tui.transcript_n = 0;
        g_tui.span_n = 0;
        g_tui.zone_n = 0;
        wrap_invalidate();   /* the bytes the index described are gone */
    } else if (g_tui.transcript_n + s.n >= TUI_TRANSCRIPT_CAP) {
        size_t room_for_old = TUI_TRANSCRIPT_CAP - 1 - s.n;
        size_t keep = g_tui.transcript_n;
        if (keep > TUI_TRANSCRIPT_CAP / 2) keep = TUI_TRANSCRIPT_CAP / 2;
        if (keep > room_for_old) keep = room_for_old;
        memmove(g_tui.transcript,
                g_tui.transcript + g_tui.transcript_n - keep, keep);
        spans_shift(g_tui.transcript_n - keep);
        zones_shift(g_tui.transcript_n - keep);
        g_tui.transcript_n = keep;
        wrap_invalidate();   /* every offset in the index just moved */
    }

    /* Strip control bytes and expand tabs, so wrapping is predictable. */
    for (size_t i = 0; i < s.n && g_tui.transcript_n + 4 < TUI_TRANSCRIPT_CAP; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == '\r') continue;
        if (c == '\t') {
            memcpy(g_tui.transcript + g_tui.transcript_n, "    ", 4);
            g_tui.transcript_n += 4;
        } else if (c == '\n' || c >= 0x20) {
            g_tui.transcript[g_tui.transcript_n++] = (char)c;
        } else {
            continue;
        }
        g_tui.trail_nl = c == '\n' ? g_tui.trail_nl + 1 : 0;
    }
    g_tui.scroll_rows = 0;
}

/* One run of content bytes, wherever this run's output is going. */
static void content_put(Str s) {
    if (g_tui.fullscreen) {
        transcript_put(s);
        return;
    }
    put_raw(s.p, s.n);
    g_tui.trail_nl = 0;
    g_tui.wrote_any = true;
}

/* Write back the newlines held since the last content byte, at most one blank
 * row's worth: how much air a block leaves is tui_block's to say. */
static void nl_commit(void) {
    size_t n = g_tui.pend_nl;
    if (!n) return;
    g_tui.pend_nl = 0;
    if (n > 2) n = 2;
    if (g_tui.fullscreen) {
        transcript_put((Str){ "\n\n", n });
    } else {
        put_raw("\n\n", n);
        g_tui.trail_nl += n;
    }
}

void tui_block(void) {
    b8 empty = g_tui.fullscreen ? g_tui.transcript_n == 0 : !g_tui.wrote_any;
    /* One newline closes the row the last block left open and the second
     * leaves the blank one between them; with nothing written yet there is no
     * row to close. A block that committed its own closing rows needs fewer,
     * and never fewer than the writer already asked for. */
    size_t need = empty ? (g_tui.fullscreen ? 1 : 0)
                : g_tui.trail_nl < 2 ? 2 - g_tui.trail_nl : 0;
    if (g_tui.pend_nl < need) g_tui.pend_nl = need;
}

void tui_write(Str s) {
    /* The busiest path in a turn, so it doubles as the pump keeping the
     * composer responsive and servicing a pending resize. */
    if (g_tui.fullscreen) tui_poll_input();
    if (!s.p || s.n == 0) return;
    for (size_t i = 0; i < s.n;) {
        if (s.p[i] == '\n') { g_tui.pend_nl++; i++; continue; }
        size_t k = i;
        while (k < s.n && s.p[k] != '\n') k++;
        nl_commit();
        content_put((Str){ s.p + i, k - i });
        i = k;
    }
    if (!g_tui.fullscreen) { flush_out(); return; }
    /* SSE delivers many tiny deltas, and 15 Hz is plenty for readable text.
     * A newline or a status change still paints at once. */
    f64 now = yoke_now_seconds();
    b8 has_newline = memchr(s.p, '\n', s.n) != NULL;
    if (g_winch || has_newline || now - g_tui.last_paint >= 1.0 / 15.0)
        repaint();
}

void tui_printf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    i32 n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    tui_write((Str){buf, len});
}

void tui_putstr(Str s) { tui_write(s); }

/* Styled output is transcript text like any other; only the byte range
 * recorded around it tells the painter how to paint those rows. */
static void write_span(Str s, u8 kind) {
    if (!g_tui.fullscreen) { tui_write(s); return; }
    /* The air above a block belongs to no style. */
    nl_commit();
    size_t a = g_tui.transcript_n;
    tui_write(s);
    size_t b = g_tui.transcript_n;
    if (b > a) span_add(a, b, kind);
}

void tui_write_styled(Str s, TuiStyle st) {
    static const u8 kinds[] = {
        [TUI_PLAIN] = ROW_PLAIN, [TUI_HEADING] = ROW_HEADING,
        [TUI_CODE] = ROW_CODE,   [TUI_QUOTE] = ROW_QUOTE,
        [TUI_BOLD] = ROW_BOLD,   [TUI_EMPH] = ROW_EMPH,
        [TUI_MONO] = ROW_MONO,   [TUI_MARKER] = ROW_MARKER,
    };
    if (st == TUI_PLAIN || (size_t)st >= sizeof kinds) tui_write(s);
    else write_span(s, kinds[st]);
}

void tui_write_reason(Str s) { write_span(s, ROW_REASON); }
void tui_write_muted(Str s)  { write_span(s, ROW_REASON); }
void tui_write_tool(Str s)   { write_span(s, ROW_TOOL); }
void tui_write_result(Str s) { write_span(s, ROW_RESULT); }
void tui_write_error(Str s)  { write_span(s, ROW_ERROR); }

/* A user turn is a block of screen rather than a labelled line: a padding row
 * above and below, and the whole range recorded so every row it wraps onto
 * carries the panel background. */
void tui_write_user(Str s) {
    tui_block();
    if (!g_tui.fullscreen) {
        tui_write(STR("> "));
        tui_write(s);
        return;
    }
    nl_commit();                          /* the air above is not the box */
    size_t a = g_tui.transcript_n;
    tui_write(STR("\n"));                 /* the box's top padding row */
    tui_write(s);
    /* The padding row below belongs to the box, so it is committed here to
     * fall inside the recorded range rather than left to the next block. */
    tui_write(STR("\n\n"));
    nl_commit();
    size_t b = g_tui.transcript_n;
    if (b > a) span_add(a, b, ROW_USER);
    g_tui.pend_nl = 1;
}

void tui_set_interrupt_flag(volatile sig_atomic_t *flag) {
    g_tui.interrupt = flag;
}

/* One byte read ahead of its key and handed to the next read, which is how
 * an Esc that turned out to introduce nothing keeps the byte behind it. */
static i32 g_pushback = -1;

static b8 input_ready(i32 timeout_ms) {
    if (g_pushback >= 0) return true;
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    i32 rc = poll(&pfd, 1, timeout_ms);
    return rc > 0 && (pfd.revents & (POLLIN | POLLHUP)) != 0;
}

static i32 rbyte(void) {
    if (g_pushback >= 0) { i32 c = g_pushback; g_pushback = -1; return c; }
    unsigned char c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n < 0 && errno == EINTR) return g_winch ? -3 : -2;
    if (n <= 0) return -1;
    return (i32)c;
}

/* Continuation byte of an escape sequence. A bare Esc must not park the
 * reader on a blocking read, especially while polling during a turn. */
static i32 rbyte_soon(void) {
    return input_ready(50) ? rbyte() : -1;
}

typedef struct { i32 final; i32 nparams; i32 p[4]; b8 mouse; } Csi;

static i32 read_csi(Csi *out) {
    memset(out, 0, sizeof *out);
    i32 cur = 0;
    b8 got = false;
    for (;;) {
        i32 c = rbyte_soon();
        if (c < 0) return -1;
        if (c == '<' && out->nparams == 0 && !got) {
            out->mouse = true;
            continue;
        }
        if (c >= '0' && c <= '9') { cur = cur * 10 + c - '0'; got = true; continue; }
        if (c == ';') {
            if (out->nparams < 4) out->p[out->nparams] = cur;
            out->nparams++; cur = 0; got = false; continue;
        }
        if (c >= 0x20 && c <= 0x2f) continue;
        if (got) {
            if (out->nparams < 4) out->p[out->nparams] = cur;
            out->nparams++;
        }
        out->final = c;
        return c;
    }
}

enum {
    KEY_NONE = 0, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END,
    KEY_PREV_WORD, KEY_NEXT_WORD, KEY_NEWLINE, KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_WHEEL_UP, KEY_WHEEL_DOWN, KEY_MOUSE_DOWN, KEY_MOUSE_DRAG, KEY_MOUSE_UP,
    KEY_MOUSE_MOVE, KEY_SHIFT_TAB, KEY_PASTE
};

/* Coordinates of the mouse key just returned by read_escape (1-based). */
static i32 g_mouse_row, g_mouse_col;

static i32 read_escape(void) {
    i32 first = rbyte_soon();
    if (first < 0) return KEY_NONE;
    /* Two Escapes in one burst are two keys: the second opens a sequence of
     * its own rather than closing this one. */
    if (first == 0x1b) { g_pushback = 0x1b; return KEY_NONE; }
    if (first == '\r' || first == '\n') return KEY_NEWLINE;
    if (first == '[') {
        Csi csi;
        i32 final = read_csi(&csi);
        if (csi.mouse && (final == 'M' || final == 'm') && csi.nparams >= 1) {
            i32 button = csi.p[0];
            if (button & 64) return button & 1 ? KEY_WHEEL_DOWN : KEY_WHEEL_UP;
            if (csi.nparams < 3) return KEY_NONE;
            g_mouse_col = csi.p[1];
            g_mouse_row = csi.p[2];
            if (final == 'm') return KEY_MOUSE_UP;
            /* Motion carries 32, and no button held sets both low bits. */
            if (button & 32) return (button & 3) == 3 ? KEY_MOUSE_MOVE
                                                      : KEY_MOUSE_DRAG;
            if ((button & 3) == 0) return KEY_MOUSE_DOWN;
            return KEY_NONE;
        }
        i32 modifier = csi.nparams >= 2 ? csi.p[1] : 0;
        b8 ctrl = modifier == 5;
        switch (final) {
            case 'D': return ctrl ? KEY_PREV_WORD : KEY_LEFT;
            case 'C': return ctrl ? KEY_NEXT_WORD : KEY_RIGHT;
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            case 'Z': return KEY_SHIFT_TAB;
            case '~':
                if (csi.nparams < 1) return KEY_NONE;
                if (csi.p[0] == 1 || csi.p[0] == 7) return KEY_HOME;
                if (csi.p[0] == 4 || csi.p[0] == 8) return KEY_END;
                if (csi.p[0] == 5) return KEY_PAGE_UP;
                if (csi.p[0] == 6) return KEY_PAGE_DOWN;
                /* Bracketed paste: the markers are consumed here, so every
                 * reader of a key sees the same paste state. */
                if (csi.p[0] == 200 || csi.p[0] == 201) {
                    g_tui.pasting = csi.p[0] == 200;
                    g_tui.paste_cr = false;
                    return KEY_PASTE;
                }
                return KEY_NONE;
            default: return KEY_NONE;
        }
    }
    /* SS3: what a terminal in application cursor key mode sends, which is
     * whatever DECCKM the session was started under left set. */
    if (first == 'O') {
        switch (rbyte_soon()) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            default:  return KEY_NONE;
        }
    }
    return KEY_NONE;
}

static size_t prev_word(const char *buf, size_t cur) {
    while (cur > 0 && (buf[cur - 1] == ' ' || buf[cur - 1] == '\t')) cur--;
    while (cur > 0 && buf[cur - 1] != ' ' && buf[cur - 1] != '\t') cur--;
    return cur;
}

/* The composer is multi-line, so Home and Ctrl-A act on a line rather than on
 * the whole buffer. */
static size_t line_start(const char *buf, size_t cur) {
    while (cur > 0 && buf[cur - 1] != '\n') cur--;
    return cur;
}

static size_t line_end(const char *buf, size_t n, size_t cur) {
    while (cur < n && buf[cur] != '\n') cur++;
    return cur;
}

static size_t next_word(const char *buf, size_t n, size_t cur) {
    while (cur < n && (buf[cur] == ' ' || buf[cur] == '\t')) cur++;
    while (cur < n && buf[cur] != ' ' && buf[cur] != '\t') cur++;
    return cur;
}

/* Results are kept in one bounded list held in order, so a walk that meets
 * thousands of paths still costs the 256 it shows: a match better than the
 * worst kept replaces it, and a worse one is dropped where it is found. The
 * order is the match rank, then depth, so a shallow answer comes first, then
 * the name, so a search is reproducible.
 */
static b8 entry_before(u8 rank_a, u16 depth_a, const char *name_a,
                       u8 rank_b, u16 depth_b, const char *name_b) {
    if (rank_a != rank_b) return rank_a < rank_b;
    if (depth_a != depth_b) return depth_a < depth_b;
    for (size_t i = 0;; i++) {
        char ca = lower_ascii(name_a[i]), cb = lower_ascii(name_b[i]);
        if (ca != cb) return ca < cb;
        if (!ca) return false;
    }
}

static void path_insert(const char *rel, size_t rel_n, u8 rank, u16 depth) {
    /* A truncated path is a different path, so an oversized one is left out
     * rather than clipped into the list. */
    if (rel_n + 1 > TUI_PATH_SLOT) return;
    size_t pos = 0;
    while (pos < g_tui.path_n) {
        u16 s = g_tui.path_ord[pos];
        if (entry_before(rank, depth, rel,
                         g_tui.path_rank[s], g_tui.path_depth[s],
                         g_tui.path_slot[s])) break;
        pos++;
    }
    u16 slot;
    size_t shift;
    if (g_tui.path_n < TUI_PATH_ENTS) {
        slot = (u16)g_tui.path_n;
        shift = g_tui.path_n - pos;
        g_tui.path_n++;
    } else {
        if (pos + 1 >= TUI_PATH_ENTS) return;   /* worse than everything kept */
        slot = g_tui.path_ord[TUI_PATH_ENTS - 1];
        shift = TUI_PATH_ENTS - 1 - pos;
    }
    memmove(&g_tui.path_ord[pos + 1], &g_tui.path_ord[pos],
            shift * sizeof g_tui.path_ord[0]);
    g_tui.path_ord[pos] = slot;
    memcpy(g_tui.path_slot[slot], rel, rel_n);
    g_tui.path_slot[slot][rel_n] = '\0';
    g_tui.path_rank[slot] = rank;
    g_tui.path_depth[slot] = depth;
    g_tui.path_ents[slot].name = (Str){ g_tui.path_slot[slot], rel_n };
    g_tui.path_ents[slot].desc = (Str){0};
}

/* How well a path answers the typed word, lower being better: a name that
 * starts with it, a name that holds it, a path that holds it, then a path its
 * letters appear across in order, which is what reaches a deep file without
 * naming the directories above it. */
static b8 path_rank_of(Str rel, Str name, Str q, u8 *out) {
    if (str_starts_ci(name, q))   { *out = 0; return true; }
    if (str_contains_ci(name, q)) { *out = 1; return true; }
    if (str_contains_ci(rel, q))  { *out = 2; return true; }
    size_t k = 0;
    for (size_t i = 0; i < rel.n && k < q.n; i++)
        if (lower_ascii(rel.p[i]) == lower_ascii(q.p[k])) k++;
    if (k < q.n) return false;
    *out = 3;
    return true;
}

static b8 path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- ignore files --------------------------------------------------------
 * .gitignore and .ignore say the same thing about a path, so they are read as
 * one list: what the project says is not part of the work. The picker hides
 * what they exclude unless `show_ignored` is on, which is a default rather
 * than a rule, since a path typed by hand still reaches anything.
 *
 * The subset understood is the one people write: comments, '!' negation with
 * the last match winning, a trailing '/' for directories only, and a leading
 * or embedded '/' anchoring the pattern to the file's own directory. '**' is
 * left to fnmatch, which is enough while one directory is listed at a time.
 */
enum { IG_NEG = 1, IG_DIRONLY = 2, IG_PATHNAME = 4 };

static void ignore_add(Str pat, size_t base_n) {
    pat = str_trim(pat);
    if (!pat.n || pat.p[0] == '#') return;
    u8 flags = 0;
    if (pat.p[0] == '!') { flags |= IG_NEG; pat = str_drop(pat, 1); }
    if (pat.n && pat.p[pat.n - 1] == '/') { flags |= IG_DIRONLY; pat.n--; }
    if (pat.n && pat.p[0] == '/') { flags |= IG_PATHNAME; pat = str_drop(pat, 1); }
    else for (size_t i = 0; i + 1 < pat.n; i++)
        if (pat.p[i] == '/') { flags |= IG_PATHNAME; break; }
    if (!pat.n || g_tui.ig_n >= TUI_IGNORE_PATS) return;
    if (g_tui.ig_buf_n + pat.n + 1 > sizeof g_tui.ig_buf) return;
    char *slot = g_tui.ig_buf + g_tui.ig_buf_n;
    memcpy(slot, pat.p, pat.n);
    slot[pat.n] = '\0';
    g_tui.ig_buf_n += pat.n + 1;
    g_tui.ig_pat[g_tui.ig_n] = slot;
    g_tui.ig_flag[g_tui.ig_n] = flags;
    g_tui.ig_base[g_tui.ig_n] = (u16)base_n;
    g_tui.ig_n++;
}

static void ignore_load(const char *path, size_t base_n) {
    i32 fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[TUI_IGNORE_BUF];
    ssize_t got = read(fd, buf, sizeof buf);
    (void)close(fd);
    if (got <= 0) return;
    size_t n = (size_t)got, start = 0;
    for (size_t i = 0; i <= n; i++)
        if (i == n || buf[i] == '\n') {
            ignore_add((Str){ buf + start, i - start }, base_n);
            start = i + 1;
        }
}

/* The ignore files of one directory, in force from it downward. */
static void ignore_push(const char *dir, size_t n) {
    static const char *names[] = { ".gitignore", ".ignore" };
    char path[YOKE_MAX_PATH];
    if (n + 12 >= sizeof path || n > UINT16_MAX) return;
    memcpy(path, dir, n);
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        memcpy(path + n, names[k], strlen(names[k]) + 1);
        ignore_load(path, n);
    }
}

/* Every ignore file from the working directory down to `dir`, outermost
 * first, since the last pattern that matches decides. Rebuilt per listing
 * rather than cached: an ignore file edited mid-session is a few opens, and a
 * stale answer is a file the picker refuses to show. */
static void ignore_build(Str dir) {
    g_tui.ig_n = 0;
    g_tui.ig_buf_n = 0;
    /* An absolute path is outside the project the ignore files describe. */
    if (dir.n && dir.p[0] == '/') return;
    for (size_t base = 0;;) {
        ignore_push(dir.p, base);
        if (base >= dir.n) break;
        while (base < dir.n && dir.p[base] != '/') base++;
        if (base < dir.n) base++;
    }
}

static b8 ignore_match(const char *rel, size_t rel_n, b8 is_dir) {
    b8 ignored = false;
    for (size_t i = 0; i < g_tui.ig_n; i++) {
        u8 f = g_tui.ig_flag[i];
        if ((f & IG_DIRONLY) && !is_dir) continue;
        if (g_tui.ig_base[i] > rel_n) continue;
        const char *sub = rel + g_tui.ig_base[i];
        if (!(f & IG_PATHNAME)) {
            const char *slash = strrchr(sub, '/');
            if (slash) sub = slash + 1;
        }
        i32 flags = f & IG_PATHNAME ? FNM_PATHNAME : 0;
        if (fnmatch(g_tui.ig_pat[i], sub, flags) == 0) ignored = !(f & IG_NEG);
    }
    return ignored;
}

/* One directory, and every directory under it while a word is being matched.
 * A directory keeps its trailing slash, so accepting one is a step into it
 * rather than an answer, and recursing costs the slash already there. Its own
 * ignore files are pushed for the subtree and popped on the way out. */
static void path_walk(char *path, size_t n, size_t root_n, Str q, u16 depth,
                      u16 max_depth, size_t *budget) {
    DIR *d = opendir(n ? path : ".");
    if (!d) return;
    for (struct dirent *e; *budget && (e = readdir(d)) != NULL;) {
        (*budget)--;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        Str name = str_c(e->d_name);
        if (n + name.n + 2 >= YOKE_MAX_PATH) continue;
        memcpy(path + n, name.p, name.n);
        size_t end = n + name.n;
        path[end] = '\0';
        b8 is_dir;
        /* d_type answers for most entries; the rest, and a symlink, cost the
         * stat they would have cost anyway. */
        if (e->d_type == DT_DIR) is_dir = true;
        else if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK)
            is_dir = path_is_dir(path);
        else is_dir = false;
        /* Nothing inside a repository's own bookkeeping is worth mentioning
         * to a model, whatever the ignore files say. */
        if (is_dir && str_eq(name, STR(".git"))) continue;
        if (!g_tui.show_ignored && ignore_match(path, end, is_dir)) continue;

        u8 rank = is_dir ? 0 : 1;   /* with nothing typed, directories first */
        Str rel = { path + root_n, end - root_n };
        b8 hit = !q.n || path_rank_of(rel, name, q, &rank);
        if (is_dir) {
            path[end] = '/';
            path[end + 1] = '\0';
            if (hit) path_insert(path, end + 1, rank, depth);
            if (depth < max_depth && *budget) {
                size_t pats = g_tui.ig_n, bytes = g_tui.ig_buf_n;
                ignore_push(path, end + 1);
                path_walk(path, end + 1, root_n, q, (u16)(depth + 1),
                          max_depth, budget);
                g_tui.ig_n = pats;
                g_tui.ig_buf_n = bytes;
            }
        } else if (hit) {
            path_insert(path, end, rank, depth);
        }
    }
    (void)closedir(d);
}

/* What the popup offers for the word after '@': the entries of the directory
 * it names, and once something is typed after that directory, every path
 * under it that the word matches. A dotfile is offered like `ls -A` does, an
 * ignored path is not. */
static void path_refresh(Str prefix, Str keep) {
    size_t cut = 0;
    for (size_t i = prefix.n; i-- > 0;)
        if (prefix.p[i] == '/') { cut = i + 1; break; }
    Str dir = { prefix.p, cut };
    Str base = { prefix.p + cut, prefix.n - cut };
    char path[YOKE_MAX_PATH];
    if (dir.n + 1 >= sizeof path) return;
    memcpy(path, dir.p, dir.n);
    path[dir.n] = '\0';

    g_tui.path_n = 0;
    ignore_build(dir);
    /* The walk is bounded rather than complete: a tree nobody ignored is
     * still answered in the time a keystroke has. */
    size_t budget = TUI_PATH_SCAN;
    path_walk(path, dir.n, dir.n, base, 0,
              base.n ? TUI_PATH_DEPTH : 0, &budget);

    size_t n = g_tui.path_n;
    for (size_t i = 0; i < n; i++) g_tui.comp_idx[i] = g_tui.path_ord[i];
    g_tui.comp_n = n;
    g_tui.comp_sel = 0;
    /* A rebuilt list keeps the entry the selection was on, so moving it and
     * then typing is not a move undone. */
    for (size_t i = 0; keep.n && i < n; i++)
        if (str_eq(g_tui.path_ents[g_tui.comp_idx[i]].name, keep)) {
            g_tui.comp_sel = i;
            break;
        }
    g_tui.path_mode = n > 0;
}

/* The word ending at the cursor, when it is a '@' starting one: that is a
 * path being picked rather than text being typed. */
static b8 path_prefix(Str *out) {
    size_t cur = g_tui.input_cur;
    size_t start = cur;
    while (start > 0) {
        char c = g_tui.input[start - 1];
        if (c == ' ' || c == '\t' || c == '\n') break;
        start--;
    }
    if (start >= cur || g_tui.input[start] != '@') return false;
    g_tui.path_at = start;
    *out = (Str){ g_tui.input + start + 1, cur - start - 1 };
    return true;
}

/* The popup is offered while the buffer is a single unfinished word starting
 * with '/', which is when a command name is still being typed, and for the
 * '@' word at the cursor, which is a path. */
static void completion_refresh(void) {
    /* The selected path is copied out rather than aliased: the rebuild it
     * survives is what overwrites the buffer it lives in. */
    char keep[YOKE_MAX_PATH];
    size_t keep_n = 0;
    size_t previous = SIZE_MAX;
    if (g_tui.comp_n) {
        Str sel = popup_items()[g_tui.comp_idx[g_tui.comp_sel]].name;
        if (!g_tui.path_mode) previous = g_tui.comp_idx[g_tui.comp_sel];
        else if (sel.n <= sizeof keep) {
            memcpy(keep, sel.p, sel.n);
            keep_n = sel.n;
        }
    }
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.path_mode = false;
    if (g_tui.comp_dismissed) return;
    Str prefix;
    if (path_prefix(&prefix)) {
        path_refresh(prefix, (Str){ keep, keep_n });
        return;
    }
    if (!g_tui.cmds || !g_tui.cmd_n) return;
    Str in = { g_tui.input, g_tui.input_n };
    if (in.n == 0 || in.p[0] != '/') return;
    for (size_t i = 0; i < in.n; i++)
        if (in.p[i] == ' ' || in.p[i] == '\t' || in.p[i] == '\n') return;
    size_t exact = SIZE_MAX;
    for (size_t i = 0; i < g_tui.cmd_n && g_tui.comp_n < YOKE_MAX_COMMANDS; i++) {
        if (!str_starts_ci(g_tui.cmds[i].name, in)) continue;
        /* Narrowing the list keeps the highlight on the same command. */
        if (i == previous) g_tui.comp_sel = g_tui.comp_n;
        if (g_tui.cmds[i].name.n == in.n) exact = g_tui.comp_n;
        g_tui.comp_idx[g_tui.comp_n++] = (u16)i;
    }
    /* An alias is a way in, so what it matches is the command it stands for:
     * the row is that command and appending it twice would list it twice. */
    for (size_t a = 0; a < g_tui.alias_n && g_tui.comp_n < YOKE_MAX_COMMANDS;
         a++) {
        if (!str_starts_ci(g_tui.aliases[a].alias, in)) continue;
        size_t cmd = SIZE_MAX;
        for (size_t i = 0; i < g_tui.cmd_n; i++)
            if (str_eq(g_tui.cmds[i].name, g_tui.aliases[a].name)) {
                cmd = i;
                break;
            }
        if (cmd == SIZE_MAX) continue;
        b8 listed = false;
        for (size_t i = 0; i < g_tui.comp_n; i++)
            if (g_tui.comp_idx[i] == cmd) { listed = true; break; }
        if (listed) continue;
        if (cmd == previous) g_tui.comp_sel = g_tui.comp_n;
        if (g_tui.aliases[a].alias.n == in.n) exact = g_tui.comp_n;
        g_tui.comp_idx[g_tui.comp_n++] = (u16)cmd;
    }
    /* A name typed out in full is the command asked for even when a longer
     * one starts with it, so "/mode" must not submit "/model". */
    if (exact != SIZE_MAX) g_tui.comp_sel = exact;
}

static void completion_move(i32 delta) {
    if (!g_tui.comp_n) return;
    size_t n = g_tui.comp_n;
    g_tui.comp_sel = (g_tui.comp_sel + (delta > 0 ? 1 : n - 1)) % n;
}

/* A name typed out in full has nothing left to complete, so the completion
 * keys must not swallow the keystroke that finishes the command. */
static b8 completion_would_change(void) {
    if (!g_tui.comp_n) return false;
    if (g_tui.path_mode) return true;
    Str name = g_tui.cmds[g_tui.comp_idx[g_tui.comp_sel]].name;
    return name.n != g_tui.input_n
        || memcmp(name.p, g_tui.input, name.n) != 0;
}

/* The picked path replaces the word it was picked for, the '@' left in place
 * as the marker it is. */
static void path_accept(Str name) {
    size_t start = g_tui.path_at + 1;
    size_t cur = g_tui.input_cur;
    size_t tail = g_tui.input_n - cur;
    if (start + name.n + tail + 1 > sizeof g_tui.input) return;
    memmove(g_tui.input + start + name.n, g_tui.input + cur, tail);
    memcpy(g_tui.input + start, name.p, name.n);
    g_tui.input_n = start + name.n + tail;
    g_tui.input_cur = start + name.n;
    g_tui.input[g_tui.input_n] = '\0';
    /* A directory is a step rather than a choice, so its contents are the
     * next list; a file is the answer and closes the popup. */
    if (name.n && name.p[name.n - 1] == '/') {
        g_tui.comp_dismissed = false;
        completion_refresh();
        return;
    }
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.path_mode = false;
    g_tui.comp_dismissed = true;
}

static void completion_accept(void) {
    if (!g_tui.comp_n) return;
    if (g_tui.path_mode) {
        path_accept(g_tui.path_ents[g_tui.comp_idx[g_tui.comp_sel]].name);
        return;
    }
    Str name = g_tui.cmds[g_tui.comp_idx[g_tui.comp_sel]].name;
    size_t n = name.n < sizeof g_tui.input - 1 ? name.n : sizeof g_tui.input - 1;
    memcpy(g_tui.input, name.p, n);
    g_tui.input[n] = '\0';
    g_tui.input_n = n;
    g_tui.input_cur = n;
    /* Showing the one entry a complete name still matches is noise. */
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.comp_dismissed = true;
}

b8 tui_show_ignored(void) { return g_tui.show_ignored; }

void tui_set_show_ignored(b8 on) { g_tui.show_ignored = on; }

void tui_set_history(History *h) {
    g_tui.hist = h;
    g_tui.draft_n = 0;
    if (h) history_reset_cursor(h);
}

static void composer_load(char *buf, size_t *n, size_t *cur, Str s) {
    size_t take = s.n < YOKE_LINE_BUF - 1 ? s.n : YOKE_LINE_BUF - 1;
    if (take) memcpy(buf, s.p, take);
    buf[take] = '\0';
    *n = take;
    *cur = take;
}

/* Stepping off the newest entry brings back the draft the first Up put
 * aside, so recall never eats typed text. */
static b8 history_recall(i32 dir, char *buf, size_t *n, size_t *cur) {
    History *h = g_tui.hist;
    if (!h || !h->n) return false;
    Str entry;
    if (dir < 0) {
        if (!history_browsing(h)) {
            memcpy(g_tui.draft, buf, *n);
            g_tui.draft_n = *n;
        }
        if (!history_prev(h, &entry)) return false;
        composer_load(buf, n, cur, entry);
        return true;
    }
    if (!history_browsing(h)) return false;
    if (history_next(h, &entry)) { composer_load(buf, n, cur, entry); return true; }
    composer_load(buf, n, cur, (Str){ g_tui.draft, g_tui.draft_n });
    return true;
}

void tui_set_input(Str s) {
    if (!g_tui.fullscreen) return;
    composer_load(g_tui.input, &g_tui.input_n, &g_tui.input_cur, s);
    g_tui.comp_dismissed = false;
    completion_refresh();
    repaint();
}

void tui_set_commands(const TuiCmd *cmds, size_t n) {
    g_tui.cmds = cmds;
    g_tui.cmd_n = n < YOKE_MAX_COMMANDS ? n : YOKE_MAX_COMMANDS;
}

void tui_set_aliases(const TuiAlias *aliases, size_t n) {
    g_tui.aliases = aliases;
    g_tui.alias_n = n;
}

/* The search is literal and case-insensitive: a name either holds what was
 * typed or it does not, with no fuzzy ordering to explain. */
static void pick_filter(Str query) {
    g_tui.comp_n = 0;
    for (size_t i = 0; i < g_tui.cmd_n && g_tui.comp_n < YOKE_MAX_POPUP; i++)
        if (str_contains_ci(g_tui.cmds[i].name, query))
            g_tui.comp_idx[g_tui.comp_n++] = (u16)i;
    /* The narrowed list keeps the anchor it opened on, so a search never
     * moves the default choice to the other end. */
    g_tui.comp_sel = g_tui.pick_end && g_tui.comp_n ? g_tui.comp_n - 1 : 0;
}

/* Viewport keys, shared by the composer and the picker: a list drawn over the
 * transcript is no reason to lose the transcript. */
static b8 scroll_key(i32 key) {
    size_t rows, cols;
    screen_size(&rows, &cols); (void)cols;
    size_t page = rows > 4 ? rows - 4 : 1;
    if (key == KEY_PAGE_UP) g_tui.scroll_rows += page;
    else if (key == KEY_PAGE_DOWN)
        g_tui.scroll_rows = g_tui.scroll_rows > page ? g_tui.scroll_rows - page
                                                     : 0;
    else if (key == KEY_WHEEL_UP) g_tui.scroll_rows += 3;
    else if (key == KEY_WHEEL_DOWN)
        g_tui.scroll_rows = g_tui.scroll_rows > 3 ? g_tui.scroll_rows - 3 : 0;
    else return false;
    return true;
}

/* The search box is the notice row, which already sits above the popup. */
static void pick_search_row(Str query) {
    char row[sizeof g_tui.notice];
    i32 n = snprintf(row, sizeof row, "search: %.*s%s", (i32)query.n, query.p,
                     g_tui.comp_n ? "" : "  (no match)");
    if (n < 0) return;
    size_t len = (size_t)n < sizeof row ? (size_t)n : sizeof row - 1;
    tui_notice((Str){row, len});   /* repaints */
}

/* A picker is answered by choosing a row, so Enter takes it and Escape
 * declines; a settings screen is acted on, so Space acts on the selected row
 * and both Enter and Escape close it. */
typedef enum { PICK_CHOOSE, PICK_SETTINGS } PickKind;

/* A modal list over the same popup the composer completes with; only the
 * source of the entries differs, so it is swapped in and the composer's own
 * state restored on the way out.
 *
 * A long list also takes the keyboard: scrolling six visible rows through
 * hundreds of entries is not a way to choose one. */
static b8 pick_impl(Str title, const TuiCmd *items, size_t n,
                    TuiPickAnchor anchor, size_t start, PickKind kind,
                    size_t *out) {
    if (!g_tui.fullscreen || !items || !n || !out) return false;
    if (n > YOKE_MAX_POPUP) n = YOKE_MAX_POPUP;

    const TuiCmd *saved_cmds = g_tui.cmds;
    size_t saved_cmd_n = g_tui.cmd_n;
    b8 saved_dismissed = g_tui.comp_dismissed;
    char saved_status[sizeof g_tui.status];
    char saved_notice[sizeof g_tui.notice];
    size_t saved_notice_n = g_tui.notice_n;
    memcpy(saved_status, g_tui.status, sizeof saved_status);
    memcpy(saved_notice, g_tui.notice, sizeof saved_notice);

    b8 search = kind == PICK_CHOOSE && n > TUI_PICK_SEARCH_MIN;
    char query[TUI_PICK_QUERY];
    size_t query_n = 0;

    g_tui.cmds = items;
    g_tui.cmd_n = n;
    g_tui.path_mode = false;   /* the popup is the picker's now */
    g_tui.comp_n = n;
    g_tui.pick_end = anchor == TUI_PICK_LAST;
    g_tui.comp_sel = start < n ? start : (g_tui.pick_end ? n - 1 : 0);
    for (size_t i = 0; i < n; i++) g_tui.comp_idx[i] = (u16)i;
    /* The status is set last, so the frame announcing the picker already
     * carries the list and the search box. */
    if (search) pick_search_row((Str){query, query_n});
    /* Unless something already answered there: a screen reopened after acting
     * on a row would paint its hint over what the action had to say. */
    if (kind == PICK_SETTINGS && !g_tui.notice_n)
        tui_notice(STR("Space changes the selected row · Enter or Esc "
                       "closes"));
    char status[sizeof g_tui.status];
    snprintf(status, sizeof status, "%.*s", (i32)title.n, title.p);
    tui_set_status(status);   /* repaints */

    b8 chosen = false;
    for (;;) {
        i32 c = rbyte();
        if (c == -3) { repaint(); continue; }
        /* -2 is a signal that is not a resize, so SIGINT cancels here just as
         * it abandons a draft at the prompt. */
        /* Pasted text is a query rather than keys, so nothing in it picks,
         * acts on a row or cancels. */
        if (c < 0) break;
        if ((c == 0x03 || c == 0x04) && !g_tui.pasting) break;
        if (g_tui.pasting && (c == '\r' || c == '\n')) continue;
        if ((c == '\r' || c == '\n') && !g_tui.pasting) {
            if (kind == PICK_SETTINGS) break;
            if (!g_tui.comp_n) continue;
            *out = g_tui.comp_idx[g_tui.comp_sel];
            chosen = true;
            break;
        }
        if (c == ' ' && kind == PICK_SETTINGS && !g_tui.pasting) {
            if (!g_tui.comp_n) continue;
            chosen = true;
            break;
        }
        if (c == 0x0e) completion_move(1);
        else if (c == 0x10) completion_move(-1);
        else if (c == 0x1b) {
            i32 key = read_escape();
            if (key == KEY_DOWN) completion_move(1);
            else if (key == KEY_UP) completion_move(-1);
            else if (scroll_key(key)) { /* the transcript moves, not the list */ }
            else if (key == KEY_NONE) break;          /* bare Esc cancels */
        } else if (search) {
            if (c == 0x7f || c == 0x08) {
                if (query_n) query_n = prev_glyph(query, query_n);
            } else if (c == 0x15) {
                query_n = 0;
            } else if (((c >= 0x20 && c < 0x7f) || c >= 0x80)
                       && query_n + 1 < sizeof query) {
                query[query_n++] = (char)c;
            } else {
                continue;
            }
            pick_filter((Str){query, query_n});
            pick_search_row((Str){query, query_n});
            continue;
        }
        repaint();
    }

    /* The row the selection was left on, so a settings screen reopened after
     * a change opens where it was rather than back at the top. */
    if (kind == PICK_SETTINGS && g_tui.comp_n)
        *out = g_tui.comp_idx[g_tui.comp_sel];

    g_tui.cmds = saved_cmds;
    g_tui.cmd_n = saved_cmd_n;
    g_tui.pick_end = false;
    g_tui.comp_dismissed = saved_dismissed;
    memcpy(g_tui.notice, saved_notice, sizeof saved_notice);
    g_tui.notice_n = saved_notice_n;
    /* The match list described the picker's entries; rebuild it. */
    completion_refresh();
    memcpy(g_tui.status, saved_status, sizeof saved_status);
    repaint();
    return chosen;
}

b8 tui_pick(Str title, const TuiCmd *items, size_t n, TuiPickAnchor anchor,
            size_t *out) {
    return pick_impl(title, items, n, anchor, SIZE_MAX, PICK_CHOOSE, out);
}

b8 tui_pick_from(Str title, const TuiCmd *items, size_t n, size_t start,
                 size_t *out) {
    return pick_impl(title, items, n, TUI_PICK_FIRST, start, PICK_CHOOSE, out);
}

b8 tui_settings(Str title, const TuiCmd *rows, size_t n, size_t *sel) {
    if (!sel) return false;
    size_t start = *sel;
    return pick_impl(title, rows, n, TUI_PICK_FIRST, start, PICK_SETTINGS,
                     sel);
}

/* A question the composer is borrowed for. The editor is deliberately not the
 * composer's own, since a question wants none of its history recall,
 * completion or shell mode, and a secret answer must not survive in a buffer
 * the next frame paints. The question itself sits in the notice row, where
 * every other answer to a command appears. */
b8 tui_ask(Str question, b8 secret, char *out, size_t cap) {
    if (!g_tui.fullscreen || !out || cap < 2) return false;
    size_t limit = cap - 1 < TUI_ASK_MAX ? cap - 1 : TUI_ASK_MAX;

    char saved_input[YOKE_LINE_BUF];
    size_t saved_n = g_tui.input_n, saved_cur = g_tui.input_cur;
    char saved_notice[sizeof g_tui.notice];
    size_t saved_notice_n = g_tui.notice_n;
    size_t saved_comp_n = g_tui.comp_n;
    b8 saved_editing = g_tui.editing;
    memcpy(saved_input, g_tui.input, saved_n);
    memcpy(saved_notice, g_tui.notice, sizeof saved_notice);

    g_tui.ask = true;
    g_tui.ask_secret = secret;
    g_tui.editing = true;
    g_tui.input_n = 0;
    g_tui.input_cur = 0;
    g_tui.comp_n = 0;
    char row[sizeof g_tui.notice];
    i32 rn = snprintf(row, sizeof row, "%.*s  (Esc cancels)",
                      (i32)question.n, question.p);
    tui_notice(rn > 0 ? (Str){ row, (size_t)rn < sizeof row ? (size_t)rn
                                                            : sizeof row - 1 }
                      : question);   /* repaints */

    b8 answered = false;
    for (;;) {
        i32 c = rbyte();
        if (c == -3) { repaint(); continue; }
        if (c < 0) break;
        if ((c == 0x03 || c == 0x04) && !g_tui.pasting) break;
        if ((c == '\r' || c == '\n') && !g_tui.pasting) {
            answered = g_tui.input_n > 0;
            break;
        }
        if (g_tui.pasting && (c == '\r' || c == '\n')) continue;
        if (c == 0x1b) {
            i32 key = read_escape();
            if (key == KEY_NONE) break;             /* bare Esc cancels */
            scroll_key(key);
        } else if (c == 0x7f || c == 0x08) {
            if (g_tui.input_n) g_tui.input_n = prev_glyph(g_tui.input, g_tui.input_n);
        } else if (c == 0x15) {
            g_tui.input_n = 0;
        } else if (((c >= 0x20 && c < 0x7f) || c >= 0x80)
                   && g_tui.input_n < limit) {
            g_tui.input[g_tui.input_n++] = (char)c;
        }
        g_tui.input_cur = g_tui.input_n;
        repaint();
    }

    size_t n = answered ? g_tui.input_n : 0;
    memcpy(out, g_tui.input, n);
    out[n] = '\0';

    /* A secret has no reason to stay in a buffer the composer keeps for the
     * rest of the session. */
    memset(g_tui.input, 0, g_tui.input_n);
    g_tui.ask = false;
    g_tui.ask_secret = false;
    g_tui.editing = saved_editing;
    memcpy(g_tui.input, saved_input, saved_n);
    g_tui.input_n = saved_n;
    g_tui.input_cur = saved_cur;
    g_tui.comp_n = saved_comp_n;
    memcpy(g_tui.notice, saved_notice, sizeof saved_notice);
    g_tui.notice_n = saved_notice_n;
    repaint();
    return answered;
}

/* What a keystroke asked the caller to do; edits are already applied. */
typedef enum { ED_EDIT = 0, ED_SUBMIT, ED_EOF, ED_REWIND, ED_EXPAND,
               ED_MODE } EdAction;

/* The zone under a mouse cell, 0 outside them all. */
static u32 zone_at_cell(i32 mouse_row, i32 mouse_col) {
    size_t row, col;
    sel_point(mouse_row, mouse_col, &row, &col);
    return zone_at_off(row < TUI_SEL_ROWS ? g_tui.row_src[row] : SIZE_MAX);
}

/* One byte of pasted text. It edits and nothing else: a paste carries no
 * commands, so it neither submits nor completes nor recalls, and the
 * completion list is rebuilt once when the end marker arrives. A tab becomes
 * four spaces because the composer paints its own cells and a terminal tab
 * stop would move the cursor out from under the snapshot. */
static void paste_byte(i32 c) {
    b8 was_cr = g_tui.paste_cr;
    g_tui.paste_cr = c == '\r';
    if (c == '\n' && was_cr) return;   /* CRLF is one line break */

    char run[4];
    size_t run_n = 0;
    if (c == '\r' || c == '\n') run[run_n++] = '\n';
    else if (c == '\t') while (run_n < sizeof run) run[run_n++] = ' ';
    else if ((c >= 0x20 && c < 0x7f) || c >= 0x80) run[run_n++] = (char)c;
    else return;

    char *buf = g_tui.input;
    size_t n = g_tui.input_n, cur = g_tui.input_cur;
    if (n + run_n >= sizeof g_tui.input) return;
    memmove(buf + cur + run_n, buf + cur, n - cur);
    memcpy(buf + cur, run, run_n);
    cur += run_n; n += run_n; buf[n] = '\0';
    g_tui.input_n = n;
    g_tui.input_cur = cur;
}

/* One input byte applied to the shared composer. The caller decides whether a
 * submit is honoured, so the same editor drives the prompt and the
 * keep-typing-while-busy path. */
static EdAction editor_key(i32 c) {
    if (g_tui.pasting && c != 0x1b) { paste_byte(c); return ED_EDIT; }
    char *buf = g_tui.input;
    const size_t cap = sizeof g_tui.input;
    size_t n = g_tui.input_n, cur = g_tui.input_cur;
    EdAction action = ED_EDIT;
    /* Anything but the mouse itself invalidates a highlight, as a keystroke
     * drops the terminal's own selection. */
    b8 keep_sel = false;
    /* A recall is not typing: it must not reopen a dismissed popup. */
    b8 recalled = false;
    /* Only a second Esc rewinds, so the arming is consumed here. */
    b8 was_armed = g_tui.esc_armed;
    g_tui.esc_armed = false;
    if (was_armed) g_tui.notice_n = 0;

    size_t before_n = n;

    /* Keys are only stolen while the popup is open. Tab completes the
     * highlighted entry and stays in the composer; Enter picks it and runs
     * it, so choosing from the popup never costs a second Enter. */
    if (g_tui.comp_n
        && (c == '\t' || c == '\r' || c == '\n' || c == 0x0e || c == 0x10)) {
        sel_clear();
        if (c == 0x0e || c == 0x10) {
            completion_move(c == 0x0e ? 1 : -1);
            return ED_EDIT;
        }
        /* A path is text in a message rather than a command, so accepting one
         * leaves the composer where it is. */
        b8 path = g_tui.path_mode;
        if (completion_would_change()) completion_accept();
        if (path) return ED_EDIT;
        g_tui.comp_n = 0;
        g_tui.comp_sel = 0;
        g_tui.comp_dismissed = true;
        if (c == '\t') return ED_EDIT;
        return ED_SUBMIT;
    }
    if (c == '\r' || c == '\n') { sel_clear(); return ED_SUBMIT; }
    if (c == 0x04) {
        if (n == 0) return ED_EOF;
        if (cur < n) {
            size_t next = next_glyph(buf, n, cur);
            memmove(buf + cur, buf + next, n - next);
            n -= next - cur; buf[n] = '\0';
        }
    } else if (c == 0x01) cur = line_start(buf, cur);
    else if (c == 0x05) cur = line_end(buf, n, cur);
    else if (c == 0x02) cur = prev_glyph(buf, cur);
    else if (c == 0x06) cur = next_glyph(buf, n, cur);
    else if (c == 0x0b) {
        /* On an empty tail, eat the line break itself, so repeated Ctrl-K
         * walks down the composer the way readline does. */
        size_t end = line_end(buf, n, cur);
        if (end == cur && end < n) end++;
        memmove(buf + cur, buf + end, n - end);
        n -= end - cur; buf[n] = '\0';
    } else if (c == 0x15) {
        size_t start = line_start(buf, cur);
        memmove(buf + start, buf + cur, n - cur);
        n -= cur - start; cur = start; buf[n] = '\0';
    } else if (c == 0x17) {
        size_t word = prev_word(buf, cur);
        memmove(buf + word, buf + cur, n - cur);
        n -= cur - word; cur = word; buf[n] = '\0';
    } else if (c == 0x0c) {
        /* An explicit repaint also repairs a terminal after stray output. */
        g_tui.frame_valid = false;
    } else if (c == 0x7f || c == 0x08) {
        if (cur > 0) {
            size_t prev = prev_glyph(buf, cur);
            memmove(buf + prev, buf + cur, n - cur);
            n -= cur - prev; cur = prev; buf[n] = '\0';
        }
    } else if (c == 0x1b) {
        i32 key = read_escape();
        if (key == KEY_LEFT) cur = prev_glyph(buf, cur);
        else if (key == KEY_RIGHT) cur = next_glyph(buf, n, cur);
        else if (key == KEY_HOME) cur = line_start(buf, cur);
        else if (key == KEY_END) cur = line_end(buf, n, cur);
        else if (key == KEY_PREV_WORD) cur = prev_word(buf, cur);
        else if (key == KEY_NEXT_WORD) cur = next_word(buf, n, cur);
        else if (scroll_key(key)) {
            /* The viewport moved; the draft did not. */
        } else if (key == KEY_MOUSE_DOWN) {
            sel_begin(g_mouse_row, g_mouse_col); keep_sel = true;
            g_tui.click_down = zone_at_cell(g_mouse_row, g_mouse_col);
        } else if (key == KEY_MOUSE_DRAG) {
            sel_extend(g_mouse_row, g_mouse_col); keep_sel = true;
        } else if (key == KEY_MOUSE_MOVE) {
            g_tui.hover_id = zone_at_cell(g_mouse_row, g_mouse_col);
            keep_sel = true;
        } else if (key == KEY_MOUSE_UP) {
            /* A click, not a drag: a range being selected is a copy, and the
             * zone it starts in is not what the pointer meant. */
            b8 hit = !g_tui.sel_active && g_tui.click_down
                  && g_tui.click_down == zone_at_cell(g_mouse_row, g_mouse_col);
            if (hit) { g_tui.click_id = g_tui.click_down; action = ED_EXPAND; }
            g_tui.click_down = 0;
            sel_finish(); keep_sel = true;
        } else if (key == KEY_SHIFT_TAB) {
            /* A command rather than an edit, so the draft is left alone. */
            action = ED_MODE;
        } else if (g_tui.comp_n && (key == KEY_DOWN || key == KEY_UP)) {
            completion_move(key == KEY_DOWN ? 1 : -1);
        } else if (key == KEY_UP || key == KEY_DOWN) {
            recalled = history_recall(key == KEY_UP ? -1 : 1, buf, &n, &cur);
        } else if (key == KEY_NONE && g_tui.comp_n) {
            g_tui.comp_dismissed = true;   /* bare Esc closes the popup */
        } else if (key == KEY_NONE && was_armed) {
            action = ED_REWIND;
        } else if (key == KEY_NONE && g_tui.notice_n) {
            g_tui.notice_n = 0;            /* and then the notice above it */
        } else if (key == KEY_NONE && g_tui.busy && g_tui.interrupt) {
            /* Nothing to dismiss and a turn running: Esc cancels it the way
             * Ctrl-C does, without touching the composed text. */
            *g_tui.interrupt = 1;
        } else if (key == KEY_NONE) {
            /* Going back a turn is destructive enough to ask twice. */
            g_tui.esc_armed = true;
            tui_notice(STR("Press Escape again to edit previous message"));
        } else if (key == KEY_NEWLINE && n + 1 < cap) {
            memmove(buf + cur + 1, buf + cur, n - cur);
            buf[cur++] = '\n'; n++; buf[n] = '\0';
        }
    } else if ((c >= 0x20 && c < 0x7f) || c >= 0x80) {
        if (n + 1 < cap) {
            memmove(buf + cur + 1, buf + cur, n - cur);
            buf[cur++] = (char)c; n++; buf[n] = '\0';
        }
    }

    if (!keep_sel) sel_clear();
    g_tui.input_n = n;
    g_tui.input_cur = cur;
    /* Any change to the text reopens a popup an earlier Esc/Tab closed. */
    if (recalled) g_tui.comp_dismissed = true;
    else if (n != before_n) g_tui.comp_dismissed = false;
    completion_refresh();
    return action;
}

/* Called when a line is submitted or abandoned: the notice answered the last
 * command, so the next one retires it. Keystrokes leave it alone. */
static void composer_clear(void) {
    g_tui.notice_n = 0;
    g_tui.esc_armed = false;
    g_tui.input[0] = '\0';
    g_tui.input_n = 0;
    g_tui.input_cur = 0;
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.comp_dismissed = false;
    g_tui.draft_n = 0;
    if (g_tui.hist) history_reset_cursor(g_tui.hist);
}

/* Drain what the terminal already has, without ever blocking. Enter is
 * swallowed, so the composed text stays put until the prompt reopens. */
void tui_poll_input(void) {
    if (!g_tui.fullscreen) return;
    b8 dirty = g_winch != 0;
    while (!g_tui.input_eof && input_ready(0)) {
        i32 c = rbyte();
        if (c == -3) { dirty = true; continue; }
        if (c == -2) continue;
        if (c < 0) { g_tui.input_eof = true; break; }
        /* Enter, Ctrl-C and Ctrl-D belong to the prompt rather than to a live
         * turn, except that an open popup lets Enter complete an entry, which
         * is harmless mid-turn since the submit is dropped. */
        if ((c == '\r' || c == '\n') && !g_tui.comp_n && !g_tui.pasting)
            continue;
        if ((c == 0x03 || c == 0x04) && !g_tui.pasting) continue;
        editor_key(c);
        dirty = true;
    }
    if (dirty) repaint();
}

b8 tui_readline(const char *prompt, char *buf, size_t cap, size_t *out_n) {
    if (!buf || cap == 0) { if (out_n) *out_n = 0; return false; }
    if (!g_tui.tty) {
        if (prompt) { put_str(prompt); flush_out(); }
        if (!fgets(buf, (i32)cap, stdin)) { *out_n = 0; return false; }
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
        buf[n] = '\0'; *out_n = n; return true;
    }

    /* The composer already holds anything typed during the previous turn. */
    g_tui.editing = true;
    tui_set_status("ready");   /* repaints */

    for (;;) {
        i32 c = rbyte();
        if (c == -3) { repaint(); continue; }
        if (c == -2 || (c == 0x03 && !g_tui.pasting)) {
            /* Idle Ctrl-C abandons the draft and nothing else. */
            composer_clear();
            repaint();
            continue;
        }
        if (c < 0) { *out_n = 0; return false; }

        EdAction action = editor_key(c);
        if (action == ED_EOF) { *out_n = 0; return false; }
        if (action == ED_REWIND || action == ED_EXPAND || action == ED_MODE) {
            /* The gesture and the command are one request, so it answers as
             * the command. The draft it was typed over is left alone. */
            char cmd[32];
            i32 len;
            if (action == ED_REWIND) len = snprintf(cmd, sizeof cmd, "/rewind");
            else if (action == ED_MODE) len = snprintf(cmd, sizeof cmd, "/mode");
            else len = snprintf(cmd, sizeof cmd, "/expand %u", g_tui.click_id);
            size_t n = len > 0 ? (size_t)len : 0;
            if (n >= cap) n = cap - 1;
            memcpy(buf, cmd, n);
            buf[n] = '\0';
            *out_n = n;
            repaint();
            return true;
        }
        if (action == ED_SUBMIT) {
            size_t n = g_tui.input_n < cap ? g_tui.input_n : cap - 1;
            memcpy(buf, g_tui.input, n);
            buf[n] = '\0';
            /* Here, so the slash commands the caller consumes are recallable
             * too. */
            if (g_tui.hist) history_add(g_tui.hist, (Str){buf, n});
            composer_clear();
            *out_n = n;
            repaint();
            return true;
        }
        /* With more bytes already queued, painting the intermediate state
         * costs a frame nobody sees. */
        if (!input_ready(0)) repaint();
    }
}
