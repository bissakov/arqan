#include "agent.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define TUI_TRANSCRIPT_CAP (8u << 20)
#define TUI_MAX_ROWS 4096
#define TUI_BODY_GUTTER 2
#define TUI_MIN_COLS 40
#define TUI_MIN_ROWS 12
#define TUI_OUT_CAP (1u << 16)   
#define TUI_SEL_ROWS 512         
#define TUI_SEL_ROW_BYTES 2048   
#define TUI_SEL_BYTES (1u << 16) 
#define TUI_VIEW_BYTES AGENT_RESP_BUF 
#define TUI_VIEW_RUNS YHL_RUN_MAX     
#define TUI_POPUP_ROWS 8         
#define TUI_PICK_NOTICE_ROWS 4   
#define TUI_PICK_SEARCH_MIN 10   
#define TUI_PATH_ENTS 256        
#define TUI_PATH_SLOT 512        
#define TUI_PATH_DEPTH 12        

_Static_assert(TUI_STATUS_N == AGENT_STATUS_FIELDS,
               "status preference mask must cover every field");
#define TUI_PATH_SCAN 20000      
#define TUI_PICK_QUERY 64        
#define TUI_FIND_QUERY 128       
#define TUI_FIND_ROW_HITS 64     
#define TUI_ASK_MAX AGENT_MAX_REASONING_TEMPLATE
/* Markdown claims one span per emphasis run, so this counts words of a reply
 * rather than messages. */
#define TUI_MAX_SPANS 4096
#define TUI_MAX_SYNTAX 16384
#define TUI_CKPTS 4096           
#define TUI_MAX_ZONES 512        
#define TUI_MAX_PINS  512        
#define TUI_MAX_USERS 512        


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

#define S_NOBOLD      "\033[22m"
#define S_ITALIC      "\033[3m"
#define S_MONO        "\033[38;5;180m"
#define S_STRIKE      "\033[9;38;5;245m"
#define S_USER_BG     "\033[48;5;238m"
#define S_CODE_BG     "\033[48;5;235m"
/* A fence inside a user turn stays inside the panel: it takes a shade of its
 * own rather than the assistant's code slab, so the block never breaks. */
#define S_USER_CODE_BG "\033[48;5;236m"

#define S_USER_RULE   "\033[38;5;81m"
#define S_POPUP_BG    "\033[48;5;237m"
#define S_POPUP_SEL   "\033[48;5;24m"
#define S_LINK        "\033[4;38;5;81m"
#define S_LINK_HOVER  "\033[1;4;38;5;81m"

#define S_FIND        "\033[48;5;94m"
#define S_FIND_CUR    "\033[48;5;214m\033[38;5;16m"

typedef struct {
    struct termios original_termios;
    struct sigaction original_winch;
    b8 raw;
    b8 tty;
    b8 fullscreen;
    b8 editing;
    b8 busy;          
    b8 input_eof;     
    b8 color;
    /* Between a bracketed paste's start and end markers every byte is text:
     * a newline in it is a line break in the draft rather than a submit. */
    b8 pasting;
    b8 paste_cr;      
    Str model;
    Str provider;
    Str reasoning_effort;
    Str thinking_budget;
    AgentMode mode;   
    PermissionPolicy permissions;
    Str base_url;     
    /* A modal question owns the composer. Its answer is not a message, so it
     * reaches neither the transcript nor the prompt history. */
    b8 ask;
    b8 ask_secret;
    char cwd_buf[4096];
    Str cwd;
    size_t context_tokens;
    b8 context_known;
    b8 context_exact;
    size_t context_window;
    b8 status_visible[TUI_STATUS_N];
    char status[32];
    
    char activity[48];
    size_t activity_n;
    f64 activity_started;      
    f64 activity_turn;         
    size_t transcript_n;
    /* Newlines written but not committed. The transcript never ends with one,
     * so the air between two blocks is decided in tui_block alone instead of
     * being split between the block that ended and the one that starts. */
    size_t pend_nl;
    size_t trail_nl;   
    b8 wrote_any;      
    size_t scroll_rows;
    
    size_t wrap_cols;        
    size_t wrap_scanned;     
    size_t wrap_rows;        
    size_t ckpt_off[TUI_CKPTS];
    size_t ckpt_n;
    size_t ckpt_step;        
    
    size_t span_a[TUI_MAX_SPANS];
    size_t span_b[TUI_MAX_SPANS];
    u8     span_k[TUI_MAX_SPANS];
    size_t span_head;
    size_t span_n;
    
    size_t syntax_a[TUI_MAX_SYNTAX];
    size_t syntax_b[TUI_MAX_SYNTAX];
    u8     syntax_k[TUI_MAX_SYNTAX];
    size_t syntax_head;
    size_t syntax_n;
    u64    transcript_epoch;
    
    size_t user_a[TUI_MAX_USERS];
    size_t user_b[TUI_MAX_USERS];
    size_t user_n;
    size_t user_open_a;
    b8     user_open;
    /* Byte ranges a click acts on, each carrying the caller's id, kept beside
     * the transcript like spans. */
    size_t zone_a[TUI_MAX_ZONES];
    size_t zone_b[TUI_MAX_ZONES];
    u32    zone_id[TUI_MAX_ZONES];
    size_t zone_n;
    u32    zone_open;        
    size_t zone_open_a;
    u32    click_down;       
    u32    click_id;         
    u32    hover_id;         
    
    size_t pin_off[TUI_MAX_PINS];
    u32    pin_id[TUI_MAX_PINS];
    size_t pin_n;
    
    u32    anchor_id;
    b8     anchor_is_pin;    
    size_t anchor_below;
    size_t anchor_scroll;
    
    volatile sig_atomic_t *interrupt;
    f64 last_paint;
    size_t painted_rows;
    size_t painted_cols;
    u64 row_hash[TUI_MAX_ROWS];
    b8 frame_valid;
    b8 size_warning;
    size_t bar_first, bar_total, bar_visible;
    b8 bar_valid;
    /* The composer outlives a single tui_readline: text typed while a turn is
     * running is still here when the next prompt opens. */
    size_t input_n;
    size_t input_cur;
    
    size_t queued_n;
    /* Vertical motion inside the composer. `input_top` is the visual row the
     * composer window starts at, kept across frames so a tall draft scrolls
     * by the row the cursor left rather than snapping to the caret; the goal
     * column holds the cell a run of Up/Down aims for, so crossing a short
     * row does not shorten the ones after it. */
    size_t input_top;
    size_t goal_col;
    b8 goal_col_valid;
    size_t kill_n;
    const TuiCmd *cmds;
    const TuiMark *marks;   
    size_t cmd_n;
    const TuiAlias *aliases;
    size_t alias_n;
    u16 comp_idx[AGENT_MAX_POPUP];      
    /* The popup also completes a filesystem path: while `path_mode` is set it
     * is offering these entries instead of the command table, listed from the
     * directory the word at the cursor names. */
    b8 path_mode;
    TuiCmd path_ents[TUI_PATH_ENTS];
    char path_slot[TUI_PATH_ENTS][TUI_PATH_SLOT];
    u8 path_rank[TUI_PATH_ENTS];
    u16 path_depth[TUI_PATH_ENTS];
    u16 path_ord[TUI_PATH_ENTS];     
    size_t path_n;
    size_t path_at;                  
    
    size_t attach_n;
    
    b8 justify;
    AgentIgnore ignore;              
    size_t comp_n;
    size_t comp_sel;
    b8 pick_end;                     
    b8 comp_dismissed;               
    
    b8 esc_armed;
    
    char notice[160];
    size_t notice_n;
    
    char pick_notice[512];
    size_t pick_notice_n;
    /* A modal picker owns the frame until it is answered, so the popup is
     * given the room the composer's own completions do not need. */
    b8 picking;
    /* The transcript block the open modal screen is asking about, SIZE_MAX
     * for none. Set only while that screen is up, and the frame lifts the
     * transcript out from under it rather than covering the block. */
    size_t keep_off;
    /* What a run with nothing to talk to is missing, empty when it wants for
     * nothing. Not owned: the caller passes a literal. */
    Str setup_hint;
    
    History *hist;
    size_t draft_n;
    
    b8 hist_nav;
    
    u16 row_text_n[TUI_SEL_ROWS];
    u16 row_text_w[TUI_SEL_ROWS];
    
    size_t row_src[TUI_SEL_ROWS];
    
    b8 find_open;            // the box owns the keyboard
    void (*find_expand)(void *ud);   
    void *find_expand_ud;
    b8 find_wrapped;         
    char find_q[TUI_FIND_QUERY];
    size_t find_q_n;
    size_t find_cur;         
    size_t find_count;
    size_t find_index;       
    size_t find_scanned;     
    u64    find_epoch;       
    
    size_t view_first_off;
    size_t view_end_off;
    b8 sel_active;    
    b8 sel_drag;      
    size_t sel_ar, sel_ac;   
    size_t sel_br, sel_bc;   
    char sel_text[TUI_SEL_BYTES];
    f64 copy_notice;  
    b8 tmux_copy_hinted;  
    char out[TUI_OUT_CAP];
    size_t out_n;
} TuiState;

/* The bulk payload buffers, held apart from TuiState because every one of
 * them is read only up to a counter that lives there. Their bytes therefore
 * carry no state of their own, so tui_start resets the control block alone
 * and these pages stay untouched until something is actually written into
 * them: a session that never fills the scrollback never pays for it. */
typedef struct {
    char transcript[TUI_TRANSCRIPT_CAP];   
    char input[AGENT_LINE_BUF];             
    char queued[AGENT_LINE_BUF];            
    char draft[AGENT_LINE_BUF];             
    char kill[AGENT_LINE_BUF];              
    char attach[AGENT_MAX_PATH];            
    char view[TUI_VIEW_BYTES];               
    
    u32  view_syn_a[TUI_VIEW_RUNS];
    u32  view_syn_b[TUI_VIEW_RUNS];
    u8   view_syn_k[TUI_VIEW_RUNS];
    char row_text[TUI_SEL_ROWS][TUI_SEL_ROW_BYTES];  
} TuiBulk;

typedef struct {
    b8 batch;
} TuiPaint;

static TuiPaint g_paint;

static TuiState g_tui;
static TuiBulk g_bulk;
static volatile sig_atomic_t g_winch = 0;

typedef struct {
    b8 active;
    b8 modal;
    b8 close_down;
    char title[128];
    size_t title_n;
    size_t text_n;
    size_t syn_n;
    size_t start_line;
    size_t top;
    size_t wrap_cols;
    size_t total_rows;
    size_t ckpt_off[TUI_CKPTS];
    size_t ckpt_n;
    size_t ckpt_step;
    size_t top_row, left_col, right_col, bottom_row;
    b8 paint;
} View;

static View g_view;
static b8 view_locks_row(size_t row) {
    return g_view.active && !g_view.paint
        && row >= g_view.top_row && row <= g_view.bottom_row;
}


static void view_unlock(void) {
    g_view.top_row = 1;
    g_view.bottom_row = 0;
}

static void on_winch(i32 sig) { (void)sig; g_winch = 1; }


static void flush_out(void) {
    size_t off = 0;
    while (off < g_tui.out_n) {
        ssize_t w = write(STDOUT_FILENO, g_tui.out + off, g_tui.out_n - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        break;   // closed or full pipe: drop the frame rather than spin
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


static void frame_begin(void) { put_str("\033[?2026h"); }

static void frame_end(void) {
    put_str("\033[?2026l");
    flush_out();
}

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

static void terminal_size(size_t *rows, size_t *cols) {
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *rows = (size_t)ws.ws_row;
        *cols = (size_t)ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

static void screen_size(size_t *rows, size_t *cols) {
    terminal_size(rows, cols);
    if (*rows == 0) *rows = 24;
    if (*cols == 0) *cols = 80;
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


static size_t glyph(const char *s, size_t n, i32 *width) {
    if (n == 0) { *width = 0; return 0; }
    u8 c = (u8)s[0];
    if (c < 0x80) { *width = 1; return 1; }
    u32 cp = 0;
    size_t used = utf8_decode(s, n, &cp);
    /* Bytes the terminal will print as something take a cell each: a
     * malformed sequence advances one byte rather than stalling the walk. */
    if (used == 0) { *width = 1; return 1; }
    i32 w = agent_width(cp);
    
    if (w == 0 && cp < 0xA0) w = 1;
    *width = w;
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

static struct {
    size_t row;   
    size_t col;   
} g_cap;

/* Byte offset of the first glyph at or after `cell`; *reached is the column
 * landed on, since a wide glyph can straddle the requested one. */
static size_t row_byte_at(size_t r, size_t cell, size_t *reached) {
    const char *p = g_bulk.row_text[r];
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
    g_cap.row = row && row <= TUI_SEL_ROWS ? row : 0;
    g_cap.col = col ? col - 1 : 0;
    if (!g_cap.row) return;
    size_t r = g_cap.row - 1;
    if (g_cap.col < g_tui.row_text_w[r]) {
        size_t reached = 0;
        size_t bytes = row_byte_at(r, g_cap.col, &reached);
        g_tui.row_text_n[r] = (u16)bytes;
        g_tui.row_text_w[r] = (u16)reached;
    }
    while (g_tui.row_text_w[r] < g_cap.col
           && (size_t)g_tui.row_text_n[r] + 1 < TUI_SEL_ROW_BYTES) {
        g_bulk.row_text[r][g_tui.row_text_n[r]++] = ' ';
        g_tui.row_text_w[r]++;
    }
}

static void snap_put(const char *s, size_t used, size_t width) {
    if (g_cap.row) {
        size_t r = g_cap.row - 1;
        if ((size_t)g_tui.row_text_n[r] + used < TUI_SEL_ROW_BYTES
            && (size_t)g_tui.row_text_w[r] + width < 0xffffu) {
            memcpy(g_bulk.row_text[r] + g_tui.row_text_n[r], s, used);
            g_tui.row_text_n[r] = (u16)((size_t)g_tui.row_text_n[r] + used);
            g_tui.row_text_w[r] = (u16)((size_t)g_tui.row_text_w[r] + width);
        }
    }
    g_cap.col += width;
}


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
    *c1 = r == r1 ? s1 : (size_t)-1;   
}

static void put_text(const char *s, size_t n) {
    size_t sel_c0 = 0, sel_c1 = 0;
    sel_row_range(g_cap.row, &sel_c0, &sel_c1);
    /* Styles are re-emitted between calls and can drop reverse video, so the
     * flag is per call rather than per row. */
    b8 reverse = false;
    for (size_t i = 0; i < n;) {
        i32 width = 0;
        size_t used = glyph(s + i, n - i, &width);
        b8 selected = g_cap.col >= sel_c0 && g_cap.col < sel_c1;
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
    size_t start = g_cap.col > c0 ? g_cap.col : c0;
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
        while (b > a && g_bulk.row_text[r][b - 1] == ' ') b--;  
        for (size_t i = a; i < b && n + 1 < cap; i++)
            out[n++] = g_bulk.row_text[r][i];
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

b8 tui_clipboard_via_tmux(void) { return getenv("TMUX") != NULL; }

/* The clipboard is written blind: OSC 52 carries no reply, so nothing here
 * learns whether it landed. Under tmux the odds are worth saying out loud,
 * once per run, since a drag-select is a gesture the user repeats. */
static void copy_acknowledge(void) {
    g_tui.copy_notice = agent_now_seconds() + 2.0;
    if (!tui_clipboard_via_tmux() || g_tui.tmux_copy_hinted) return;
    g_tui.tmux_copy_hinted = true;
    tui_notice(AGENT_TMUX_COPY_NOTICE);
}


static void sel_copy(void) {
    size_t n = sel_extract(g_tui.sel_text, sizeof g_tui.sel_text);
    if (!n) return;
    put_str("\033]52;c;");
    b64_put((const u8 *)g_tui.sel_text, n);
    put_str("\a");
    flush_out();
    copy_acknowledge();
}

static void sel_clear(void) {
    if (!g_tui.sel_active && !g_tui.sel_drag) return;
    g_tui.sel_active = false;
    g_tui.sel_drag = false;
    g_tui.bar_valid = false;   
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
    g_tui.sel_active = false;   
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

/* ---- line breaking -------------------------------------------------------
 * One breaker feeds the row index, the row painter and the composer's cursor
 * arithmetic, so the three can never disagree about where a row ends.
 *
 * A break happens between words: `end` is the last byte painted and `next` is
 * where the following row starts, and the spaces between them belong to
 * neither, since a row that begins on the space its predecessor broke at is
 * indented by an accident of the width. A word wider than the line still
 * breaks inside itself, since the alternative is a row that cannot be shown.
 */
typedef struct {
    size_t end;    
    size_t next;   
    size_t width;  
    b8 hard;       
} Row;

static size_t skip_spaces(Str s, size_t i) {
    while (i < s.n && s.p[i] == ' ') i++;
    return i;
}

static Row row_break(Str s, size_t from, size_t cols, size_t col0) {
    size_t col = col0;
    size_t sp = SIZE_MAX;      
    size_t sp_col = 0;
    b8 word = false;           
    Row brk = {0};
    b8 have_brk = false;
    for (size_t i = from; i < s.n;) {
        if (s.p[i] == '\n') return (Row){ i, i + 1, col - col0, true };
        i32 gw = 0;
        size_t used = glyph(s.p + i, s.n - i, &gw);
        size_t w = gw > 0 ? (size_t)gw : 0;
        
        if (w && col > 0 && col + w > cols && i > from) {
            if (s.p[i] == ' ') {
                size_t at = sp == SIZE_MAX ? i : sp;
                size_t at_col = sp == SIZE_MAX ? col : sp_col;
                return (Row){ at, skip_spaces(s, at), at_col - col0, false };
            }
            if (have_brk) return brk;
            return (Row){ i, i, col - col0, false };
        }
        if (s.p[i] == ' ') {
            if (sp == SIZE_MAX) { sp = i; sp_col = col; }
        } else {
            if (sp != SIZE_MAX && word) {
                brk = (Row){ sp, i, sp_col - col0, false };
                have_brk = true;
            }
            sp = SIZE_MAX;
            word = true;
        }
        i += used;
        col += w;
    }
    return (Row){ s.n, s.n, col - col0, true };
}


static size_t text_rows(Str s, size_t cols, size_t prompt_cells,
                        size_t cursor, size_t *cursor_row, size_t *cursor_col) {
    size_t row = 0, col0 = prompt_cells;
    for (size_t i = 0;;) {
        Row r = row_break(s, i, cols, col0);
        if (cursor_row && cursor >= i && cursor <= r.end) {
            *cursor_row = row;
            size_t at = col0;
            for (size_t j = i; j < cursor;) {
                i32 w = 0;
                size_t used = glyph(s.p + j, s.n - j, &w);
                j += used;
                at += w > 0 ? (size_t)w : 0;
            }
            *cursor_col = at;
        } else if (cursor_row && cursor > r.end && cursor < r.next) {
            *cursor_row = row;
            *cursor_col = col0 + r.width;
        }
        if (r.hard && r.end >= s.n) break;
        i = r.next;
        row++;
    }
    return row + 1;
}


static size_t row_col_off(Str s, size_t cols, size_t prompt_cells,
                          size_t want_row, size_t want_col) {
    size_t row = 0, col0 = prompt_cells;
    for (size_t i = 0;;) {
        Row r = row_break(s, i, cols, col0);
        if (row == want_row) {
            size_t at = col0, j = i;
            while (j < r.end) {
                i32 w = 0;
                size_t used = glyph(s.p + j, s.n - j, &w);
                size_t cw = w > 0 ? (size_t)w : 0;
                if (at + cw > want_col) break;
                at += cw;
                j += used;
            }
            return j;
        }
        if (r.hard && r.end >= s.n) return s.n;
        i = r.next;
        row++;
    }
}

/* Bytes painted, which is fewer than `s` holds when a glyph no longer fits:
 * a caller painting one row in several styles stops where this one did. */
static size_t put_safe_clipped(Str s, size_t max_cells, size_t *used_cells) {
    size_t cells = 0;
    size_t i = 0;
    for (; i < s.n;) {
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
    return i;
}

static void put_status_field(Str field, const char *field_style,
                             size_t body_cols, size_t *used, b8 *have_field) {
    if (!field.n || *used >= body_cols) return;
    if (*have_field) {
        if (body_cols - *used <= 3) return;
        style(S_MUTED);
        put_safe_clipped(STR(" · "), body_cols - *used, used);
    }
    style(field_style);
    put_safe_clipped(field, body_cols - *used, used);
    *have_field = true;
}

static void pad_row(size_t used, size_t cols) {
    while (used++ < cols) put_raw(" ", 1);
}

enum {
    ROW_PLAIN = 1, ROW_COMPOSER, ROW_STATUS,
    ROW_USER, ROW_REASON, ROW_TOOL, ROW_RESULT, ROW_ERROR, ROW_NOTICE,
    ROW_POPUP, ROW_WELCOME_ART, ROW_WELCOME_TEXT,
    ROW_HEADING, ROW_CODE, ROW_QUOTE,      
    ROW_ZONE, ROW_ZONE_HOVER,              
    ROW_SOURCE,                            
    ROW_BOLD, ROW_EMPH, ROW_MONO, ROW_MARKER, ROW_STRIKE
};

static b8 kind_is_block(u8 kind) { return kind && kind < ROW_BOLD; }


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
        /* Wrapped composer rows are painted from a reset, so they name the
         * panel's text colour rather than inherit the terminal's. */
        case ROW_COMPOSER:     return S_PANEL_BG S_TEXT;
        case ROW_ZONE:         return S_LINK;
        case ROW_ZONE_HOVER:   return S_POPUP_BG S_LINK_HOVER;
        case ROW_QUOTE:        return S_MUTED;
        
        case ROW_SOURCE:       return S_TEXT;
        case ROW_BOLD:         return S_BOLD S_TEXT;
        case ROW_EMPH:         return S_ITALIC S_MUTED;
        case ROW_MONO:         return S_MONO;
        case ROW_MARKER:       return S_BLUE;
        case ROW_STRIKE:       return S_STRIKE;
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
    
    if (kind == ROW_COMPOSER) {
        h = hash_add(h, &g_tui.busy, sizeof g_tui.busy);
        h = hash_add(h, &g_tui.ask, sizeof g_tui.ask);
    }
    h = hash_add(h, prefix.p, prefix.n);
    return hash_add(h, text.p, text.n);
}

static b8 row_changed(size_t row, u64 hash, b8 force) {
    if (view_locks_row(row)) return false;
    size_t index = row - 1;
    if (!force && g_tui.row_hash[index] == hash) return false;
    g_tui.row_hash[index] = hash;
    return true;
}

static char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

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

/* Every byte of `needle` in order, gaps allowed: "shins" finds "Show
 * instructions". A row is found by what it is called rather than by how it
 * is spelled, which is what a settings screen is searched for. */
static b8 str_fuzzy_ci(Str s, Str needle) {
    size_t at = 0;
    for (size_t i = 0; i < s.n && at < needle.n; i++)
        if (lower_ascii(s.p[i]) == lower_ascii(needle.p[at])) at++;
    return at == needle.n;
}


static size_t span_slot(size_t i) {
    return (g_tui.span_head + i) % TUI_MAX_SPANS;
}

static size_t syntax_slot(size_t i) {
    return (g_tui.syntax_head + i) % TUI_MAX_SYNTAX;
}

/* Reasoning arrives as many tiny deltas, so an append that continues the
 * previous span extends it rather than claiming a slot. */
static void span_add(size_t a, size_t b, u8 kind) {
    size_t last = g_tui.span_n ? span_slot(g_tui.span_n - 1) : 0;
    if (g_tui.span_n && g_tui.span_k[last] == kind
        && g_tui.span_b[last] == a) {
        g_tui.span_b[last] = b;
        return;
    }
    if (g_tui.span_n == TUI_MAX_SPANS) {
        g_tui.span_head = (g_tui.span_head + 1) % TUI_MAX_SPANS;
        g_tui.span_n--;
    }
    size_t slot = span_slot(g_tui.span_n);
    g_tui.span_a[slot] = a;
    g_tui.span_b[slot] = b;
    g_tui.span_k[slot] = kind;
    g_tui.span_n++;
}

static void spans_shift(size_t delta) {
    size_t drop = 0;
    while (drop < g_tui.span_n
           && g_tui.span_b[span_slot(drop)] <= delta) drop++;
    g_tui.span_head = span_slot(drop);
    g_tui.span_n -= drop;
    for (size_t i = 0; i < g_tui.span_n; i++) {
        size_t slot = span_slot(i);
        g_tui.span_a[slot] = g_tui.span_a[slot] > delta
                           ? g_tui.span_a[slot] - delta : 0;
        g_tui.span_b[slot] -= delta;
    }
}

static void syntax_shift(size_t delta) {
    size_t drop = 0;
    while (drop < g_tui.syntax_n
           && g_tui.syntax_b[syntax_slot(drop)] <= delta) drop++;
    g_tui.syntax_head = syntax_slot(drop);
    g_tui.syntax_n -= drop;
    for (size_t i = 0; i < g_tui.syntax_n; i++) {
        size_t slot = syntax_slot(i);
        g_tui.syntax_a[slot] = g_tui.syntax_a[slot] > delta
                             ? g_tui.syntax_a[slot] - delta : 0;
        g_tui.syntax_b[slot] -= delta;
    }
}

static void user_add(size_t a, size_t b) {
    if (a >= b) return;
    if (g_tui.user_n == TUI_MAX_USERS) {
        memmove(g_tui.user_a, g_tui.user_a + 1,
                sizeof g_tui.user_a - sizeof g_tui.user_a[0]);
        memmove(g_tui.user_b, g_tui.user_b + 1,
                sizeof g_tui.user_b - sizeof g_tui.user_b[0]);
        g_tui.user_n--;
    }
    g_tui.user_a[g_tui.user_n] = a;
    g_tui.user_b[g_tui.user_n] = b;
    g_tui.user_n++;
}

static void users_shift(size_t delta) {
    size_t w = 0;
    for (size_t i = 0; i < g_tui.user_n; i++) {
        if (g_tui.user_b[i] <= delta) continue;
        g_tui.user_a[w] = g_tui.user_a[i] > delta
                        ? g_tui.user_a[i] - delta : 0;
        g_tui.user_b[w] = g_tui.user_b[i] - delta;
        w++;
    }
    g_tui.user_n = w;
}

static b8 user_at_off(size_t off) {
    size_t lo = 0, hi = g_tui.user_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_tui.user_b[mid] <= off) lo = mid + 1; else hi = mid;
    }
    return lo < g_tui.user_n && off >= g_tui.user_a[lo];
}


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


static void pin_add(size_t off, u32 id) {
    if (g_tui.pin_n == TUI_MAX_PINS) {
        memmove(g_tui.pin_off, g_tui.pin_off + 1,
                sizeof g_tui.pin_off - sizeof g_tui.pin_off[0]);
        memmove(g_tui.pin_id, g_tui.pin_id + 1,
                sizeof g_tui.pin_id - sizeof g_tui.pin_id[0]);
        g_tui.pin_n--;
    }
    g_tui.pin_off[g_tui.pin_n] = off;
    g_tui.pin_id[g_tui.pin_n] = id;
    g_tui.pin_n++;
}

static void pins_shift(size_t delta) {
    size_t w = 0;
    for (size_t i = 0; i < g_tui.pin_n; i++) {
        if (g_tui.pin_off[i] < delta) continue;
        g_tui.pin_off[w] = g_tui.pin_off[i] - delta;
        g_tui.pin_id[w] = g_tui.pin_id[i];
        w++;
    }
    g_tui.pin_n = w;
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


static u32 zone_at_off(size_t off) {
    if (off == SIZE_MAX) return 0;
    size_t lo = 0, hi = g_tui.zone_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_tui.zone_b[mid] <= off) lo = mid + 1; else hi = mid;
    }
    return lo < g_tui.zone_n && off >= g_tui.zone_a[lo] ? g_tui.zone_id[lo] : 0;
}

static size_t span_first(size_t off) {
    size_t lo = 0, hi = g_tui.span_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_tui.span_b[span_slot(mid)] <= off) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static u8 span_kind(size_t off) {
    size_t i = span_first(off);
    if (i == g_tui.span_n) return 0;
    size_t slot = span_slot(i);
    return off >= g_tui.span_a[slot] ? g_tui.span_k[slot] : 0;
}

static u8 span_run(size_t off, size_t limit, size_t *end) {
    size_t i = span_first(off);
    u8 kind = 0;
    size_t stop = limit;
    if (i < g_tui.span_n) {
        size_t slot = span_slot(i);
        if (off >= g_tui.span_a[slot]) {
            kind = g_tui.span_k[slot];
            if (g_tui.span_b[slot] < stop) stop = g_tui.span_b[slot];
        } else if (g_tui.span_a[slot] < stop) {
            stop = g_tui.span_a[slot];
        }
    }
    *end = stop;
    return kind;
}


static u64 hash_spans(u64 h, size_t off, size_t n) {
    for (size_t i = span_first(off);
         i < g_tui.span_n; i++) {
        size_t slot = span_slot(i);
        if (g_tui.span_a[slot] >= off + n) break;
        h = hash_add(h, &g_tui.span_a[slot], sizeof g_tui.span_a[slot]);
        h = hash_add(h, &g_tui.span_b[slot], sizeof g_tui.span_b[slot]);
        h = hash_add(h, &g_tui.span_k[slot], sizeof g_tui.span_k[slot]);
    }
    return h;
}

static size_t syntax_first(size_t off) {
    size_t lo = 0, hi = g_tui.syntax_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_tui.syntax_b[syntax_slot(mid)] <= off) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static u8 syntax_run(size_t off, size_t limit, size_t *end) {
    size_t i = syntax_first(off);
    u8 kind = 0;
    size_t stop = limit;
    if (i < g_tui.syntax_n) {
        size_t slot = syntax_slot(i);
        if (off >= g_tui.syntax_a[slot]) {
            kind = g_tui.syntax_k[slot];
            if (g_tui.syntax_b[slot] < stop) stop = g_tui.syntax_b[slot];
        } else if (g_tui.syntax_a[slot] < stop) {
            stop = g_tui.syntax_a[slot];
        }
    }
    *end = stop;
    return kind;
}

static u64 hash_syntax(u64 h, size_t off, size_t n) {
    for (size_t i = syntax_first(off);
         i < g_tui.syntax_n; i++) {
        size_t slot = syntax_slot(i);
        if (g_tui.syntax_a[slot] >= off + n) break;
        h = hash_add(h, &g_tui.syntax_a[slot], sizeof g_tui.syntax_a[slot]);
        h = hash_add(h, &g_tui.syntax_b[slot], sizeof g_tui.syntax_b[slot]);
        h = hash_add(h, &g_tui.syntax_k[slot], sizeof g_tui.syntax_k[slot]);
    }
    return h;
}


typedef struct {
    size_t gaps;    
    size_t extra;   
    size_t seen;    
    b8 word;        
    b8 pending;     
} Just;

/* Space runs with a word on each side; the indent a row opens with is not
 * one, since widening it would move the block's left edge. */
static size_t row_gaps(Str text) {
    size_t gaps = 0;
    b8 word = false, in_sp = false;
    for (size_t i = 0; i < text.n; i++) {
        if (text.p[i] == ' ') { if (word) in_sp = true; }
        else { if (in_sp) gaps++; in_sp = false; word = true; }
    }
    return gaps;
}

/* The cells are spread across the row rather than handed to the first gaps
 * that ask, so a row two cells short widens two gaps a word apart instead of
 * opening a hole after its first word. */
static void just_gap(Just *j) {
    size_t take = (j->seen + 1) * j->extra / j->gaps
                - j->seen * j->extra / j->gaps;
    j->seen++;
    while (take--) put_text(" ", 1);
}

/* `j` carries the row's state across fragments, since a style run may end
 * inside the space run a gap is opened at. */
static void put_just(const char *p, size_t n, Just *j) {
    if (!j || !j->gaps) { put_text(p, n); return; }
    for (size_t i = 0; i < n;) {
        size_t k = i;
        if (p[i] == ' ') {
            while (k < n && p[k] == ' ') k++;
            put_text(p + i, k - i);
            j->pending = j->word;
        } else {
            while (k < n && p[k] != ' ') k++;
            if (j->pending && j->seen < j->gaps) just_gap(j);
            j->pending = false;
            put_text(p + i, k - i);
            j->word = true;
        }
        i = k;
    }
}

static const char *syntax_style(u8 kind) {
    switch (kind) {
        case YHL_SEM_COMMENT:  return S_MUTED S_ITALIC;
        case YHL_SEM_STRING:   return "\033[38;5;114m";
        case YHL_SEM_NUMBER:   return "\033[38;5;221m";
        case YHL_SEM_KEYWORD:  return "\033[38;5;177m";
        case YHL_SEM_TYPE:     return "\033[38;5;81m";
        case YHL_SEM_FUNCTION: return "\033[38;5;75m";
        case YHL_SEM_BUILTIN:  return S_MONO;
        default:               return NULL;
    }
}

/* ---- transcript search ---------------------------------------------------
 * Matching is ASCII case-insensitive: bytes above 0x7f compare exactly, so a
 * UTF-8 query is matched literally rather than half-folded. The query is held
 * as it was typed, since the box shows it back.
 */
static u8 fold(u8 c) { return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c; }

static b8 find_match_at(size_t off) {
    size_t n = g_tui.find_q_n;
    if (!n || g_tui.transcript_n < n || off > g_tui.transcript_n - n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (fold((u8)g_bulk.transcript[off + i]) != fold((u8)g_tui.find_q[i]))
            return false;
    return true;
}

/* First match starting at or after `from` and before `until`, SIZE_MAX for
 * none. The first byte drives a memchr in both its cases, so a scan of the
 * whole scrollback costs a pair of word-at-a-time passes rather than a
 * comparison per byte, and a caller that cares about one row pays for that
 * row rather than for everything after it.
 *
 * The upper-case pass stops at the lower-case hit rather than running to the
 * end of the range: a letter that never appears in that case would otherwise
 * re-scan the whole scrollback once per match, which is the difference
 * between a keystroke and a hang on a transcript of any size. */
static size_t find_next_in(size_t from, size_t until) {
    size_t n = g_tui.find_q_n;
    if (!n || g_tui.transcript_n < n) return SIZE_MAX;
    size_t last = g_tui.transcript_n - n;
    if (until && until - 1 < last) last = until - 1;
    u8 lo = fold((u8)g_tui.find_q[0]);
    u8 up = lo >= 'a' && lo <= 'z' ? (u8)(lo - 32) : lo;
    for (size_t i = from; i <= last;) {
        size_t span = last - i + 1;
        const char *base = g_bulk.transcript + i;
        const char *a = memchr(base, lo, span);
        size_t before = a ? (size_t)(a - base) : span;
        const char *b = up != lo ? memchr(base, up, before) : NULL;
        const char *hit = b ? b : a;
        if (!hit) return SIZE_MAX;
        i = (size_t)(hit - g_bulk.transcript);
        if (find_match_at(i)) return i;
        i++;
    }
    return SIZE_MAX;
}

static size_t find_next_at(size_t from) {
    return find_next_in(from, g_tui.transcript_n);
}


#define FIND_BACK_WINDOW (64u << 10)

static size_t find_prev_before(size_t limit) {
    size_t q = g_tui.find_q_n;
    if (!q || !limit) return SIZE_MAX;
    for (size_t hi = limit;;) {
        size_t lo = hi > FIND_BACK_WINDOW ? hi - FIND_BACK_WINDOW : 0;
        size_t from = lo > q - 1 ? lo - (q - 1) : 0;
        size_t best = SIZE_MAX;
        for (size_t i = find_next_in(from, hi); i != SIZE_MAX;
             i = find_next_in(i + 1, hi))
            best = i;
        if (best != SIZE_MAX) return best;
        if (!lo) return SIZE_MAX;
        hi = lo;
    }
}

static size_t find_count_from(size_t from) {
    size_t n = 0;
    for (size_t i = find_next_at(from); i != SIZE_MAX; i = find_next_at(i + 1))
        n++;
    return n;
}


static void find_shift(size_t delta) {
    if (g_tui.find_cur != SIZE_MAX)
        g_tui.find_cur = g_tui.find_cur >= delta ? g_tui.find_cur - delta
                                                 : SIZE_MAX;
    g_tui.find_scanned = SIZE_MAX;
}

static void find_invalidate(void) {
    g_tui.find_cur = SIZE_MAX;
    g_tui.find_index = 0;
    g_tui.find_count = 0;
    g_tui.find_scanned = SIZE_MAX;
}


static struct {
    size_t a[TUI_FIND_ROW_HITS];
    size_t b[TUI_FIND_ROW_HITS];
    b8     cur[TUI_FIND_ROW_HITS];
    size_t n;
    b8     moving;
} g_find;

static void find_row_build(size_t off, size_t len) {
    g_find.n = 0;
    size_t q = g_tui.find_q_n;
    if (!q || off == SIZE_MAX) return;
    
    size_t from = off > q - 1 ? off - (q - 1) : 0;
    size_t stop = off + len;
    for (size_t i = find_next_in(from, stop);
         i != SIZE_MAX && g_find.n < TUI_FIND_ROW_HITS;
         i = find_next_in(i + 1, stop)) {
        if (i + q <= off) continue;
        size_t a = i > off ? i - off : 0;
        size_t b = i + q < stop ? i + q - off : len;
        size_t k = g_find.n;
        
        if (k && g_find.b[k - 1] >= a) {
            if (b > g_find.b[k - 1]) g_find.b[k - 1] = b;
            if (i == g_tui.find_cur) g_find.cur[k - 1] = true;
            continue;
        }
        g_find.a[k] = a;
        g_find.b[k] = b;
        g_find.cur[k] = i == g_tui.find_cur;
        g_find.n = k + 1;
    }
}


static b8 find_row_run(size_t i, size_t max, size_t *len, b8 *current) {
    *len = max;
    *current = false;
    for (size_t k = 0; k < g_find.n; k++) {
        if (i < g_find.a[k]) {
            if (g_find.a[k] - i < *len) *len = g_find.a[k] - i;
            return false;
        }
        if (i < g_find.b[k]) {
            if (g_find.b[k] - i < *len) *len = g_find.b[k] - i;
            *current = g_find.cur[k];
            return true;
        }
    }
    return false;
}

static u64 hash_find_row(u64 h) {
    h = hash_add(h, &g_find.n, sizeof g_find.n);
    for (size_t i = 0; i < g_find.n; i++) {
        h = hash_add(h, &g_find.a[i], sizeof g_find.a[i]);
        h = hash_add(h, &g_find.b[i], sizeof g_find.b[i]);
        h = hash_add(h, &g_find.cur[i], sizeof g_find.cur[i]);
    }
    return h;
}

/* `p` starts at row byte `rel`. The base style is re-emitted after every
 * highlighted stretch, since the background is layered over it rather than
 * replacing it. */
static void put_hits(const char *p, size_t rel, size_t n, Just *j,
                     void (*restore)(void *ud), void *ud) {
    for (size_t k = 0; k < n;) {
        size_t seg = 0;
        b8 cur = false;
        b8 hit = find_row_run(rel + k, n - k, &seg, &cur);
        if (restore) restore(ud);
        if (hit) style(cur ? S_FIND_CUR : S_FIND);
        put_just(p + k, seg, j);
        k += seg;
    }
}

typedef struct { u8 kind, base, syntax; b8 user; } RunStyle;


static const char *user_bg(u8 kind) {
    return kind == ROW_CODE ? S_USER_CODE_BG : S_USER_BG;
}


static void paint_user_rule(size_t screen_row, size_t screen_col) {
    if (screen_col < 2) return;
    cup(screen_row, screen_col > 2 ? screen_col - 2 : 1);
    style(S_USER_RULE);
    put_str("▌");
}

static void run_style(void *ud) {
    const RunStyle *r = ud;
    style(S_RESET);
    style(kind_style(r->kind ? r->kind : r->base));
    
    if (r->user) style(user_bg(r->base));
    if (r->syntax) style(syntax_style(r->syntax));
}

static void paint_runs(Str text, size_t off, Just *j, u8 base, b8 user) {
    for (size_t i = 0; i < text.n;) {
        size_t end = 0, syntax_end = 0;
        u8 kind = span_run(off + i, off + text.n, &end);
        u8 syntax = syntax_run(off + i, off + text.n, &syntax_end);
        if (syntax_end < end) end = syntax_end;
        size_t take = end - (off + i);
        RunStyle rs = { kind, base, syntax, user };
        run_style(&rs);
        if (!g_find.n) put_just(text.p + i, take, j);
        else put_hits(text.p + i, i, take, j, run_style, &rs);
        i += take;
    }
}

static u8 display_kind(u8 kind, Str text) {
    if (kind != ROW_PLAIN) return kind;
    if (text.n >= 2 && text.p[0] == '[' && text.p[text.n - 1] == ']')
        return ROW_NOTICE;
    return kind;
}


static Str prompt_indent(size_t cells) {
    static const char blanks[] = "        ";
    if (cells > sizeof blanks - 1) cells = sizeof blanks - 1;
    return (Str){ blanks, cells };
}

/* Whether a composer prefix is the marker itself rather than the indent that
 * stands in for it on the rows below. */
static b8 is_marker(Str prefix) {
    return prefix.n != 0 && prefix.p[0] != ' ';
}


static void update_text_row(size_t screen_row, Str prefix, Str text,
                            size_t screen_col, size_t screen_cols,
                            u8 kind, size_t text_off, size_t pad, b8 force) {
    kind = display_kind(kind, text);
    b8 user = text_off != SIZE_MAX && user_at_off(text_off);
    Just just = { pad ? row_gaps(text) : 0, pad, 0, false, false };
    
    find_row_build(text_off, text.n);
    u64 hash = row_hash(prefix, text, kind);
    hash = hash_find_row(hash);
    hash = hash_add(hash, &user, sizeof user);
    hash = hash_add(hash, &just.gaps, sizeof just.gaps);
    hash = hash_add(hash, &just.extra, sizeof just.extra);
    if (text_off != SIZE_MAX) {
        hash = hash_spans(hash, text_off, text.n);
        hash = hash_syntax(hash, text_off, text.n);
    }
    size_t sel_c0, sel_c1;
    sel_row_range(screen_row, &sel_c0, &sel_c1);
    hash = hash_add(hash, &sel_c0, sizeof sel_c0);
    hash = hash_add(hash, &sel_c1, sizeof sel_c1);
    if (!row_changed(screen_row, hash, force)) return;
    g_tui.bar_valid = false;
    cup(screen_row, 1);
    put_str(S_RESET "\033[2K");

    if (kind == ROW_COMPOSER || user || kind == ROW_CODE) {
        /* The whole row carries the panel colour, so a user turn or a fenced
         * code block reads as a block of screen rather than a prefixed line. */
        style(user ? user_bg(kind)
            : kind == ROW_CODE ? S_CODE_BG : S_PANEL_BG);
        pad_row(0, screen_cols);
        if (user) paint_user_rule(screen_row, screen_col);
        cup(screen_row, screen_col);
    } else {
        cup(screen_row, screen_col);
    }

    if (kind == ROW_COMPOSER && is_marker(prefix)) {
        const char *mark = prefix.p[0] == '!' ? S_PANEL_BG S_RED
                                              : S_PANEL_BG S_CYAN;
        style(g_tui.busy ? S_PANEL_BG S_MUTED : mark);
        put_text(prefix.p, prefix.n);
        
        style(S_PANEL_BG S_NOBOLD S_TEXT);
    } else if (prefix.n) {
        
        if (kind == ROW_WELCOME_ART) style(S_CYAN);
        else if (kind == ROW_WELCOME_TEXT) style(S_MUTED);
        else if (kind == ROW_COMPOSER) style(S_PANEL_BG);
        put_text(prefix.p, prefix.n);
    }

    
    if (kind == ROW_COMPOSER && is_marker(prefix) && text.n == 0 && !g_tui.ask) {
        size_t gutter = screen_col - 1;
        size_t body = screen_cols > gutter * 2 ? screen_cols - gutter * 2 : 0;
        size_t room = body > 2 ? body - 2 : 0;
        style(S_PANEL_BG S_MUTED);
        put_safe_clipped(prefix.p[0] == '!' ? STR("Run a shell command...")
                                            : STR("Message " AGENT_NAME "..."),
                         room, NULL);
    } else if (kind == ROW_ZONE || kind == ROW_ZONE_HOVER) {
        /* The indent belongs to the block, not to the click target: styling
         * it would draw a bar across the transcript instead of a label. */
        size_t lead = 0;
        while (lead < text.n && text.p[lead] == ' ') lead++;
        put_text(text.p, lead);
        RunStyle rs = { kind, kind, 0, false };
        run_style(&rs);
        if (!g_find.n) put_text(text.p + lead, text.n - lead);
        else put_hits(text.p + lead, lead, text.n - lead, NULL, run_style, &rs);
        style(S_RESET);
    } else if (text_off != SIZE_MAX) {
        paint_runs(text, text_off, &just, kind, user);
    } else {
        const char *s = kind_style(kind);
        if (s) style(s);
        put_just(text.p, text.n, &just);
    }
    paint_sel_tail(screen_row, screen_cols);
    style(S_RESET);
}


static void wrap_invalidate(void) {
    g_tui.wrap_cols = 0;
    g_tui.wrap_scanned = 0;
    g_tui.wrap_rows = 0;
    g_tui.ckpt_n = 0;
    g_tui.ckpt_step = 64;
}

static void ckpt_record(size_t row, size_t off) {
    if (!g_tui.ckpt_step) g_tui.ckpt_step = 64;
    if (row % g_tui.ckpt_step || row / g_tui.ckpt_step != g_tui.ckpt_n) return;
    if (g_tui.ckpt_n == TUI_CKPTS) {
        
        for (size_t i = 0; i * 2 < g_tui.ckpt_n; i++)
            g_tui.ckpt_off[i] = g_tui.ckpt_off[i * 2];
        g_tui.ckpt_n = (g_tui.ckpt_n + 1) / 2;
        g_tui.ckpt_step *= 2;
        if (row % g_tui.ckpt_step || row / g_tui.ckpt_step != g_tui.ckpt_n) return;
    }
    g_tui.ckpt_off[g_tui.ckpt_n++] = off;
}


static size_t wrap_scan(size_t cols) {
    if (cols != g_tui.wrap_cols || g_tui.wrap_scanned > g_tui.transcript_n)
        wrap_invalidate();
    if (!g_tui.wrap_cols) {
        g_tui.wrap_cols = cols;
        ckpt_record(0, 0);
    }
    Str s = { g_bulk.transcript, g_tui.transcript_n };
    size_t i = g_tui.wrap_scanned, row = g_tui.wrap_rows;
    for (;;) {
        Row r = row_break(s, i, cols, 0);
        /* The last row is where the next append lands, and a word arriving
         * into it can move the break behind bytes already scanned, so the
         * scan resumes at its start rather than at its end. */
        if (r.hard && r.end >= s.n) break;
        i = r.next;
        row++;
        ckpt_record(row, i);
    }
    g_tui.wrap_scanned = i;
    g_tui.wrap_rows = row;
    return row + 1;
}

static size_t wrap_seek(size_t row, size_t *at_row) {
    size_t k = g_tui.ckpt_step ? row / g_tui.ckpt_step : 0;
    if (g_tui.ckpt_n == 0) { *at_row = 0; return 0; }
    if (k >= g_tui.ckpt_n) k = g_tui.ckpt_n - 1;
    *at_row = k * g_tui.ckpt_step;
    return g_tui.ckpt_off[k];
}

/* The cells a row is short of the right edge, or 0 when it keeps them.
 *
 * Only a row the wrapper broke is justified: a row that ended where its
 * author ended it is as long as it was meant to be. Prose is the only shape
 * it applies to, since widening the gaps of a tool's output, a code block or
 * a diff would move columns their reader is lining up. A row left far short
 * by a word too wide to join it keeps its gaps rather than opening a river
 * through them. */
static size_t justify_pad(Str text, u8 kind, Row r, size_t cols,
                          size_t col0) {
    if (!g_tui.justify || r.hard) return 0;
    if (kind != ROW_PLAIN && kind != ROW_USER && kind != ROW_QUOTE) return 0;
    size_t used = col0 + r.width;
    if (used >= cols) return 0;
    size_t pad = cols - used;
    if (pad * 4 > cols) return 0;
    return row_gaps(text) ? pad : 0;
}

/* Bytes at the head of the composer painted as the prompt rather than as
 * text: a shell line's '!' is the marker, so a motion measured over the
 * buffer must skip it to land where the caret is drawn. */
static size_t composer_marker(const char *buf, size_t n) {
    return !g_tui.ask && n > 0 && buf[0] == '!' ? 1 : 0;
}

/* A composed line starting with '!' runs in the shell instead of reaching
 * the model, which the composer's marker announces. */
static b8 composer_shell(void) {
    return composer_marker(g_bulk.input, g_tui.input_n) != 0;
}


static void update_text_rows(Str s, size_t base_off, size_t cols,
                             size_t prompt_cells,
                             size_t first_row, size_t visible_rows,
                             size_t screen_row, size_t screen_col,
                             size_t screen_cols, u8 kind, b8 force) {
    size_t row = 0, col0 = prompt_cells, painted = 0;
    for (size_t start = 0;;) {
        Row r = row_break(s, start, cols, col0);
        if (row >= first_row && row < first_row + visible_rows) {
            Str prefix = (Str){0};
            if (row == 0 && prompt_cells)
                prefix = composer_shell() ? STR("! ") : STR("› ");
            else if (prompt_cells)
                prefix = prompt_indent(prompt_cells);
            u8 row_kind = kind;
            size_t text_off = SIZE_MAX;
            if (kind == ROW_PLAIN) {
                u8 sk = span_kind(base_off + start);
                if (kind_is_block(sk)) row_kind = sk;
                else if (user_at_off(base_off + start)) row_kind = ROW_USER;
                text_off = base_off + start;
                size_t sr = screen_row + row - first_row - 1;
                if (sr < TUI_SEL_ROWS) g_tui.row_src[sr] = base_off + start;
                
                u32 zone = zone_at_off(base_off + start);
                if (zone)
                    row_kind = zone == g_tui.hover_id ? ROW_ZONE_HOVER
                                                      : ROW_ZONE;
            }
            
            if (kind == ROW_PLAIN) {
                if (painted == 0) g_tui.view_first_off = base_off + start;
                g_tui.view_end_off = base_off + r.next;
            }
            Str text = { s.p + start, r.end - start };
            size_t pad = justify_pad(text, row_kind, r, cols, col0);
            update_text_row(screen_row + row - first_row, prefix, text,
                            screen_col, screen_cols, row_kind, text_off, pad,
                            force);
            painted++;
            
            if (painted == visible_rows) break;
        }
        if (r.hard && r.end >= s.n) break;
        start = r.next;
        row++;
    }

    while (painted < visible_rows) {
        update_text_row(screen_row + painted, (Str){0}, (Str){0}, screen_col,
                        screen_cols, kind, SIZE_MAX, 0, force);
        painted++;
    }
}


static const TuiCmd *popup_items(void) {
    return g_tui.path_mode ? g_tui.path_ents : g_tui.cmds;
}

static b8 popup_separator(const TuiCmd *cmd) {
    return cmd && cmd->desc.p == NULL && cmd->desc.n == SIZE_MAX;
}

TuiCmd tui_separator(Str label) {
    return (TuiCmd){ label, { NULL, SIZE_MAX } };
}

static void update_popup_row(size_t screen_row, Str name, Str desc,
                             TuiMark mark, b8 selected, b8 first_line,
                             b8 separator, size_t name_cells, size_t screen_col,
                             size_t screen_cols, size_t body_cols, b8 force) {
    u64 hash = row_hash(name, desc, ROW_POPUP);
    hash = hash_add(hash, &mark, sizeof mark);
    hash = hash_add(hash, &selected, sizeof selected);
    hash = hash_add(hash, &first_line, sizeof first_line);
    hash = hash_add(hash, &separator, sizeof separator);
    hash = hash_add(hash, &name_cells, sizeof name_cells);
    size_t sel_c0, sel_c1;
    sel_row_range(screen_row, &sel_c0, &sel_c1);
    hash = hash_add(hash, &sel_c0, sizeof sel_c0);
    hash = hash_add(hash, &sel_c1, sizeof sel_c1);
    if (!row_changed(screen_row, hash, force)) return;
    
    g_tui.bar_valid = false;

    const char *bg = selected ? S_POPUP_SEL : S_POPUP_BG;
    cup(screen_row, 1);
    put_str(S_RESET "\033[2K");
    style(bg);
    pad_row(0, screen_cols);
    cup(screen_row, screen_col);

    size_t used = 0;
    style(bg);
    if (separator) {
        style(S_CYAN);
        put_safe_clipped(STR("  "), body_cols, &used);
        if (used < body_cols)
            put_safe_clipped(name, body_cols - used, &used);
        paint_sel_tail(screen_row, screen_cols);
        style(S_RESET);
        return;
    }
    style(selected ? S_CYAN : S_TEXT);
    put_safe_clipped(selected && first_line ? STR("\u203a ") : STR("  "),
                     body_cols, &used);
    
    size_t name_room = name_cells > used + 2 ? name_cells - used - 2 : 0;
    put_safe_clipped(name, name_room, &used);
    while (used < name_cells && used < body_cols) { put_text(" ", 1); used++; }
    style(bg);
    style(S_MUTED);
    
    size_t m0 = mark.n && mark.off < desc.n ? mark.off : desc.n;
    size_t m1 = m0 + mark.n <= desc.n ? m0 + mark.n : desc.n;
    if (used < body_cols)
        put_safe_clipped((Str){ desc.p, m0 }, body_cols - used, &used);
    if (m1 > m0) {
        style(bg);
        style(S_GREEN);
        if (used < body_cols)
            put_safe_clipped((Str){ desc.p + m0, m1 - m0 },
                             body_cols - used, &used);
        style(bg);
        style(S_MUTED);
    }
    if (m1 < desc.n && used < body_cols)
        put_safe_clipped((Str){ desc.p + m1, desc.n - m1 },
                         body_cols - used, &used);
    paint_sel_tail(screen_row, screen_cols);
    style(S_RESET);
}

size_t tui_text_cells(Str s) {
    size_t cells = 0;
    for (size_t b = 0; b < s.n;) {
        i32 width = 0;
        b += glyph(s.p + b, s.n - b, &width);
        cells += width > 0 ? (size_t)width : 0;
    }
    return cells;
}


size_t tui_text_fit(Str s, size_t cells, size_t *used) {
    size_t b = 0, w = 0;
    while (b < s.n) {
        i32 width = 0;
        size_t step = glyph(s.p + b, s.n - b, &width);
        size_t add = width > 0 ? (size_t)width : 0;
        if (w + add > cells) break;
        b += step ? step : 1;
        w += add;
    }
    if (used) *used = w;
    return b;
}

/* Cells taken by the widest name in the whole list rather than in the rows
 * on screen, so scrolling a list moves the selection and nothing else: a
 * description column that shifted under the reader would be the list
 * redrawing itself around a key that was meant to move one row. Half the
 * width is the cap, since a list of long names is still a list of
 * descriptions. */
static size_t popup_name_cells(size_t body_cols) {
    size_t widest = 0;
    for (size_t i = 0; i < g_tui.comp_n; i++) {
        const TuiCmd *cmd = &popup_items()[g_tui.comp_idx[i]];
        if (popup_separator(cmd)) continue;
        size_t cells = tui_text_cells(cmd->name);
        if (cells > widest) widest = cells;
    }
    widest += 4;   
    size_t cap = body_cols / 2;
    return widest < cap ? widest : cap;
}

/* Visual rows one popup entry needs, capped because no popup can display
 * more than its visual-row budget. Values and descriptions use the same word
 * breaker as the transcript and composer, with each column as a hanging
 * indent. */
static size_t popup_entry_rows(const TuiCmd *cmd, size_t body_cols,
                               size_t name_cells, size_t cap) {
    if (popup_separator(cmd)) return cap ? 1 : 0;
    size_t name_rows = 0, desc_rows = 0;
    size_t name_cols = name_cells > 2 ? name_cells - 2 : name_cells;
    for (size_t start = 0; name_rows < cap;) {
        Row r = row_break(cmd->name, start, name_cols, 2);
        name_rows++;
        if (r.hard && r.end >= cmd->name.n) break;
        start = r.next;
    }
    for (size_t start = 0; desc_rows < cap;) {
        Row r = row_break(cmd->desc, start, body_cols, name_cells);
        desc_rows++;
        if (r.hard && r.end >= cmd->desc.n) break;
        start = r.next;
    }
    return name_rows > desc_rows ? name_rows : desc_rows;
}


static size_t popup_visual_rows(size_t body_cols, size_t cap) {
    size_t name_cells = popup_name_cells(body_cols);
    size_t rows = 0;
    for (size_t i = 0; i < g_tui.comp_n && rows < cap; i++) {
        const TuiCmd *cmd = &popup_items()[g_tui.comp_idx[i]];
        rows += popup_entry_rows(cmd, body_cols, name_cells, cap - rows);
    }
    return rows;
}

static void update_notice_row(size_t screen_row, Str text, size_t screen_col,
                              size_t screen_cols, size_t body_cols, b8 force) {
    u64 hash = row_hash(STR("notice"), text, ROW_POPUP);
    size_t sel_c0, sel_c1;
    sel_row_range(screen_row, &sel_c0, &sel_c1);
    hash = hash_add(hash, &sel_c0, sizeof sel_c0);
    hash = hash_add(hash, &sel_c1, sizeof sel_c1);
    if (!row_changed(screen_row, hash, force)) return;
    
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

/* A picker's question is what its rows are chosen from, so it wraps over a
 * few rows instead of being cut at the width of one. The text is laid out
 * inside the same two-cell indent update_notice_row paints. */
static size_t notice_text_cols(size_t body_cols) {
    return body_cols > 2 ? body_cols - 2 : body_cols;
}

static size_t notice_visual_rows(Str text, size_t body_cols, size_t cap) {
    size_t cols = notice_text_cols(body_cols);
    size_t rows = 0;
    for (size_t start = 0; rows < cap;) {
        Row r = row_break(text, start, cols, 0);
        rows++;
        if (r.hard && r.end >= text.n) break;
        start = r.next;
    }
    return rows;
}

static void update_notice_rows(size_t screen_row, Str text, size_t rows,
                               size_t screen_col, size_t screen_cols,
                               size_t body_cols, b8 force) {
    size_t cols = notice_text_cols(body_cols);
    size_t painted = 0;
    for (size_t start = 0; painted < rows;) {
        Row r = row_break(text, start, cols, 0);
        Str line = { text.p + start, r.end - start };
        update_notice_row(screen_row + painted, line, screen_col, screen_cols,
                          body_cols, force);
        painted++;
        if (r.hard && r.end >= text.n) break;
        start = r.next;
    }
    while (painted < rows) {
        update_notice_row(screen_row + painted, (Str){0}, screen_col,
                          screen_cols, body_cols, force);
        painted++;
    }
}

static void view_ckpt_record(size_t row, size_t off) {
    if (row % g_view.ckpt_step
        || row / g_view.ckpt_step != g_view.ckpt_n)
        return;
    if (g_view.ckpt_n == TUI_CKPTS) {
        for (size_t i = 0; i * 2 < g_view.ckpt_n; i++)
            g_view.ckpt_off[i] = g_view.ckpt_off[i * 2];
        g_view.ckpt_n = (g_view.ckpt_n + 1) / 2;
        g_view.ckpt_step *= 2;
        if (row % g_view.ckpt_step
            || row / g_view.ckpt_step != g_view.ckpt_n)
            return;
    }
    g_view.ckpt_off[g_view.ckpt_n++] = off;
}


static void view_reindex(size_t cols) {
    Str text = { g_bulk.view, g_view.text_n };
    g_view.wrap_cols = cols;
    g_view.ckpt_n = 0;
    g_view.ckpt_step = 64;
    size_t row = 0, off = 0;
    view_ckpt_record(row, off);
    for (;;) {
        Row r = row_break(text, off, cols, 0);
        if (r.hard && r.end >= text.n) break;
        off = r.next;
        row++;
        view_ckpt_record(row, off);
    }
    g_view.total_rows = row + 1;
}

static size_t view_line_row(size_t line, size_t cols) {
    Str text = { g_bulk.view, g_view.text_n };
    size_t row = 0, logical = 0;
    for (size_t off = 0;;) {
        if (logical >= line) return row;
        Row r = row_break(text, off, cols, 0);
        if (r.hard) logical++;
        if (r.hard && r.end >= text.n) return row;
        off = r.next;
        row++;
    }
}

static size_t view_seek_row(size_t want, size_t cols) {
    Str text = { g_bulk.view, g_view.text_n };
    size_t slot = want / g_view.ckpt_step;
    if (slot >= g_view.ckpt_n) slot = g_view.ckpt_n - 1;
    size_t row = slot * g_view.ckpt_step;
    size_t off = g_view.ckpt_off[slot];
    while (row < want) {
        Row r = row_break(text, off, cols, 0);
        if (r.hard && r.end >= text.n) break;
        off = r.next;
        row++;
    }
    return off;
}

static void paint_view_border(size_t row, size_t col, size_t width,
                              size_t screen_cols, Str left, Str fill,
                              Str right, b8 force) {
    u64 hash = row_hash(left, right, ROW_POPUP);
    hash = hash_add(hash, &width, sizeof width);
    hash = hash_add(hash, &g_view.top, sizeof g_view.top);
    size_t c0, c1;
    sel_row_range(row, &c0, &c1);
    hash = hash_add(hash, &c0, sizeof c0);
    hash = hash_add(hash, &c1, sizeof c1);
    if (!row_changed(row, hash, force)) return;
    
    g_tui.bar_valid = false;
    cup(row, 1);
    put_str(S_RESET "\033[2K");
    cup(row, col);
    style(S_POPUP_BG S_MUTED);
    put_text(left.p, left.n);
    for (size_t i = 2; i < width; i++) put_text(fill.p, fill.n);
    put_text(right.p, right.n);
    style(S_RESET);
    paint_sel_tail(row, screen_cols);
    style(S_RESET);
}

static void paint_view_header(size_t row, size_t col, size_t width,
                              size_t screen_cols, b8 force) {
    Str title = { g_view.title, g_view.title_n };
    u64 hash = row_hash(title, STR("[x]"), ROW_POPUP);
    hash = hash_add(hash, &width, sizeof width);
    size_t c0, c1;
    sel_row_range(row, &c0, &c1);
    hash = hash_add(hash, &c0, sizeof c0);
    hash = hash_add(hash, &c1, sizeof c1);
    if (!row_changed(row, hash, force)) return;
    g_tui.bar_valid = false;
    cup(row, 1);
    put_str(S_RESET "\033[2K");
    cup(row, col);
    style(S_POPUP_BG S_CYAN);
    put_text("│", sizeof "│" - 1);
    size_t inner = width > 2 ? width - 2 : 0;
    size_t used = 0;
    if (inner) put_safe_clipped(STR(" "), inner, &used);
    style(S_POPUP_BG S_BOLD S_TEXT);
    size_t title_room = inner > used + 5 ? inner - used - 5 : 0;
    put_safe_clipped(title, title_room, &used);
    style(S_POPUP_BG S_MUTED);
    while (used + 4 < inner) { put_text(" ", 1); used++; }
    if (used < inner) put_safe_clipped(STR("[x]"), inner - used, &used);
    while (used < inner) { put_text(" ", 1); used++; }
    style(S_POPUP_BG S_CYAN);
    put_text("│", sizeof "│" - 1);
    style(S_RESET);
    paint_sel_tail(row, screen_cols);
    style(S_RESET);
}


static size_t view_syn_first(size_t off) {
    size_t lo = 0, hi = g_view.syn_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_bulk.view_syn_b[mid] <= off) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static u8 view_syn_run(size_t off, size_t limit, size_t *end) {
    size_t i = view_syn_first(off);
    u8 kind = 0;
    size_t stop = limit;
    if (i < g_view.syn_n) {
        if (off >= g_bulk.view_syn_a[i]) {
            kind = g_bulk.view_syn_k[i];
            if (g_bulk.view_syn_b[i] < stop) stop = g_bulk.view_syn_b[i];
        } else if (g_bulk.view_syn_a[i] < stop) {
            stop = g_bulk.view_syn_a[i];
        }
    }
    *end = stop;
    return kind;
}

static u64 hash_view_syn(u64 h, size_t off, size_t n) {
    for (size_t i = view_syn_first(off); i < g_view.syn_n; i++) {
        if (g_bulk.view_syn_a[i] >= off + n) break;
        h = hash_add(h, &g_bulk.view_syn_a[i], sizeof g_bulk.view_syn_a[i]);
        h = hash_add(h, &g_bulk.view_syn_b[i], sizeof g_bulk.view_syn_b[i]);
        h = hash_add(h, &g_bulk.view_syn_k[i], sizeof g_bulk.view_syn_k[i]);
    }
    return h;
}


static void paint_view_body_row(size_t screen_row, size_t col, size_t width,
                                size_t screen_cols, Str text, size_t off,
                                b8 force) {
    u64 hash = row_hash(text, STR("view"), ROW_POPUP);
    hash = hash_add(hash, &g_view.top, sizeof g_view.top);
    if (text.n) hash = hash_view_syn(hash, off, text.n);
    size_t c0, c1;
    sel_row_range(screen_row, &c0, &c1);
    hash = hash_add(hash, &c0, sizeof c0);
    hash = hash_add(hash, &c1, sizeof c1);
    if (!row_changed(screen_row, hash, force)) return;
    g_tui.bar_valid = false;
    cup(screen_row, 1);
    put_str(S_RESET "\033[2K");
    cup(screen_row, col);
    style(S_POPUP_BG S_CYAN);
    put_text("│", sizeof "│" - 1);
    style(S_POPUP_BG S_TEXT);
    size_t inner = width > 2 ? width - 2 : 0;
    size_t used = 0;
    if (inner && !g_view.syn_n) put_safe_clipped(text, inner, &used);
    for (size_t i = 0; inner && g_view.syn_n && i < text.n && used < inner;) {
        size_t end = 0;
        u8 kind = view_syn_run(off + i, off + text.n, &end);
        size_t take = end - (off + i);
        style(S_POPUP_BG);
        style(kind ? syntax_style(kind) : S_TEXT);
        /* A glyph too wide for what is left ends the row here rather than
         * letting a narrower one from the next run past it. */
        if (put_safe_clipped((Str){ text.p + i, take }, inner - used, &used)
            < take)
            break;
        i += take;
    }
    style(S_POPUP_BG S_TEXT);
    while (used < inner) { put_text(" ", 1); used++; }
    style(S_POPUP_BG S_CYAN);
    put_text("│", sizeof "│" - 1);
    style(S_RESET);
    paint_sel_tail(screen_row, screen_cols);
    style(S_RESET);
}

/* The rectangle is measured before anything else paints, so the rows the
 * window owns are known while the transcript beneath it is still deciding
 * what to write. */
static void view_layout(size_t rows, size_t cols) {
    if (!g_view.active || rows < 7 || cols < 20) { view_unlock(); return; }
    size_t width = cols * 4 / 5;
    if (width < 20) width = 20;
    if (width > cols - 2) width = cols - 2;
    size_t height = rows * 2 / 3;
    if (height < 7) height = 7;
    if (height > rows - 2) height = rows - 2;
    size_t left = (cols - width) / 2 + 1;
    size_t top = (rows - height) / 2 + 1;
    size_t inner_cols = width - 2;
    size_t body_rows = height - 3;
    if (g_view.wrap_cols != inner_cols) {
        view_reindex(inner_cols);
        g_view.top = view_line_row(g_view.start_line, inner_cols);
    }
    size_t total = g_view.total_rows;
    size_t max_top = total > body_rows ? total - body_rows : 0;
    if (g_view.top > max_top) g_view.top = max_top;
    g_view.top_row = top;
    g_view.left_col = left;
    g_view.right_col = left + width - 1;
    g_view.bottom_row = top + height - 1;
}

static void paint_view(size_t cols, b8 force) {
    if (!g_view.active || g_view.bottom_row < g_view.top_row) return;
    size_t top = g_view.top_row;
    size_t left = g_view.left_col;
    size_t width = g_view.right_col - g_view.left_col + 1;
    size_t height = g_view.bottom_row - g_view.top_row + 1;
    size_t inner_cols = width - 2;
    size_t body_rows = height - 3;
    size_t total = g_view.total_rows;
    g_view.paint = true;
    paint_view_border(top, left, width, cols,
                      STR("┌"), STR("─"), STR("┐"), force);
    paint_view_header(top + 1, left, width, cols, force);
    Str all = { g_bulk.view, g_view.text_n };
    size_t off = view_seek_row(g_view.top, inner_cols);
    for (size_t r = 0; r < body_rows; r++) {
        Str line = {0};
        size_t line_off = off;
        if (g_view.top + r < total) {
            Row br = row_break(all, off, inner_cols, 0);
            line = (Str){ all.p + off, br.end - off };
            off = br.next;
        }
        paint_view_body_row(top + 2 + r, left, width, cols, line, line_off,
                            force);
    }
    paint_view_border(top + height - 1, left, width, cols,
                      STR("└"), STR("─"), STR("┘"), force);
    g_view.paint = false;
}

/* The search box, and the cells its caret sits at. It is an overlay of its
 * own rather than a notice: a notice answers the last command and is retired
 * by the next keystroke, while a search survives every keystroke it is being
 * typed into and every line the transcript grows by. */
static size_t find_row_caret(size_t body_cols) {
    size_t caret = 2 + tui_text_cells(STR("find: "))
                 + tui_text_cells((Str){ g_tui.find_q, g_tui.find_q_n });
    return caret < body_cols ? caret : body_cols;
}

static void update_find_row(size_t screen_row, size_t screen_col,
                            size_t screen_cols, size_t body_cols, b8 force) {
    Str query = { g_tui.find_q, g_tui.find_q_n };
    char tail[64];
    i32 written;
    if (!g_tui.find_q_n)
        written = snprintf(tail, sizeof tail, "type to search the transcript");
    else if (!g_tui.find_count)
        written = snprintf(tail, sizeof tail, "no match");
    else
        written = snprintf(tail, sizeof tail, "%zu of %zu%s", g_tui.find_index,
                           g_tui.find_count,
                           g_tui.find_wrapped ? " (wrapped)" : "");
    size_t tail_n = written > 0 && (size_t)written < sizeof tail
                  ? (size_t)written : 0;
    Str status = { tail, tail_n };
    Str hint = g_tui.find_expand
             ? STR("\u2191 older \u00b7 \u2193 newer \u00b7 "
                   "^E all output \u00b7 Esc")
             : STR("Enter/\u2191 older \u00b7 \u2193 newer \u00b7 Esc close");

    u64 hash = row_hash(query, status, ROW_POPUP);
    size_t sel_c0, sel_c1;
    sel_row_range(screen_row, &sel_c0, &sel_c1);
    hash = hash_add(hash, &sel_c0, sizeof sel_c0);
    hash = hash_add(hash, &sel_c1, sizeof sel_c1);
    hash = hash_add(hash, &body_cols, sizeof body_cols);
    if (!row_changed(screen_row, hash, force)) return;
    g_tui.bar_valid = false;

    cup(screen_row, 1);
    put_str(S_RESET "\033[2K");
    style(S_POPUP_BG);
    pad_row(0, screen_cols);
    cup(screen_row, screen_col);
    size_t used = 0;
    style(S_POPUP_BG S_CYAN);
    put_safe_clipped(STR("  find: "), body_cols, &used);
    style(S_POPUP_BG S_TEXT);
    if (used < body_cols) put_safe_clipped(query, body_cols - used, &used);
    style(S_POPUP_BG S_MUTED);
    if (used + 3 < body_cols) {
        put_safe_clipped(STR("  \u00b7  "), body_cols - used, &used);
        put_safe_clipped(status, body_cols - used, &used);
    }
    size_t hint_cells = tui_text_cells(hint);
    if (used + hint_cells < body_cols) {
        size_t pad = body_cols - used - hint_cells;
        while (pad--) { put_text(" ", 1); used++; }
        put_safe_clipped(hint, body_cols - used, &used);
    }
    paint_sel_tail(screen_row, screen_cols);
    style(S_RESET);
}


static const char *const k_spinner[] = {
    "\u280b", "\u2819", "\u2839", "\u2838", "\u283c",
    "\u2834", "\u2826", "\u2827", "\u2807", "\u280f"
};

static Str format_elapsed(char *buf, size_t cap, f64 secs) {
    u32 whole = secs > 0 ? (u32)secs : 0;
    i32 written = whole < 60
        ? snprintf(buf, cap, "%us", whole)
        : snprintf(buf, cap, "%um%02us", whole / 60u, whole % 60u);
    size_t len = written > 0 ? (size_t)written : 0;
    if (len >= cap) len = cap ? cap - 1 : 0;
    return (Str){buf, len};
}


static void update_activity_row(size_t screen_row, size_t screen_col,
                                size_t screen_cols, size_t body_cols,
                                b8 force) {
    f64 now = agent_now_seconds();
    f64 elapsed = now - g_tui.activity_started;
    char secs_buf[24];
    Str secs = format_elapsed(secs_buf, sizeof secs_buf, elapsed);
    
    char total_buf[24];
    Str total = {0};
    if (g_tui.activity_started - g_tui.activity_turn >= 1.0)
        total = format_elapsed(total_buf, sizeof total_buf,
                               now - g_tui.activity_turn);
    size_t frames = sizeof k_spinner / sizeof k_spinner[0];
    size_t frame = (size_t)(elapsed * 10.0) % frames;
    Str label = { g_tui.activity, g_tui.activity_n };
    b8 queued = g_tui.queued_n != 0;
    b8 stoppable = g_tui.busy && g_tui.interrupt != NULL;

    u64 hash = row_hash(secs, label, ROW_TOOL);
    hash = hash_add(hash, total.p, total.n);
    hash = hash_add(hash, &frame, sizeof frame);
    hash = hash_add(hash, &queued, sizeof queued);
    hash = hash_add(hash, &stoppable, sizeof stoppable);
    size_t sel_c0, sel_c1;
    sel_row_range(screen_row, &sel_c0, &sel_c1);
    hash = hash_add(hash, &sel_c0, sizeof sel_c0);
    hash = hash_add(hash, &sel_c1, sizeof sel_c1);
    if (!row_changed(screen_row, hash, force)) return;
    g_tui.bar_valid = false;

    cup(screen_row, 1);
    put_str(S_RESET "\033[2K");
    cup(screen_row, screen_col);
    size_t used = 0;
    style(S_CYAN);
    put_safe_clipped(str_c(k_spinner[frame]), body_cols, &used);
    if (used < body_cols) put_safe_clipped(STR(" "), body_cols - used, &used);
    style(S_TEXT);
    if (used < body_cols) put_safe_clipped(label, body_cols - used, &used);
    style(S_MUTED);
    if (used + 3 <= body_cols)
        put_safe_clipped(STR(" \u00b7 "), body_cols - used, &used);
    if (used < body_cols) put_safe_clipped(secs, body_cols - used, &used);
    if (total.n && used + 3 <= body_cols) {
        put_safe_clipped(STR(" \u00b7 "), body_cols - used, &used);
        if (used < body_cols)
            put_safe_clipped(total, body_cols - used, &used);
        if (used < body_cols)
            put_safe_clipped(STR(" total"), body_cols - used, &used);
    }
    if (queued && used + 3 <= body_cols) {
        put_safe_clipped(STR(" \u00b7 "), body_cols - used, &used);
        if (used < body_cols)
            put_safe_clipped(STR("message queued"), body_cols - used, &used);
    }
    if (stoppable && used + 3 <= body_cols) {
        put_safe_clipped(STR(" \u00b7 "), body_cols - used, &used);
        if (used < body_cols)
            put_safe_clipped(queued ? STR("esc to cancel message")
                                    : STR("esc to interrupt"),
                             body_cols - used, &used);
    }
    paint_sel_tail(screen_row, screen_cols);
    style(S_RESET);
}

static void paint_completions(size_t top_row, size_t rows, size_t screen_col,
                              size_t screen_cols, size_t body_cols, b8 force) {
    if (!rows) return;
    size_t name_cells = popup_name_cells(body_cols);
    
    size_t first = g_tui.comp_sel;
    size_t need = popup_entry_rows(
        &popup_items()[g_tui.comp_idx[first]], body_cols, name_cells, rows);
    while (first) {
        const TuiCmd *prev = &popup_items()[g_tui.comp_idx[first - 1]];
        size_t room = rows - need;
        size_t prev_rows = popup_entry_rows(prev, body_cols, name_cells,
                                            room + 1);
        if (prev_rows > room) break;
        need += prev_rows;
        first--;
    }

    size_t painted = 0;
    for (size_t i = first; i < g_tui.comp_n && painted < rows; i++) {
        size_t at = g_tui.comp_idx[i];
        const TuiCmd *cmd = &popup_items()[at];
        if (popup_separator(cmd)) {
            update_popup_row(top_row + painted, cmd->name, (Str){0},
                             (TuiMark){0}, false, true, true, name_cells,
                             screen_col, screen_cols, body_cols, force);
            painted++;
            continue;
        }
        TuiMark whole = g_tui.marks && !g_tui.path_mode ? g_tui.marks[at]
                                                        : (TuiMark){0};
        size_t name_start = 0, desc_start = 0;
        size_t name_cols = name_cells > 2 ? name_cells - 2 : name_cells;
        for (b8 first_line = true; painted < rows; first_line = false) {
            Row nr = row_break(cmd->name, name_start, name_cols, 2);
            Row dr = row_break(cmd->desc, desc_start, body_cols, name_cells);
            TuiMark mark = {0};
            size_t m0 = whole.off > desc_start ? whole.off : desc_start;
            size_t mend = whole.off <= cmd->desc.n
                        && whole.n <= cmd->desc.n - whole.off
                        ? whole.off + whole.n : cmd->desc.n;
            size_t m1 = mend < dr.end ? mend : dr.end;
            if (whole.n && m1 > m0)
                mark = (TuiMark){ m0 - desc_start, m1 - m0 };
            const char *name = cmd->name.p ? cmd->name.p + name_start : NULL;
            const char *desc = cmd->desc.p ? cmd->desc.p + desc_start : NULL;
            update_popup_row(top_row + painted,
                             (Str){ name, nr.end - name_start },
                             (Str){ desc, dr.end - desc_start }, mark,
                             i == g_tui.comp_sel,
                             first_line, false,
                             name_cells, screen_col, screen_cols, body_cols,
                             force);
            painted++;
            b8 name_done = nr.hard && nr.end >= cmd->name.n;
            b8 desc_done = dr.hard && dr.end >= cmd->desc.n;
            if (name_done && desc_done) break;
            name_start = name_done ? cmd->name.n : nr.next;
            desc_start = desc_done ? cmd->desc.n : dr.next;
        }
    }
    
    while (painted < rows) {
        update_popup_row(top_row + painted, (Str){0}, (Str){0}, (TuiMark){0},
                         false, false, false, name_cells, screen_col,
                         screen_cols, body_cols, force);
        painted++;
    }
}


typedef struct { Str text; b8 art; } WelcomeLine;


#define WLINE(lit, is_art) { { (lit), sizeof(lit) - 1 }, (is_art) }
static const WelcomeLine k_welcome[] = {
    WLINE("  __ _ _ __ __ _  __ _ _ __",       true),
    WLINE(" / _` | '__/ _` |/ _` | '_ \\",      true),
    WLINE("| (_| | | | (_| | (_| | | | |",     true),
    WLINE(" \\__,_|_|  \\__, |\\__,_|_| |_|",     true),
    WLINE("              |_|",                 true),
    WLINE("",              false),
    WLINE(AGENT_NAME " " AGENT_VERSION " · a tiny terminal coding agent", false),
    WLINE("",              false),
    WLINE("type a message and press Enter to begin",          false),
};

#define WELCOME_LINES (sizeof k_welcome / sizeof k_welcome[0])

static Str welcome_text(size_t i) {
    if (g_tui.setup_hint.n && i + 1 == WELCOME_LINES)
        return g_tui.setup_hint;
    return k_welcome[i].text;
}

static size_t welcome_widest(b8 art_only) {
    size_t widest = 0;
    for (size_t i = 0; i < WELCOME_LINES; i++) {
        if (art_only && !k_welcome[i].art) continue;
        size_t cells = tui_text_cells(welcome_text(i));
        if (cells > widest) widest = cells;
    }
    return widest;
}


static b8 welcome_fits(size_t body_cols, size_t transcript_rows) {
    return welcome_widest(false) <= body_cols
        && WELCOME_LINES + 2 <= transcript_rows;
}

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
                            ROW_PLAIN, SIZE_MAX, 0, force);
            continue;
        }
        const WelcomeLine *line = &k_welcome[row - top - 1];
        Str text = welcome_text(row - top - 1);
        size_t pad = line->art ? art_pad
                   : (body_cols - tui_text_cells(text)) / 2;
        if (pad > sizeof blanks) pad = sizeof blanks;
        update_text_row(row, (Str){blanks, text.n ? pad : 0}, text,
                        body_col, screen_cols,
                        line->art ? ROW_WELCOME_ART : ROW_WELCOME_TEXT,
                        SIZE_MAX, 0, force);
    }
}

static void paint_size_warning(size_t rows, size_t cols,
                               size_t reported_rows, size_t reported_cols,
                               b8 force) {
    char current[96], needed[96], compact[96];
    i32 current_n = snprintf(current, sizeof current,
                             "Width = %zu Height = %zu",
                             reported_cols, reported_rows);
    i32 needed_n = snprintf(needed, sizeof needed,
                            "Width = %u Height = %u",
                            TUI_MIN_COLS, TUI_MIN_ROWS);
    i32 compact_n = snprintf(compact, sizeof compact,
                             "%zux%zu; need %ux%u",
                             reported_cols, reported_rows,
                             TUI_MIN_COLS, TUI_MIN_ROWS);
    Str full[] = {
        STR("Terminal size too small:"),
        { current, current_n > 0 ? (size_t)current_n : 0 },
        STR("Needed for current config:"),
        { needed, needed_n > 0 ? (size_t)needed_n : 0 },
    };
    Str compact_lines[] = {
        STR("Terminal too small"),
        { compact, compact_n > 0 ? (size_t)compact_n : 0 },
    };
    Str *lines = cols >= sizeof "Needed for current config:" - 1
               ? full : compact_lines;
    size_t line_n = lines == full ? sizeof full / sizeof full[0]
                                  : sizeof compact_lines / sizeof compact_lines[0];
    size_t shown = rows < line_n ? rows : line_n;
    size_t top = (rows - shown) / 2;
    for (size_t i = 0; i < shown && cols; i++) {
        Str line = lines[i];
        if (line.n > cols) line.n = cols;
        size_t col = (cols - line.n) / 2 + 1;
        update_text_row(top + i + 1, (Str){0}, line, col, cols,
                        i == 0 ? ROW_ERROR : ROW_PLAIN,
                        SIZE_MAX, 0, force);
    }
}

static b8 terminal_too_small(void) {
    size_t rows, cols;
    terminal_size(&rows, &cols);
    return rows < TUI_MIN_ROWS || cols < TUI_MIN_COLS;
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
        if (view_locks_row(i + 1)) continue;
        cup(i + 1, screen_col);
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


static void format_context_count(char *buf, size_t cap, size_t n) {
    if (n >= 1000000) snprintf(buf, cap, "%zuM", (n + 500000) / 1000000);
    else if (n >= 1000) snprintf(buf, cap, "%zuk", (n + 500) / 1000);
    else snprintf(buf, cap, "%zu", n);
}

static Str format_context_size(char *buf, size_t cap) {
    size_t n = g_tui.context_tokens;
    const char *mark = g_tui.context_exact ? "" : "~";
    i32 written;
    if (!g_tui.context_known) {
        written = snprintf(buf, cap, "-");
    } else if (g_tui.context_window) {
        char count[16];
        char window[16];
        format_context_count(count, sizeof count, n);
        format_context_count(window, sizeof window, g_tui.context_window);
        written = snprintf(buf, cap, "%s%s/%s", mark, count, window);
    } else {
        char count[16];
        format_context_count(count, sizeof count, n);
        written = snprintf(buf, cap, "%s%s", mark, count);
    }
    size_t len = written > 0 ? (size_t)written : 0;
    if (len >= cap) len = cap ? cap - 1 : 0;
    return (Str){buf, len};
}

void tui_batch_begin(void) { g_paint.batch = true; }

static void repaint(void);

void tui_batch_end(void) {
    if (!g_paint.batch) return;
    g_paint.batch = false;
    repaint();
}

static void find_refresh(void);
static void find_goto(size_t off);
static void find_close(void);
static size_t rows_below(size_t off);

static void repaint(void) {
    if (!g_tui.fullscreen || g_paint.batch) return;
    g_tui.last_paint = agent_now_seconds();

    size_t physical_rows, physical_cols;
    terminal_size(&physical_rows, &physical_cols);
    if (physical_rows < TUI_MIN_ROWS || physical_cols < TUI_MIN_COLS) {
        view_unlock();
        size_t paint_rows = physical_rows ? physical_rows : 24;
        size_t paint_cols = physical_cols ? physical_cols : 80;
        if (paint_rows > TUI_MAX_ROWS) paint_rows = TUI_MAX_ROWS;
        b8 force = !g_tui.frame_valid || !g_tui.size_warning || g_winch
                 || paint_rows != g_tui.painted_rows
                 || paint_cols != g_tui.painted_cols;
        memset(g_tui.row_src, 0xff, sizeof g_tui.row_src);
        g_winch = 0;
        frame_begin();
        if (force) {
            sel_clear();
            put_str("\033[?25l\033[H\033[2J");
            memset(g_tui.row_hash, 0, sizeof g_tui.row_hash);
            memset(g_tui.row_text_n, 0, sizeof g_tui.row_text_n);
            memset(g_tui.row_text_w, 0, sizeof g_tui.row_text_w);
        } else {
            put_str("\033[?25l");
        }
        paint_size_warning(paint_rows, paint_cols,
                           physical_rows, physical_cols, force);
        g_tui.painted_rows = paint_rows;
        g_tui.painted_cols = paint_cols;
        g_tui.frame_valid = true;
        g_tui.size_warning = true;
        frame_end();
        return;
    }
    /* The count the box shows is carried over the bytes appended since the
     * last frame, so a streaming turn costs the delta rather than a scan. */
    if (g_tui.find_open) find_refresh();

    size_t rows, cols;
    screen_size(&rows, &cols);
    b8 force = !g_tui.frame_valid || g_tui.size_warning || g_winch
             || rows != g_tui.painted_rows || cols != g_tui.painted_cols;
    /* Measured here rather than where it paints: the rows it covers neither
     * paint nor invalidate from the transcript, and that has to be known
     * before the transcript writes them. */
    view_layout(rows, cols);
    /* Rows not painted from the transcript carry no offset, so the mapping is
     * rebuilt rather than aged. */
    memset(g_tui.row_src, 0xff, sizeof g_tui.row_src);
    g_winch = 0;
    frame_begin();
    if (force) {
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
    Str input = { g_bulk.input, g_tui.input_n };
    size_t input_cur = g_tui.input_cur;
    char mask[TUI_ASK_MAX];
    if (g_tui.ask_secret) {
        size_t n = input.n < sizeof mask ? input.n : sizeof mask;
        memset(mask, '*', n);
        input = (Str){ mask, n };
        if (input_cur > n) input_cur = n;
    }
    if (composer_shell()) {
        
        input = str_drop(input, 1);
        if (input_cur) input_cur--;
    }
    size_t input_rows = text_rows(input, body_cols, 2, input_cur,
                                  &cursor_row, &cursor_col);
    size_t composer_padding = rows >= 6 ? 1 : 0;
    size_t status_gap = composer_padding;
    
    size_t body_gap = composer_padding;
    size_t composer_cap = rows / 3;
    if (composer_cap < 1) composer_cap = 1;
    if (composer_cap > 8) composer_cap = 8;
    size_t composer_rows = input_rows < composer_cap ? input_rows : composer_cap;
    size_t chrome_rows = 1 + composer_padding * 2 + status_gap + body_gap;
    size_t max_composer = rows > chrome_rows ? rows - chrome_rows : 1;
    if (max_composer > 1) max_composer--;   
    if (composer_rows > max_composer) composer_rows = max_composer;
    size_t body_rows = rows > composer_rows + chrome_rows
                     ? rows - composer_rows - chrome_rows : 1;
    /* Overlays stack upward from the composer and cover the bottom of the
     * transcript rather than pushing it up, so opening one hides the last
     * rows and leaves every other where the reader last saw it. One row
     * always stays uncovered. */
    size_t overlay_cap = body_rows > 1 ? body_rows - 1 : 0;
    /* A modal picker holds the frame until it is answered, so its list is
     * given most of the screen: a page of options read at once is a page
     * nobody has to walk through with the arrow keys. The composer's own
     * completions keep the fixed height, since the draft under them is what
     * the reader is working on. */
    size_t popup_cap = TUI_POPUP_ROWS;
    if (g_tui.picking && overlay_cap * 2 / 3 > popup_cap)
        popup_cap = overlay_cap * 2 / 3;
    size_t popup_rows = popup_visual_rows(body_cols, popup_cap);
    size_t notice_rows = g_tui.notice_n ? 1 : 0;
    
    size_t pick_notice_cap = overlay_cap / 3;
    if (pick_notice_cap > TUI_PICK_NOTICE_ROWS)
        pick_notice_cap = TUI_PICK_NOTICE_ROWS;
    if (pick_notice_cap < 1) pick_notice_cap = 1;
    size_t pick_notice_rows =
        g_tui.pick_notice_n
            ? notice_visual_rows((Str){ g_tui.pick_notice,
                                        g_tui.pick_notice_n },
                                 body_cols, pick_notice_cap)
            : 0;
    size_t find_rows = g_tui.find_open ? 1 : 0;
    /* The spinner row reads as the next line of the conversation, so it sits
     * against the transcript with a block's row of air above it rather than
     * among the overlays below. */
    size_t activity_rows = g_tui.activity_n ? 2 : 0;
    
    if (popup_rows && pick_notice_rows && overlay_cap < 2)
        pick_notice_rows = 0;
    if (pick_notice_rows > overlay_cap) pick_notice_rows = overlay_cap;
    if (popup_rows + pick_notice_rows > overlay_cap)
        popup_rows = overlay_cap - pick_notice_rows;
    if (pick_notice_rows + notice_rows + popup_rows > overlay_cap)
        notice_rows = overlay_cap - pick_notice_rows - popup_rows;
    if (find_rows + pick_notice_rows + notice_rows + popup_rows > overlay_cap)
        find_rows = overlay_cap - pick_notice_rows - notice_rows - popup_rows;
    if (activity_rows + find_rows + pick_notice_rows + notice_rows
        + popup_rows > overlay_cap)
        activity_rows = overlay_cap - find_rows - pick_notice_rows
                      - notice_rows - popup_rows;
    
    size_t overlay_rows = find_rows + pick_notice_rows + notice_rows
                        + popup_rows;
    size_t transcript_rows = body_rows - overlay_rows - activity_rows;

    /* Pinned to the bottom unless PageUp moved the viewport. Overlays stay in
     * the window because they cover the transcript; the spinner is stacked
     * below it and would otherwise hide the newest line. */
    size_t all_rows = wrap_scan(body_cols);
    /* An overlay covers the bottom of the transcript rather than pushing it
     * up, which leaves the newest rows under it. A search box may not: a match
     * it cannot show is a search that did not answer, so while it is open the
     * rows it covers are scrolled out from under it. */
    /* Nor may a notice: it answers the last command and is read beside the
     * transcript rather than instead of it, so the rows it would cover are
     * lifted for as long as it is up. A screen borrows the same slot for its
     * own hint, and a screen is read instead of the transcript, so while one
     * is open the notice covers like the rest. */
    size_t notice_lift = g_tui.picking || g_view.active ? 0 : notice_rows;
    size_t view_rows = body_rows - activity_rows - notice_lift;
    if (g_tui.find_open) view_rows = transcript_rows;
    else if (g_tui.keep_off != SIZE_MAX) {
        
        size_t need = rows_below(g_tui.keep_off);
        if (need < transcript_rows) need = transcript_rows;
        if (need < view_rows) view_rows = need;
    }
    size_t max_scroll = all_rows > view_rows ? all_rows - view_rows : 0;
    if (g_tui.scroll_rows > max_scroll) g_tui.scroll_rows = max_scroll;
    size_t first = all_rows > view_rows + g_tui.scroll_rows
                 ? all_rows - view_rows - g_tui.scroll_rows : 0;
    if (g_tui.transcript_n == 0 && welcome_fits(body_cols, transcript_rows))
        paint_welcome(body_rows, transcript_rows, body_col, body_cols, cols,
                      force);
    else {
        size_t at_row = 0;
        size_t off = wrap_seek(first, &at_row);
        Str slice = { g_bulk.transcript + off, g_tui.transcript_n - off };
        update_text_rows(slice, off, body_cols, 0, first - at_row,
                         transcript_rows, 1, body_col, cols, ROW_PLAIN, force);
    }
    
    if (g_tui.find_open && g_tui.find_cur != SIZE_MAX && !g_find.moving
        && (g_tui.find_cur < g_tui.view_first_off
            || g_tui.find_cur >= g_tui.view_end_off)) {
        g_find.moving = true;
        find_goto(g_tui.find_cur);
        repaint();
        g_find.moving = false;
        return;
    }
    paint_scrollbar(first, all_rows, transcript_rows, cols, force);
    if (activity_rows > 1)
        update_text_row(transcript_rows + 1, (Str){0}, (Str){0}, body_col,
                        cols, ROW_PLAIN, SIZE_MAX, 0, force);
    if (activity_rows)
        update_activity_row(transcript_rows + activity_rows, body_col, cols,
                            body_cols, force);
    if (body_gap)
        update_text_row(transcript_rows + activity_rows + 1, (Str){0},
                        (Str){0}, body_col, cols, ROW_PLAIN, SIZE_MAX, 0,
                        force);

    
    size_t overlay_top = transcript_rows + activity_rows + body_gap + 1;
    if (find_rows)
        update_find_row(overlay_top, body_col, cols, body_cols, force);
    if (pick_notice_rows)
        update_notice_rows(overlay_top + find_rows,
                           (Str){ g_tui.pick_notice, g_tui.pick_notice_n },
                           pick_notice_rows, body_col, cols, body_cols, force);
    if (notice_rows)
        update_notice_row(overlay_top + find_rows + pick_notice_rows,
                          (Str){ g_tui.notice, g_tui.notice_n }, body_col, cols,
                          body_cols, force);
    paint_completions(overlay_top + find_rows + pick_notice_rows + notice_rows,
                      popup_rows, body_col, cols, body_cols, force);

    /* The window over a draft taller than the box scrolls by the row the
     * caret leaves rather than snapping to it, so walking a long draft pages
     * through it and the rows around the caret stay where the reader put
     * them. It is only ever moved far enough to keep the caret in view, and
     * never past the last row of the text. */
    size_t input_first = g_tui.input_top;
    size_t input_max_top = input_rows > composer_rows
                         ? input_rows - composer_rows : 0;
    if (input_first > input_max_top) input_first = input_max_top;
    if (cursor_row < input_first) input_first = cursor_row;
    else if (cursor_row >= input_first + composer_rows)
        input_first = cursor_row - composer_rows + 1;
    g_tui.input_top = input_first;
    size_t composer_top_row = overlay_top + overlay_rows;
    size_t composer_screen_row = composer_top_row + composer_padding;
    if (composer_padding)
        update_text_row(composer_top_row, (Str){0}, (Str){0}, body_col,
                        cols, ROW_COMPOSER, SIZE_MAX, 0, force);
    update_text_rows(input, 0, body_cols, 2, input_first, composer_rows,
                     composer_screen_row, body_col, cols, ROW_COMPOSER, force);
    if (composer_padding)
        update_text_row(composer_screen_row + composer_rows, (Str){0}, (Str){0},
                        body_col, cols, ROW_COMPOSER, SIZE_MAX, 0, force);

    
    size_t status_row = composer_screen_row + composer_rows + composer_padding
                      + status_gap;
    if (status_gap)
        update_text_row(status_row - status_gap, (Str){0}, (Str){0}, body_col,
                        cols, ROW_PLAIN, SIZE_MAX, 0, force);
    const char *status = g_tui.status[0] ? g_tui.status : "ready";
    b8 copied = g_tui.copy_notice > g_tui.last_paint;
    size_t status_sel_c0, status_sel_c1;
    sel_row_range(status_row, &status_sel_c0, &status_sel_c1);
    u64 status_hash = row_hash(g_tui.model, g_tui.provider, ROW_STATUS);
    status_hash = hash_add(status_hash, g_tui.reasoning_effort.p,
                           g_tui.reasoning_effort.n);
    status_hash = hash_add(status_hash, g_tui.thinking_budget.p,
                           g_tui.thinking_budget.n);
    status_hash = hash_add(status_hash, &copied, sizeof copied);
    status_hash = hash_add(status_hash, &g_tui.mode, sizeof g_tui.mode);
    status_hash = hash_add(status_hash, &g_tui.permissions,
                           sizeof g_tui.permissions);
    status_hash = hash_add(status_hash, &status_sel_c0, sizeof status_sel_c0);
    status_hash = hash_add(status_hash, &status_sel_c1, sizeof status_sel_c1);
    status_hash = hash_add(status_hash, g_tui.cwd.p, g_tui.cwd.n);
    status_hash = hash_add(status_hash, status, strlen(status));
    status_hash = hash_add(status_hash, &g_tui.activity_n,
                           sizeof g_tui.activity_n);
    status_hash = hash_add(status_hash, &g_tui.context_tokens,
                           sizeof g_tui.context_tokens);
    status_hash = hash_add(status_hash, &g_tui.context_known,
                           sizeof g_tui.context_known);
    status_hash = hash_add(status_hash, &g_tui.context_exact,
                           sizeof g_tui.context_exact);
    status_hash = hash_add(status_hash, &g_tui.context_window,
                           sizeof g_tui.context_window);
    status_hash = hash_add(status_hash, g_tui.status_visible,
                           sizeof g_tui.status_visible);
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
        b8 have_field = false;
        /* Spelled out rather than only coloured, since the bullet says
         * nothing on a NO_COLOR terminal, and first so a narrow screen clips
         * it last. While the spinner row is up it says the same word beside
         * the seconds it has been true for, so here the bullet carries the
         * state alone rather than repeating it a row below. */
        if (g_tui.status_visible[TUI_STATUS_STATE]) {
            style(status_style);
            put_safe_clipped(STR("● "), body_cols, &used);
            if (!g_tui.activity_n && used < body_cols) {
                put_safe_clipped(str_c(status), body_cols - used, &used);
                have_field = true;
            }
        }
        if (g_tui.status_visible[TUI_STATUS_MODEL])
            put_status_field(g_tui.model, S_TEXT, body_cols, &used,
                             &have_field);
        if (g_tui.status_visible[TUI_STATUS_REASONING]) {
            Str effort = g_tui.reasoning_effort;
            b8 off = str_eq(effort, STR("off")) || str_eq(effort, STR("Off"))
                  || str_eq(effort, STR("OFF"));
            if (!off)
                put_status_field(effort, S_TEXT, body_cols, &used,
                                 &have_field);
        }
        if (g_tui.status_visible[TUI_STATUS_THINKING]) {
            Str budget = g_tui.thinking_budget;
            b8 off = str_eq(budget, STR("off")) || str_eq(budget, STR("Off"))
                  || str_eq(budget, STR("OFF"));
            if (budget.n && !off) {
                char thinking[AGENT_MAX_REASONING_LIST + 10];
                i32 n = snprintf(thinking, sizeof thinking, "thinking %.*s",
                                 (i32)budget.n, budget.p);
                if (n > 0)
                    put_status_field((Str){thinking, (size_t)n}, S_TEXT,
                                     body_cols, &used, &have_field);
            }
        }
        if (g_tui.status_visible[TUI_STATUS_MODE])
            put_status_field(g_tui.mode == MODE_PLAN ? STR("plan")
                                                     : STR("build"),
                             g_tui.mode == MODE_PLAN ? S_YELLOW : S_TEXT,
                             body_cols, &used, &have_field);
        if (g_tui.status_visible[TUI_STATUS_PROVIDER])
            put_status_field(g_tui.provider, S_TEXT, body_cols, &used,
                             &have_field);
        if (g_tui.status_visible[TUI_STATUS_CWD])
            put_status_field(cwd, S_TEXT, body_cols, &used, &have_field);
        if (g_tui.status_visible[TUI_STATUS_CONTEXT])
            put_status_field(context, S_TEXT, body_cols, &used, &have_field);
        if (copied && g_tui.status_visible[TUI_STATUS_COPY])
            put_status_field(STR("copied"), S_GREEN, body_cols, &used,
                             &have_field);
        if (g_tui.status_visible[TUI_STATUS_PERMISSIONS])
            put_status_field(g_tui.permissions == PERMISSION_FREE
                             ? STR("free") : STR("ask"),
                             g_tui.permissions == PERMISSION_ASK
                             ? S_YELLOW : S_TEXT,
                             body_cols, &used, &have_field);
        paint_sel_tail(status_row, cols);
        style(S_RESET);
    }

    /* Unlike completion and settings, this is a real centered window. It is
     * painted last, over transcript and chrome alike, and owns no picker or
     * completion state. */
    paint_view(cols, force);

    if (g_tui.editing && !g_view.active) {
        /* The caret belongs to whatever is being typed into: while the search
         * box holds the keyboard, that is the query rather than the draft. */
        size_t screen_cursor_row = find_rows
                                 ? overlay_top
                                 : composer_screen_row + cursor_row - input_first;
        size_t screen_cursor_col = find_rows
                                 ? gutter + find_row_caret(body_cols) + 1
                                 : gutter + cursor_col + 1;
        if (screen_cursor_col > cols) screen_cursor_col = cols;
        cup(screen_cursor_row, screen_cursor_col);
        put_str("\033[?25h");
    }
    g_tui.painted_rows = rows;
    g_tui.painted_cols = cols;
    g_tui.frame_valid = true;
    g_tui.size_warning = false;
    frame_end();
}

/* Log lines become transcript notices while the alternate screen is up, so a
 * curl failure reads like the rest of the conversation instead of tearing a
 * hole in the frame. */
static void tui_log_sink(i32 level, Str msg, void *ud) {
    (void)ud;
    static const char *tags[] = {"debug", "info", "warn", "error"};
    tui_printf("\n[%s: %.*s]\n", tags[level], (i32)msg.n, msg.p);
}


static void keys_selfcheck(void);

void tui_start(Str model, Str base_url, b8 missing_key, b8 setup,
               size_t tool_count, b8 show_ignored, b8 justify,
               u64 status_fields, AgentMode mode, b8 plain) {
    if (g_tui.raw) return;
    
    memset(&g_tui, 0, sizeof g_tui);
    memset(&g_view, 0, sizeof g_view);
    keys_selfcheck();
    for (size_t i = 0; i < TUI_STATUS_N; i++)
        g_tui.status_visible[i] = (status_fields & ((u64)1 << i)) != 0;
    g_tui.find_cur = SIZE_MAX;
    g_tui.keep_off = SIZE_MAX;
    g_tui.tty = !plain && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    g_tui.model = setup ? (Str){0} : model;
    g_tui.base_url = base_url;
    g_tui.provider = setup ? (Str){0} : provider_from_url(base_url);
    agent_ignore_set_show(show_ignored);
    g_tui.justify = justify;
    g_tui.mode = mode;
    tui_set_status(setup ? "setup" : "ready");
    capture_cwd();
    const char *term = getenv("TERM");
    g_tui.color = getenv("NO_COLOR") == NULL
               && (!term || strcmp(term, "dumb"));
    

    if (!g_tui.tty) {
        g_tui.raw = true;
        if (plain) return;
        char banner[512];
        i32 n = setup
              ? snprintf(banner, sizeof banner,
                         AGENT_NAME " %s · setup tools=%zu\n",
                         AGENT_VERSION, tool_count)
              : snprintf(banner, sizeof banner,
                         AGENT_NAME " %s · model=%.*s base=%.*s tools=%zu\n",
                         AGENT_VERSION, (i32)model.n, model.p,
                         (i32)base_url.n, base_url.p, tool_count);
        if (n > 0) put_raw(banner, (size_t)n < sizeof banner
                                   ? (size_t)n : sizeof banner - 1);
        if (missing_key && !setup) put_str("warn: no API key set\n");
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
    agent_log_set_sink(tui_log_sink, NULL);
    g_tui.editing = true;
    
    put_str("\033[?1049h\033[?7l\033[?25l\033[?1003h\033[?1006h\033[?2004h");
    repaint();
}

void tui_stop(void) {
    
    if (!g_tui.fullscreen && g_tui.wrote_any && !g_tui.trail_nl) {
        put_raw("\n", 1);
        g_tui.trail_nl = 1;
        flush_out();
    }
    if (!g_tui.raw) return;
    g_view.active = false;
    agent_log_set_sink(NULL, NULL);
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

b8 tui_busy(void) { return g_tui.busy; }

b8 tui_queued_pending(void) { return g_tui.queued_n != 0; }

Str tui_queued_take(void) {
    Str out = { g_bulk.queued, g_tui.queued_n };
    if (!out.n) return out;
    g_tui.queued_n = 0;
    g_tui.notice_n = 0;
    repaint();
    return out;
}

void tui_set_busy(b8 busy) {
    if (g_tui.busy == busy) return;
    g_tui.busy = busy;
    repaint();
}

void tui_activity_end(void) {
    if (!g_tui.activity_n) return;
    g_tui.activity_n = 0;
    g_tui.activity_started = 0;
    g_tui.activity_turn = 0;
    repaint();
}

void tui_activity(Str label) {
    if (!g_tui.fullscreen) return;
    if (!label.n) { tui_activity_end(); return; }
    size_t n = label.n < sizeof g_tui.activity ? label.n
                                               : sizeof g_tui.activity;
    if (g_tui.activity_n == n && !memcmp(g_tui.activity, label.p, n)) return;
    
    if (!g_tui.activity_n) g_tui.activity_turn = agent_now_seconds();
    g_tui.activity_started = agent_now_seconds();
    memcpy(g_tui.activity, label.p, n);
    g_tui.activity_n = n;
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


void tui_set_status(const char *status) {
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

void tui_set_reasoning(Str effort, Str thinking_budget) {
    g_tui.reasoning_effort = effort;
    g_tui.thinking_budget = thinking_budget;
    repaint();
}

void tui_set_setup(b8 on) {
    if (on) {
        g_tui.model = (Str){0};
        g_tui.provider = (Str){0};
        tui_set_status("setup");
        return;
    }
    tui_set_status("ready");
}

b8 tui_status_visible(TuiStatusItem item) {
    return (size_t)item < TUI_STATUS_N && g_tui.status_visible[item];
}

void tui_set_status_visible(TuiStatusItem item, b8 visible) {
    if ((size_t)item >= TUI_STATUS_N) return;
    g_tui.status_visible[item] = visible;
    repaint();
}

void tui_set_mode(AgentMode mode) {
    g_tui.mode = mode;
    repaint();
}

void tui_set_permissions(PermissionPolicy policy) {
    g_tui.permissions = policy;
    repaint();
}

void tui_set_setup_hint(Str hint) {
    g_tui.setup_hint = hint;
    repaint();
}

b8 tui_copy(Str text) {
    if (!text.n || text.n > TUI_SEL_BYTES) return false;
    put_str("\033]52;c;");
    b64_put((const u8 *)text.p, text.n);
    put_str("\a");
    flush_out();
    g_tui.copy_notice = agent_now_seconds() + 2.0;
    repaint();
    return true;
}

void tui_set_context(size_t tokens, b8 known, b8 exact, size_t window) {
    if (g_tui.context_tokens == tokens && g_tui.context_known == known
        && g_tui.context_exact == exact && g_tui.context_window == window)
        return;
    g_tui.context_tokens = known ? tokens : 0;
    g_tui.context_known = known;
    g_tui.context_exact = known && exact;
    g_tui.context_window = window;
    repaint();
}


static void put_passthrough(Str seq) {
    put_str("\033Ptmux;");
    for (size_t i = 0; i < seq.n; i++) {
        if (seq.p[i] == '\033') put_raw("\033", 1);
        put_raw(seq.p + i, 1);
    }
    put_str("\033\\");
}


void tui_desktop_notify(Str text) {
    if (!g_tui.tty || !text.n || text.n > AGENT_MAX_NOTIFY_TEXT) return;
    char seq[AGENT_MAX_NOTIFY_TEXT + 8];
    /* "9;" followed by a digit is a sub-command in ConEmu, Windows Terminal
     * and ghostty (progress, cwd, marks); the caller's leading name keeps the
     * text out of that space, and a payload that lost it is not sent. */
    if (text.p[0] >= '0' && text.p[0] <= '9') return;
    size_t n = 0;
    memcpy(seq + n, "\033]9;", 4); n += 4;
    memcpy(seq + n, text.p, text.n); n += text.n;
    seq[n++] = '\a';
    Str s = { seq, n };
    if (getenv("TMUX")) put_passthrough(s);
    else put_raw(s.p, s.n);
    flush_out();
}

void tui_bell(void) {
    if (!g_tui.tty) return;
    put_raw("\a", 1);
    flush_out();
}

void tui_notice(Str msg) {
    if (!g_tui.fullscreen) {   
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
    g_tui.keep_off = SIZE_MAX;
    g_tui.pend_nl = 0;
    g_tui.trail_nl = 0;
    g_tui.span_n = 0;
    g_tui.span_head = 0;
    g_tui.syntax_n = 0;
    g_tui.syntax_head = 0;
    g_tui.transcript_epoch++;
    g_tui.user_n = 0;
    g_tui.user_open = false;
    g_tui.zone_n = 0;
    g_tui.zone_open = 0;
    g_tui.pin_n = 0;
    g_tui.hover_id = 0;
    find_invalidate();
    wrap_invalidate();
    g_tui.scroll_rows = 0;
    g_tui.frame_valid = false;
    repaint();
}

void tui_zone_begin(u32 id) {
    if (!g_tui.fullscreen || !id) return;
    nl_commit();   
    g_tui.zone_open = id;
    g_tui.zone_open_a = g_tui.transcript_n;
}

void tui_zone_end(void) {
    if (!g_tui.zone_open) return;
    zone_add(g_tui.zone_open_a, g_tui.transcript_n, g_tui.zone_open);
    g_tui.zone_open = 0;
}


void tui_scroll_to_bottom(void) {
    if (!g_tui.scroll_rows) return;
    g_tui.scroll_rows = 0;
    repaint();
}

/* Rows from `off` to the end, which is what a zone's place on screen is
 * measured against: a re-render is free to change everything above it.
 *
 * The wrapped-row index answers this from the checkpoint before `off`, so an
 * offset megabytes back costs the rows since that checkpoint rather than a
 * wrap of everything under it. The index belongs to the width it was built
 * at; at any other the tail is wrapped directly. */
static size_t rows_below(size_t off) {
    size_t cols = tui_body_cols();
    if (!cols || off >= g_tui.transcript_n) return 0;
    if (g_tui.wrap_cols == cols && g_tui.ckpt_n
        && g_tui.wrap_scanned <= g_tui.transcript_n) {
        size_t lo = 0, hi = g_tui.ckpt_n;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (g_tui.ckpt_off[mid] <= off) lo = mid + 1; else hi = mid;
        }
        size_t k = lo ? lo - 1 : 0;
        Str s = { g_bulk.transcript, g_tui.transcript_n };
        size_t row = k * g_tui.ckpt_step, i = g_tui.ckpt_off[k];
        while (i < off) {
            Row r = row_break(s, i, cols, 0);
            if (r.next <= i || r.next > off) break;
            i = r.next;
            row++;
        }
        size_t all = g_tui.wrap_rows + 1;
        return all > row ? all - row : 1;
    }
    Str tail = { g_bulk.transcript + off, g_tui.transcript_n - off };
    return text_rows(tail, cols, 0, 0, NULL, NULL);
}


static void find_survey(size_t limit, size_t *count, size_t *last,
                        size_t *rank) {
    size_t n = 0, best = SIZE_MAX, best_rank = 0;
    for (size_t i = find_next_at(0); i != SIZE_MAX; i = find_next_at(i + 1)) {
        if (i < limit) { best = i; best_rank = n; }
        n++;
    }
    *count = n;
    *last = best;
    *rank = best_rank;
}

static void find_goto(size_t off) {
    if (off == SIZE_MAX) return;
    if (off >= g_tui.view_first_off && off < g_tui.view_end_off) return;
    size_t view = g_tui.bar_visible ? g_tui.bar_visible : 1;
    size_t keep = view - view / 3;
    if (keep) keep--;
    size_t below = rows_below(off);
    g_tui.scroll_rows = below > keep ? below - keep : 0;
}

static void find_seek(void) {
    g_tui.find_wrapped = false;
    g_tui.find_epoch = g_tui.transcript_epoch;
    g_tui.find_scanned = g_tui.transcript_n;
    if (!g_tui.find_q_n) {
        g_tui.find_count = 0;
        g_tui.find_index = 0;
        g_tui.find_cur = SIZE_MAX;
        return;
    }
    size_t limit = g_tui.scroll_rows ? g_tui.view_end_off : g_tui.transcript_n;
    size_t off = SIZE_MAX, rank = 0;
    find_survey(limit, &g_tui.find_count, &off, &rank);
    if (off == SIZE_MAX) {
        
        off = find_next_at(0);
        rank = 0;
    }
    g_tui.find_cur = off;
    g_tui.find_index = off == SIZE_MAX ? 0 : rank + 1;
    find_goto(off);
}

static void find_refresh(void) {
    if (!g_tui.find_q_n) {
        g_tui.find_count = 0;
        g_tui.find_index = 0;
        g_tui.find_cur = SIZE_MAX;
        g_tui.find_scanned = g_tui.transcript_n;
        g_tui.find_epoch = g_tui.transcript_epoch;
        return;
    }
    if (g_tui.find_epoch == g_tui.transcript_epoch
        && g_tui.find_scanned == g_tui.transcript_n)
        return;
    if (g_tui.find_epoch != g_tui.transcript_epoch
        || g_tui.find_scanned == SIZE_MAX
        || g_tui.find_scanned > g_tui.transcript_n) {
        
        if (g_tui.find_cur != SIZE_MAX && find_match_at(g_tui.find_cur)) {
            size_t last = SIZE_MAX, rank = 0;
            find_survey(g_tui.find_cur, &g_tui.find_count, &last, &rank);
            
            g_tui.find_index = last == SIZE_MAX ? 1 : rank + 2;
            g_tui.find_epoch = g_tui.transcript_epoch;
            g_tui.find_scanned = g_tui.transcript_n;
        } else {
            find_seek();
        }
        return;
    }
    
    size_t q = g_tui.find_q_n;
    size_t from = g_tui.find_scanned > q - 1 ? g_tui.find_scanned - (q - 1) : 0;
    g_tui.find_count += find_count_from(from);
    g_tui.find_scanned = g_tui.transcript_n;
}


static void find_step(i32 dir) {
    find_refresh();
    if (!g_tui.find_count) return;
    if (g_tui.find_cur == SIZE_MAX) { find_seek(); return; }
    size_t off = dir < 0 ? find_prev_before(g_tui.find_cur)
                         : find_next_at(g_tui.find_cur + 1);
    g_tui.find_wrapped = off == SIZE_MAX;
    if (g_tui.find_wrapped) {
        off = dir < 0 ? find_prev_before(g_tui.transcript_n) : find_next_at(0);
        g_tui.find_index = dir < 0 ? g_tui.find_count : 1;
    } else {
        g_tui.find_index = dir < 0 ? g_tui.find_index - 1
                                   : g_tui.find_index + 1;
    }
    if (off == SIZE_MAX) return;
    g_tui.find_cur = off;
    find_goto(off);
}

static size_t zone_start(u32 id) {
    for (size_t i = g_tui.zone_n; i-- > 0;)
        if (g_tui.zone_id[i] == id) return g_tui.zone_a[i];
    return SIZE_MAX;
}

static size_t pin_start(u32 id) {
    for (size_t i = g_tui.pin_n; i-- > 0;)
        if (g_tui.pin_id[i] == id) return g_tui.pin_off[i];
    return SIZE_MAX;
}

static size_t anchor_start(u32 id) {
    return g_tui.anchor_is_pin ? pin_start(id) : zone_start(id);
}

void tui_pin(u32 id) {
    if (!g_tui.fullscreen || !id) return;
    pin_add(g_tui.transcript_n, id);
}

void tui_anchor_zone(u32 id) {
    g_tui.anchor_is_pin = false;
    size_t off = zone_start(id);
    g_tui.anchor_id = off == SIZE_MAX ? 0 : id;
    g_tui.anchor_below = g_tui.anchor_id ? rows_below(off) : 0;
    g_tui.anchor_scroll = g_tui.scroll_rows;
}

void tui_anchor_view(void) {
    g_tui.anchor_id = 0;
    g_tui.anchor_is_pin = true;
    g_tui.anchor_scroll = g_tui.scroll_rows;
    g_tui.anchor_below = 0;
    
    if (!g_tui.scroll_rows || !g_tui.pin_n) return;
    
    size_t need = g_tui.scroll_rows + g_tui.bar_visible;
    
    size_t lo = 0, hi = g_tui.pin_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (rows_below(g_tui.pin_off[mid]) >= need) lo = mid + 1; else hi = mid;
    }
    size_t i = lo ? lo - 1 : 0;
    g_tui.anchor_id = g_tui.pin_id[i];
    g_tui.anchor_below = rows_below(g_tui.pin_off[i]);
}

void tui_restore_anchor(void) {
    u32 id = g_tui.anchor_id;
    g_tui.anchor_id = 0;
    
    if (!g_tui.anchor_scroll) return;
    size_t off = id ? anchor_start(id) : SIZE_MAX;
    if (off == SIZE_MAX) {
        
        if (g_tui.anchor_is_pin) {
            g_tui.scroll_rows = g_tui.anchor_scroll;
            repaint();
        }
        return;
    }
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
    g_tui.notice_n = 0;
    
    if (!g_tui.sel_drag && !g_view.active) sel_clear();

    if (s.n >= TUI_TRANSCRIPT_CAP) {
        s.p += s.n - (TUI_TRANSCRIPT_CAP - 1);
        s.n = TUI_TRANSCRIPT_CAP - 1;
        g_tui.transcript_n = 0;
        g_tui.span_n = 0;
        g_tui.span_head = 0;
        g_tui.syntax_n = 0;
        g_tui.syntax_head = 0;
        g_tui.transcript_epoch++;
        g_tui.user_n = 0;
        g_tui.zone_n = 0;
        g_tui.pin_n = 0;
        g_tui.keep_off = SIZE_MAX;
        find_invalidate();
        wrap_invalidate();   
    } else if (g_tui.transcript_n + s.n >= TUI_TRANSCRIPT_CAP) {
        size_t room_for_old = TUI_TRANSCRIPT_CAP - 1 - s.n;
        size_t keep = g_tui.transcript_n;
        if (keep > TUI_TRANSCRIPT_CAP / 2) keep = TUI_TRANSCRIPT_CAP / 2;
        if (keep > room_for_old) keep = room_for_old;
        memmove(g_bulk.transcript,
                g_bulk.transcript + g_tui.transcript_n - keep, keep);
        spans_shift(g_tui.transcript_n - keep);
        syntax_shift(g_tui.transcript_n - keep);
        g_tui.transcript_epoch++;
        users_shift(g_tui.transcript_n - keep);
        zones_shift(g_tui.transcript_n - keep);
        pins_shift(g_tui.transcript_n - keep);
        find_shift(g_tui.transcript_n - keep);
        if (g_tui.keep_off != SIZE_MAX) {
            size_t delta = g_tui.transcript_n - keep;
            g_tui.keep_off = g_tui.keep_off > delta ? g_tui.keep_off - delta
                                                    : 0;
        }
        g_tui.transcript_n = keep;
        wrap_invalidate();   
    }

    
    size_t cols = g_tui.scroll_rows ? tui_body_cols() : 0;
    size_t rows_before = cols ? wrap_scan(cols) : 0;

    for (size_t i = 0; i < s.n && g_tui.transcript_n + 4 < TUI_TRANSCRIPT_CAP; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == '\r') continue;
        if (c == '\t') {
            memcpy(g_bulk.transcript + g_tui.transcript_n, "    ", 4);
            g_tui.transcript_n += 4;
        } else if (c == '\n' || c >= 0x20) {
            g_bulk.transcript[g_tui.transcript_n++] = (char)c;
        } else {
            continue;
        }
        g_tui.trail_nl = c == '\n' ? g_tui.trail_nl + 1 : 0;
    }
    if (cols) {
        size_t rows_after = wrap_scan(cols);
        if (rows_after > rows_before)
            g_tui.scroll_rows += rows_after - rows_before;
    }
}

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
    
    size_t need = empty ? (g_tui.fullscreen ? 1 : 0)
                : g_tui.trail_nl < 2 ? 2 - g_tui.trail_nl : 0;
    if (g_tui.pend_nl < need) g_tui.pend_nl = need;
}

void tui_write(Str s) {
    /* The busiest path in a turn, so it doubles as the pump keeping the
     * composer responsive and servicing a pending resize. Markdown may split
     * one provider delta into many styled writes, so poll once per bounded
     * batch rather than issuing a poll syscall for every style run. */
    static u8 poll_skip;
    if (g_tui.fullscreen && (g_winch || poll_skip == 0)) {
        tui_poll_input();
        poll_skip = 63;
    } else if (g_tui.fullscreen) {
        poll_skip--;
    }
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
    
    f64 now = agent_now_seconds();
    if (g_winch || now - g_tui.last_paint >= 1.0 / 15.0)
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


static void write_span(Str s, u8 kind) {
    if (!g_tui.fullscreen) { tui_write(s); return; }
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
        [TUI_STRIKE] = ROW_STRIKE,
    };
    if (st == TUI_PLAIN || (size_t)st >= sizeof kinds) tui_write(s);
    else write_span(s, kinds[st]);
}

void tui_write_muted(Str s)  { write_span(s, ROW_REASON); }
void tui_write_text(Str s)   { write_span(s, ROW_PLAIN); }
void tui_write_source(Str s) { write_span(s, ROW_SOURCE); }
void tui_write_tool(Str s)   { write_span(s, ROW_TOOL); }
void tui_write_result(Str s) { write_span(s, ROW_RESULT); }
void tui_write_error(Str s)  { write_span(s, ROW_ERROR); }

b8 tui_highlight_enabled(void) {
    return g_tui.fullscreen && g_tui.color;
}

size_t tui_transcript_pos(void) { return g_tui.transcript_n; }
u64 tui_transcript_epoch(void) { return g_tui.transcript_epoch; }

void tui_keep_visible(size_t off) {
    if (off <= g_tui.transcript_n) g_tui.keep_off = off;
}

void tui_syntax_add(size_t a, size_t b, u8 kind) {
    if (!tui_highlight_enabled() || a >= b || b > g_tui.transcript_n
        || kind < YHL_SEM_COMMENT || kind > YHL_SEM_BUILTIN)
        return;
    size_t last = g_tui.syntax_n ? syntax_slot(g_tui.syntax_n - 1) : 0;
    if (g_tui.syntax_n && g_tui.syntax_k[last] == kind
        && g_tui.syntax_b[last] == a) {
        g_tui.syntax_b[last] = b;
        return;
    }
    if (g_tui.syntax_n == TUI_MAX_SYNTAX) {
        g_tui.syntax_head = (g_tui.syntax_head + 1) % TUI_MAX_SYNTAX;
        g_tui.syntax_n--;
    }
    size_t slot = syntax_slot(g_tui.syntax_n);
    g_tui.syntax_a[slot] = a;
    g_tui.syntax_b[slot] = b;
    g_tui.syntax_k[slot] = kind;
    g_tui.syntax_n++;
}

void tui_syntax_commit(void) { repaint(); }

/* A user turn is a block of screen rather than a labelled line: a padding row
 * above and below, and the whole range recorded so every row it wraps onto
 * carries the panel background. */
void tui_user_begin(void) {
    tui_block();
    if (!g_tui.fullscreen) {
        tui_write(STR("> "));
        return;
    }
    nl_commit();                          
    g_tui.user_open_a = g_tui.transcript_n;
    g_tui.user_open = true;
    tui_write(STR("\n"));                 
}

void tui_user_end(void) {
    if (!g_tui.fullscreen || !g_tui.user_open) return;
    /* The padding row below belongs to the box, so it is committed here to
     * fall inside the recorded range rather than left to the next block. */
    tui_write(STR("\n\n"));
    nl_commit();
    size_t b = g_tui.transcript_n;
    user_add(g_tui.user_open_a, b);
    g_tui.user_open = false;
    g_tui.pend_nl = 1;
}

void tui_write_user(Str s) {
    tui_user_begin();
    tui_write(s);
    tui_user_end();
}

void tui_set_interrupt_flag(volatile sig_atomic_t *flag) {
    g_tui.interrupt = flag;
}

/* NOTE: `pushed` rather than a -1 sentinel on `pushback`, so the whole struct
 * zero-initializes and its 8KB buffer stays in .bss. */
static struct {
    unsigned char b[8192];
    size_t n, at;
    i32    pushback;
    b8     pushed;
} g_input;

static b8 input_buffered(void) { return g_input.at < g_input.n; }

#ifdef AGENT_TESTING
/* A settled screen, announced. The suite drives the UI over a pty, where the
 * only other way to know a repaint has finished is to wait out a silence long
 * enough to be sure it will not be broken; this says so instead, so a case
 * spends no time guessing. Emitted where the reader is about to block with a
 * painted frame behind it, which is the definition of settled.
 *
 * It carries the number of input bytes consumed so far, which is what makes
 * it answerable: a park is only the answer to what a case wrote once the
 * count reaches everything the case has sent. A bare sequence number cannot
 * distinguish that from the park before it.
 *
 * It carries the size the frame behind it was painted for for the same
 * reason: a resize is a signal rather than input, so no byte count moves and
 * the park before it would otherwise pass for the answer to it.
 *
 * APC: terminals consume and ignore it, and it carries no cursor or cell
 * effect, so a golden screen is the same with it as without. Compiled into
 * the test build alone; the shipped binary has no beacon and no counter. */
static unsigned long g_idle_seq;
static unsigned long g_input_bytes;

static void idle_beacon(void) {
    if (!g_tui.fullscreen || !g_tui.frame_valid) return;
    char b[96];
    i32 n = snprintf(b, sizeof b, "\033_agent;idle;%lu;%lu;%zux%zu\033\\",
                     ++g_idle_seq, g_input_bytes, g_tui.painted_cols,
                     g_tui.painted_rows);
    if (n > 0 && (size_t)n < sizeof b) {
        put_raw(b, (size_t)n);
        flush_out();
    }
}

/* Input taken by the poll path, which does not park and so paints no settled
 * frame: this reports the count alone, and never claims the screen is done.
 * Silent unless that path has taken input since the last one, so a streaming
 * turn pays no write and no flush per pump. */
static void input_notice(void) {
    static unsigned long last = ~0UL;
    if (!g_tui.fullscreen) return;
    if (g_input_bytes == last) return;
    last = g_input_bytes;
    char b[64];
    int  n = snprintf(b, sizeof b, "\033_agent;input;%lu\033\\", g_input_bytes);
    if (n > 0 && (size_t)n < sizeof b) {
        put_raw(b, (size_t)n);
        flush_out();
    }
}
#else
#define idle_beacon() ((void)0)
#define input_notice() ((void)0)
#endif

static b8 input_ready(i32 timeout_ms) {
    if (g_input.pushed || input_buffered()) return true;
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    i32 rc = poll(&pfd, 1, timeout_ms);
    return rc > 0 && (pfd.revents & (POLLIN | POLLHUP)) != 0;
}

static i32 rbyte(void) {
    if (g_input.pushed) { g_input.pushed = false; return g_input.pushback; }
    if (!input_buffered()) {
        
        idle_beacon();
        
        ssize_t n = read(STDIN_FILENO, g_input.b, sizeof g_input.b);
        if (n < 0 && errno == EINTR) return g_winch ? -3 : -2;
        if (n <= 0) return -1;
#ifdef AGENT_TESTING
        g_input_bytes += (unsigned long)n;
#endif
        g_input.n = (size_t)n;
        g_input.at = 0;
    }
    return (i32)g_input.b[g_input.at++];
}

/* Continuation byte of an escape sequence. A bare Esc must not park the
 * reader on a blocking read, especially while polling during a turn. */
static i32 rbyte_soon(void) {
    return input_ready(50) ? rbyte() : -1;
}

/* A bracketed paste is one burst: the terminal writes the start marker, the
 * text and the end marker in one go. An end marker that never arrives - a
 * terminal that died mid-paste, or text that carried a start marker of its
 * own - would otherwise leave the composer treating every later key as text,
 * with no key left that could recover it. So the paste is retired once the
 * input has stayed drained, which only the caller about to block may decide:
 * a paste larger than one read still arrives back to back.
 *
 * The grace covers the gap a writer leaves when a paste too big for the
 * terminal's pipe is refilled a block at a time, and stays far below the
 * time it takes to reach for the next key, which must not be eaten as text.
 */
enum { PASTE_GRACE_MS = 20 };

static void paste_retire_if_drained(void) {
    if (!g_tui.pasting || input_ready(PASTE_GRACE_MS)) return;
    g_tui.pasting = false;
    g_tui.paste_cr = false;
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
    KEY_NONE = 0, KEY_IGNORE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME,
    KEY_END,
    KEY_PREV_WORD, KEY_NEXT_WORD, KEY_NEWLINE, KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_WHEEL_UP, KEY_WHEEL_DOWN, KEY_MOUSE_DOWN, KEY_MOUSE_DRAG, KEY_MOUSE_UP,
    KEY_MOUSE_MOVE, KEY_SHIFT_TAB, KEY_PASTE,
    KEY_DELETE, KEY_TOP, KEY_BOTTOM,
    
    KEY_KILL_WORD, KEY_KILL_PREV_WORD
};

static struct {
    i32 row, col;
} g_mouse;

/* ---- keybindings ---------------------------------------------------------
 * Every key the UI answers is a row of one context's table. A context is one
 * layer of the input chain: the search box, an open screen, the composer, and
 * the line editor the composer and a question share.
 *
 * A key is bound at most once per context, and the compiler enforces it: the
 * tables expand into a `switch`, so a second row for the same key in one
 * table is a duplicate case value and the build fails naming both sites.
 * Where one key means two things, the choice is a branch inside its single
 * row and turns on state; it is never a second binding placed earlier in a
 * chain, which is what used to make the loser silent dead code.
 *
 * Contexts are layered, and the outer one is tried first: the composer's
 * table runs before the shared editor's, so a key in both is an override and
 * not a duplicate. Only one context owns the keyboard at a time, so the same
 * byte meaning different things in two of them is intended, and the page
 * `/keys` renders it under both.
 *
 * A row is `X(key, label, help, body)`. `key` is a byte or a KEY_ code,
 * `body` the statements that run, and `label` names the key on the page; an
 * empty label hides the row, which is how an alias nobody types (Ctrl-J for
 * Enter, Ctrl-H for Backspace) stays bound without a line of its own.
 *
 * KEY_PASTE is deliberately unbound. read_escape consumes the bracketed
 * paste markers and sets `g_tui.pasting` as it goes, so every context reads
 * the same paste state without binding the key itself.
 */

#define KEY_CASE(key, label, help, ...) case key: { __VA_ARGS__ } break;
#define KEY_DOC(key, label, help, ...)  { label, help },

typedef struct { const char *key; const char *help; } KeyRow;
typedef struct { const char *name; const KeyRow *rows; size_t n; } KeyContext;

static i32 read_escape(void) {
    i32 first = rbyte_soon();
    if (first < 0) return KEY_NONE;
    /* Two Escapes in one burst are two keys: the second opens a sequence of
     * its own rather than closing this one. */
    if (first == 0x1b) { g_input.pushback = 0x1b; g_input.pushed = true; return KEY_NONE; }
    if (first == '\r' || first == '\n') return KEY_NEWLINE;
    if (first == '[') {
        Csi csi;
        i32 final = read_csi(&csi);
        if (csi.mouse && (final == 'M' || final == 'm') && csi.nparams >= 1) {
            i32 button = csi.p[0];
            if (button & 64) return button & 1 ? KEY_WHEEL_DOWN : KEY_WHEEL_UP;
            if (csi.nparams < 3) return KEY_IGNORE;
            g_mouse.col = csi.p[1];
            g_mouse.row = csi.p[2];
            if (final == 'm') return KEY_MOUSE_UP;
            if (button & 32) return (button & 3) == 3 ? KEY_MOUSE_MOVE
                                                      : KEY_MOUSE_DRAG;
            if ((button & 3) == 0) return KEY_MOUSE_DOWN;
            return KEY_IGNORE;
        }
        i32 modifier = csi.nparams >= 2 ? csi.p[1] : 0;
        b8 ctrl = modifier == 5;
        switch (final) {
            case 'D': return ctrl ? KEY_PREV_WORD : KEY_LEFT;
            case 'C': return ctrl ? KEY_NEXT_WORD : KEY_RIGHT;
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'H': return ctrl ? KEY_TOP : KEY_HOME;
            case 'F': return ctrl ? KEY_BOTTOM : KEY_END;
            case 'Z': return KEY_SHIFT_TAB;
            case '~':
                if (csi.nparams < 1) return KEY_IGNORE;
                if (csi.nparams >= 3 && csi.p[0] == 27
                    && csi.p[1] == 2
                    && (csi.p[2] == '\r' || csi.p[2] == '\n'))
                    return KEY_NEWLINE;
                if (csi.p[0] == 1 || csi.p[0] == 7)
                    return ctrl ? KEY_TOP : KEY_HOME;
                if (csi.p[0] == 3) return KEY_DELETE;
                if (csi.p[0] == 4 || csi.p[0] == 8)
                    return ctrl ? KEY_BOTTOM : KEY_END;
                if (csi.p[0] == 5) return KEY_PAGE_UP;
                if (csi.p[0] == 6) return KEY_PAGE_DOWN;
                
                if (csi.p[0] == 200 || csi.p[0] == 201) {
                    g_tui.pasting = csi.p[0] == 200;
                    g_tui.paste_cr = false;
                    return KEY_PASTE;
                }
                return KEY_IGNORE;
            case 'u':
                if (csi.nparams >= 2 && csi.p[1] == 2
                    && (csi.p[0] == '\r' || csi.p[0] == '\n'))
                    return KEY_NEWLINE;
                return KEY_IGNORE;
            default: return KEY_IGNORE;
        }
    }
    
    if (first == 'O') {
        switch (rbyte_soon()) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            default:  return KEY_IGNORE;
        }
    }
    
    switch (first) {
        case 'b': case 'B': return KEY_PREV_WORD;
        case 'f': case 'F': return KEY_NEXT_WORD;
        case 'd': case 'D': return KEY_KILL_WORD;
        case 0x7f: case 0x08: return KEY_KILL_PREV_WORD;
        default: break;
    }
    return KEY_IGNORE;
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

static size_t ctrl_a_start(const char *buf, size_t cur) {
    size_t start = line_start(buf, cur);
    if (cur != start || start == 0) return start;
    return line_start(buf, start - 1);
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
        if (pos + 1 >= TUI_PATH_ENTS) return;   
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
        if (n + name.n + 2 >= AGENT_MAX_PATH) continue;
        memcpy(path + n, name.p, name.n);
        size_t end = n + name.n;
        path[end] = '\0';
        b8 is_dir;
        
        if (e->d_type == DT_DIR) is_dir = true;
        else if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK)
            is_dir = path_is_dir(path);
        else is_dir = false;
        
        if (is_dir && str_eq(name, STR(".git"))) continue;
        if (!agent_ignore_show()
            && agent_ignore_match(&g_tui.ignore, path, end, is_dir)) continue;

        u8 rank = is_dir ? 0 : 1;   
        Str rel = { path + root_n, end - root_n };
        b8 hit = !q.n || path_rank_of(rel, name, q, &rank);
        if (is_dir) {
            path[end] = '/';
            path[end + 1] = '\0';
            if (hit) path_insert(path, end + 1, rank, depth);
            if (depth < max_depth && *budget) {
                AgentIgnoreMark mark = agent_ignore_mark(&g_tui.ignore);
                agent_ignore_push(&g_tui.ignore, path, end + 1, end + 1);
                path_walk(path, end + 1, root_n, q, (u16)(depth + 1),
                          max_depth, budget);
                agent_ignore_restore(&g_tui.ignore, mark);
            }
        } else if (hit) {
            path_insert(path, end, rank, depth);
        }
    }
    (void)closedir(d);
}


static void path_refresh(Str prefix, Str keep) {
    size_t cut = 0;
    for (size_t i = prefix.n; i-- > 0;)
        if (prefix.p[i] == '/') { cut = i + 1; break; }
    Str dir = { prefix.p, cut };
    Str base = { prefix.p + cut, prefix.n - cut };
    char path[AGENT_MAX_PATH];
    if (dir.n + 1 >= sizeof path) return;
    memcpy(path, dir.p, dir.n);
    path[dir.n] = '\0';

    g_tui.path_n = 0;
    agent_ignore_build(&g_tui.ignore, dir);
    /* The walk is bounded rather than complete: a tree nobody ignored is
     * still answered in the time a keystroke has. */
    size_t budget = TUI_PATH_SCAN;
    path_walk(path, dir.n, dir.n, base, 0,
              base.n ? TUI_PATH_DEPTH : 0, &budget);

    size_t n = g_tui.path_n;
    for (size_t i = 0; i < n; i++) g_tui.comp_idx[i] = g_tui.path_ord[i];
    g_tui.comp_n = n;
    g_tui.comp_sel = 0;
    
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
        char c = g_bulk.input[start - 1];
        if (c == ' ' || c == '\t' || c == '\n') break;
        start--;
    }
    if (start >= cur || g_bulk.input[start] != '@') return false;
    g_tui.path_at = start;
    *out = (Str){ g_bulk.input + start + 1, cur - start - 1 };
    return true;
}


static void completion_refresh(void) {
    /* The selected path is copied out rather than aliased: the rebuild it
     * survives is what overwrites the buffer it lives in. */
    char keep[AGENT_MAX_PATH];
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
    Str in = { g_bulk.input, g_tui.input_n };
    if (in.n == 0 || in.p[0] != '/') return;
    for (size_t i = 0; i < in.n; i++)
        if (in.p[i] == ' ' || in.p[i] == '\t' || in.p[i] == '\n') return;
    size_t exact = SIZE_MAX;
    for (size_t i = 0; i < g_tui.cmd_n && g_tui.comp_n < AGENT_MAX_COMMANDS; i++) {
        if (!str_starts_ci(g_tui.cmds[i].name, in)) continue;
        if (i == previous) g_tui.comp_sel = g_tui.comp_n;
        if (g_tui.cmds[i].name.n == in.n) exact = g_tui.comp_n;
        g_tui.comp_idx[g_tui.comp_n++] = (u16)i;
    }
    
    for (size_t a = 0; a < g_tui.alias_n && g_tui.comp_n < AGENT_MAX_COMMANDS;
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
    size_t next = g_tui.comp_sel;
    for (size_t tried = 0; tried < n; tried++) {
        next = (next + (delta > 0 ? 1 : n - 1)) % n;
        if (!popup_separator(&popup_items()[g_tui.comp_idx[next]])) {
            g_tui.comp_sel = next;
            return;
        }
    }
}

/* A name typed out in full has nothing left to complete, so the completion
 * keys must not swallow the keystroke that finishes the command. */
static b8 completion_would_change(void) {
    if (!g_tui.comp_n) return false;
    if (g_tui.path_mode) return true;
    Str name = g_tui.cmds[g_tui.comp_idx[g_tui.comp_sel]].name;
    return name.n != g_tui.input_n
        || memcmp(name.p, g_bulk.input, name.n) != 0;
}

/* An image is attached rather than typed: the bytes are what a model that
 * can see needs, and the path alone says nothing to one that cannot open it.
 * The type is decided by the name here and by the header in media_add, which
 * is what refuses a file that only looks like an image. */
static b8 path_is_image(Str name) {
    static const char *const k_ext[] = { ".png", ".jpg", ".jpeg", ".gif",
                                         ".webp" };
    for (size_t i = 0; i < sizeof k_ext / sizeof k_ext[0]; i++) {
        size_t n = strlen(k_ext[i]);
        if (name.n <= n) continue;
        size_t at = name.n - n, k = 0;
        while (k < n && lower_ascii(name.p[at + k]) == k_ext[i][k]) k++;
        if (k == n) return true;
    }
    return false;
}


static b8 attach_offered(void) {
    for (size_t i = 0; i < g_tui.cmd_n; i++)
        if (str_eq(g_tui.cmds[i].name, STR("/attach"))) return true;
    return false;
}


static void path_accept(Str name) {
    size_t start = g_tui.path_at + 1;
    size_t cur = g_tui.input_cur;
    size_t tail = g_tui.input_n - cur;
    /* An image is taken as an attachment instead, and the '@' word goes with
     * it: the attachment writes an [Image #n] marker of its own, so leaving
     * the path would name the same file twice. */
    if (name.n && name.p[name.n - 1] != '/' && name.n <= sizeof g_bulk.attach
        && path_is_image(name) && attach_offered()) {
        memcpy(g_bulk.attach, name.p, name.n);
        g_tui.attach_n = name.n;
        memmove(g_bulk.input + g_tui.path_at, g_bulk.input + cur, tail);
        g_tui.input_n = g_tui.path_at + tail;
        g_tui.input_cur = g_tui.path_at;
        g_bulk.input[g_tui.input_n] = '\0';
        g_tui.comp_n = 0;
        g_tui.comp_sel = 0;
        g_tui.path_mode = false;
        g_tui.comp_dismissed = true;
        return;
    }
    if (start + name.n + tail + 1 > sizeof g_bulk.input) return;
    memmove(g_bulk.input + start + name.n, g_bulk.input + cur, tail);
    memcpy(g_bulk.input + start, name.p, name.n);
    g_tui.input_n = start + name.n + tail;
    g_tui.input_cur = start + name.n;
    g_bulk.input[g_tui.input_n] = '\0';
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
    size_t n = name.n < sizeof g_bulk.input - 1 ? name.n : sizeof g_bulk.input - 1;
    memcpy(g_bulk.input, name.p, n);
    g_bulk.input[n] = '\0';
    g_tui.input_n = n;
    g_tui.input_cur = n;
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.comp_dismissed = true;
}

b8 tui_show_ignored(void) { return agent_ignore_show(); }

void tui_set_show_ignored(b8 on) {
    agent_ignore_set_show(on);
}

b8 tui_justify(void) { return g_tui.justify; }


void tui_set_justify(b8 on) {
    if (g_tui.justify == on) return;
    g_tui.justify = on;
    g_tui.frame_valid = false;
}

void tui_set_history(History *h) {
    g_tui.hist = h;
    g_tui.draft_n = 0;
    g_tui.hist_nav = false;
    if (h) history_reset_cursor(h);
}

static void composer_load(char *buf, size_t *n, size_t *cur, Str s) {
    size_t take = s.n < AGENT_LINE_BUF - 1 ? s.n : AGENT_LINE_BUF - 1;
    if (take) memcpy(buf, s.p, take);
    buf[take] = '\0';
    *n = take;
    *cur = take;
    
    g_tui.input_top = 0;
    g_tui.goal_col_valid = false;
}

/* Up or Down over the draft's own visual rows, wrapped ones included, aiming
 * at the column the run of moves started from. False when there is no row
 * that way, which is the caller's cue to hand the key to history recall. */
static b8 composer_move_row(i32 dir, const char *buf, size_t n, size_t *cur) {
    size_t mark = composer_marker(buf, n);
    Str s = { (char *)buf + mark, n - mark };
    size_t view_cur = *cur > mark ? *cur - mark : 0;
    size_t cols = tui_body_cols();
    if (!cols) return false;
    size_t row = 0, col = 0;
    size_t total = text_rows(s, cols, 2, view_cur, &row, &col);
    if (dir < 0 ? row == 0 : row + 1 >= total) return false;
    if (!g_tui.goal_col_valid) {
        g_tui.goal_col = col;
        g_tui.goal_col_valid = true;
    }
    size_t want = dir < 0 ? row - 1 : row + 1;
    *cur = mark + row_col_off(s, cols, 2, want, g_tui.goal_col);
    return true;
}


static b8 history_recall(i32 dir, char *buf, size_t *n, size_t *cur) {
    History *h = g_tui.hist;
    if (!h || !h->n) return false;
    Str entry;
    if (dir < 0) {
        if (!history_browsing(h)) {
            memcpy(g_bulk.draft, buf, *n);
            g_tui.draft_n = *n;
        }
        if (!history_prev(h, &entry)) return false;
        composer_load(buf, n, cur, entry);
        return true;
    }
    if (!history_browsing(h)) return false;
    if (history_next(h, &entry)) { composer_load(buf, n, cur, entry); return true; }
    composer_load(buf, n, cur, (Str){ g_bulk.draft, g_tui.draft_n });
    return true;
}

void tui_set_input(Str s) {
    if (!g_tui.fullscreen) return;
    composer_load(g_bulk.input, &g_tui.input_n, &g_tui.input_cur, s);
    g_tui.comp_dismissed = false;
    g_tui.hist_nav = false;
    completion_refresh();
    repaint();
}

Str tui_input(void) {
    if (!g_tui.fullscreen) return (Str){0};
    return (Str){ g_bulk.input, g_tui.input_n };
}

void tui_set_commands(const TuiCmd *cmds, size_t n) {
    g_tui.cmds = cmds;
    g_tui.cmd_n = n < AGENT_MAX_COMMANDS ? n : AGENT_MAX_COMMANDS;
}

void tui_set_aliases(const TuiAlias *aliases, size_t n) {
    g_tui.aliases = aliases;
    g_tui.alias_n = n;
}

/* A picker searches literally and case-insensitively: a name either holds
 * what was typed or it does not, with no fuzzy ordering to explain. The
 * settings screen matches loosely instead, since its rows are sentences the
 * reader is recalling rather than names they know. */
static void pick_filter(Str query, b8 fuzzy) {
    g_tui.comp_n = 0;
    for (size_t i = 0; i < g_tui.cmd_n && g_tui.comp_n < AGENT_MAX_POPUP; i++) {
        if (popup_separator(&g_tui.cmds[i]) && query.n) continue;
        if (fuzzy ? str_fuzzy_ci(g_tui.cmds[i].name, query)
                  : str_contains_ci(g_tui.cmds[i].name, query))
            g_tui.comp_idx[g_tui.comp_n++] = (u16)i;
    }
    
    g_tui.comp_sel = g_tui.pick_end && g_tui.comp_n ? g_tui.comp_n - 1 : 0;
    if (g_tui.comp_n
        && popup_separator(&g_tui.cmds[g_tui.comp_idx[g_tui.comp_sel]]))
        completion_move(g_tui.pick_end ? -1 : 1);
}


static void scroll_to_top(void) {
    g_tui.scroll_rows = SIZE_MAX;
    repaint();
}


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
    else if (key == KEY_TOP || key == KEY_HOME) scroll_to_top();
    else if (key == KEY_BOTTOM || key == KEY_END) tui_scroll_to_bottom();
    else return false;
    return true;
}

static void view_close(void) {
    if (!g_view.active) return;
    g_view.active = false;
    g_view.modal = false;
    g_view.close_down = false;
    /* Rows under the rectangle have stale hashes from before it opened; the
     * next frame must restore all of them, not only rows whose source moved. */
    g_tui.frame_valid = false;
    sel_clear();
    repaint();
}

static size_t view_visible_rows(void) {
    return g_view.bottom_row > g_view.top_row + 2
         ? g_view.bottom_row - g_view.top_row - 2 : 1;
}

static void view_move(i32 delta) {
    size_t visible = view_visible_rows();
    size_t max_top = g_view.total_rows > visible
                   ? g_view.total_rows - visible : 0;
    if (delta < 0) {
        size_t step = (size_t)(-delta);
        g_view.top = g_view.top > step ? g_view.top - step : 0;
    } else {
        size_t step = (size_t)delta;
        g_view.top = step > max_top - (g_view.top < max_top ? g_view.top
                                                            : max_top)
                   ? max_top : g_view.top + step;
    }
}

static b8 view_close_cell(i32 row, i32 col) {
    if (row < 1 || col < 1) return false;
    size_t r = (size_t)row, c = (size_t)col;
    return r == g_view.top_row + 1 && c >= g_view.right_col - 4
        && c < g_view.right_col;
}


static b8 view_feed(i32 c) {
    if (!g_view.active) return false;
    if (c == -3) { repaint(); return true; }
    if (c < 0) return false;
    if (c == 0x03 || c == 0x04 || c == '\r' || c == '\n' || c == 'q')
        return false;
    if (c == 0x0c) { g_tui.frame_valid = false; repaint(); return true; }
    if (c != 0x1b) return true;

    i32 key = read_escape();
    size_t page = view_visible_rows();
    if (page > 1) page--;
    if (key == KEY_NONE) return false;
    if (key == KEY_UP) view_move(-1);
    else if (key == KEY_DOWN) view_move(1);
    else if (key == KEY_PAGE_UP) view_move(-(i32)page);
    else if (key == KEY_PAGE_DOWN) view_move((i32)page);
    else if (key == KEY_WHEEL_UP) view_move(-3);
    else if (key == KEY_WHEEL_DOWN) view_move(3);
    else if (key == KEY_TOP || key == KEY_HOME) g_view.top = 0;
    else if (key == KEY_BOTTOM || key == KEY_END) {
        size_t visible = view_visible_rows();
        g_view.top = g_view.total_rows > visible
                   ? g_view.total_rows - visible : 0;
    } else if (key == KEY_MOUSE_DOWN) {
        g_view.close_down = view_close_cell(g_mouse.row, g_mouse.col);
        if (!g_view.close_down) sel_begin(g_mouse.row, g_mouse.col);
    } else if (key == KEY_MOUSE_DRAG) {
        g_view.close_down = false;
        sel_extend(g_mouse.row, g_mouse.col);
    } else if (key == KEY_MOUSE_UP) {
        b8 close = g_view.close_down
                && view_close_cell(g_mouse.row, g_mouse.col);
        g_view.close_down = false;
        sel_finish();
        if (close) return false;
    }
    repaint();
    return true;
}

static void view_run(void) {
    g_view.modal = true;
    while (g_view.active && view_feed(rbyte())) { }
    view_close();
}

/* The search box is the notice row, which already sits above the popup. A
 * settings screen is acted on rather than chosen from, so an empty box also
 * carries the keys that act. */
static void pick_search_row(Str query, b8 settings) {
    char row[sizeof g_tui.notice];
    i32 n = snprintf(row, sizeof row, "search: %.*s%s%s", (i32)query.n,
                     query.p, g_tui.comp_n ? "" : "  (no match)",
                     settings && !query.n
                         ? " · Space or Left/Right changes the selected "
                           "row · Esc closes"
                         : "");
    if (n < 0) return;
    size_t len = (size_t)n < sizeof row ? (size_t)n : sizeof row - 1;
    tui_notice((Str){row, len});   
}


typedef enum { PICK_CHOOSE, PICK_SETTINGS, PICK_INFO } PickKind;


static void pick_reselect(Str query, b8 fuzzy, size_t keep) {
    pick_filter(query, fuzzy);
    for (size_t i = 0; i < g_tui.comp_n; i++)
        if (g_tui.comp_idx[i] == keep
            && !popup_separator(&g_tui.cmds[keep])) {
            g_tui.comp_sel = i;
            return;
        }
}


static void pick_settings_act(const TuiSettings *set, Str query, i32 delta) {
    if (!set || !g_tui.comp_n) return;
    size_t row = g_tui.comp_idx[g_tui.comp_sel];
    if (popup_separator(&g_tui.cmds[row])) return;
    set->act(set->ud, row, delta);
    size_t n = set->build(set->ud);
    if (n > set->max) n = set->max;
    if (n > AGENT_MAX_POPUP) n = AGENT_MAX_POPUP;
    if (!n) return;   
    g_tui.cmd_n = n;
    pick_reselect(query, true, row < n ? row : n - 1);
}

/* A picker's whole state, so the same screen can be driven modally at the
 * prompt and byte by byte from the poll while a turn streams. `set` is a copy
 * because a screen left open through a turn outlives its caller's frame; the
 * rows and `ud` it points at belong to the caller and must outlive it too. */
typedef struct {
    b8 active;
    b8 modal;              
    b8 search;
    b8 has_set;
    b8 chosen;
    b8 has_action;
    b8 expired;            
    PickKind kind;
    TuiSettings set;
    TuiPickAction action;   
    size_t out;            
    
    i32 timeout_ms;
    size_t fallback;
    f64 deadline;          
    char query[TUI_PICK_QUERY];
    size_t query_n;
    const TuiCmd *saved_cmds;
    const TuiMark *saved_marks;
    size_t saved_cmd_n;
    b8 saved_dismissed;
    char saved_status[sizeof g_tui.status];
    char saved_notice[sizeof g_tui.notice];
    size_t saved_notice_n;
} Pick;

static Pick g_pick;

static void pick_close(void);

static b8 pick_open(Str title, const TuiCmd *items, const TuiMark *marks,
                    size_t n, size_t search_n, TuiPickAnchor anchor,
                    size_t start, PickKind kind, const TuiSettings *set,
                    const TuiPickAction *act, Str notice, b8 modal) {
    /* The request belongs to this screen: one that never opens must not leave
     * it behind, and a frame painted with no screen up never lifts. */
    size_t keep_off = g_tui.keep_off;
    g_tui.keep_off = SIZE_MAX;
    if (!g_tui.fullscreen || !items || !n) return false;
    if (terminal_too_small()) {
        repaint();
        return false;
    }
    
    if (g_tui.find_open) find_close();
    /* A screen left open during a turn yields to a modal caller, whose answer
     * the agent loop is waiting for. Two screens have no keyboard to share,
     * so anything else is refused. */
    if (g_view.active && modal && !g_view.modal) view_close();
    if (g_view.active) return false;
    if (g_pick.active && modal && !g_pick.modal) pick_close();
    if (g_pick.active) return false;
    if (n > AGENT_MAX_POPUP) n = AGENT_MAX_POPUP;

    memset(&g_pick, 0, sizeof g_pick);
    g_pick.active = true;
    g_pick.modal = modal;
    g_pick.kind = kind;
    if (set) { g_pick.set = *set; g_pick.has_set = true; }
    g_pick.saved_cmds = g_tui.cmds;
    g_pick.saved_marks = g_tui.marks;
    g_pick.saved_cmd_n = g_tui.cmd_n;
    g_pick.saved_dismissed = g_tui.comp_dismissed;
    g_pick.saved_notice_n = g_tui.notice_n;
    memcpy(g_pick.saved_status, g_tui.status, sizeof g_pick.saved_status);
    memcpy(g_pick.saved_notice, g_tui.notice, sizeof g_pick.saved_notice);
    size_t notice_n = notice.n < sizeof g_tui.pick_notice
                    ? notice.n : sizeof g_tui.pick_notice;
    if (notice_n) memcpy(g_tui.pick_notice, notice.p, notice_n);
    g_tui.pick_notice_n = notice_n;
    g_tui.keep_off = keep_off;

    b8 settings = kind == PICK_SETTINGS;
    
    g_pick.search = settings
                 || (kind == PICK_CHOOSE && search_n > TUI_PICK_SEARCH_MIN);

    g_tui.cmds = items;
    g_tui.marks = marks;
    g_tui.cmd_n = n;
    g_tui.path_mode = false;   
    g_tui.picking = true;
    g_tui.comp_n = n;
    g_tui.pick_end = anchor == TUI_PICK_LAST;
    g_tui.comp_sel = start < n ? start : (g_tui.pick_end ? n - 1 : 0);
    for (size_t i = 0; i < n; i++) g_tui.comp_idx[i] = (u16)i;
    if (popup_separator(&items[g_tui.comp_idx[g_tui.comp_sel]]))
        completion_move(g_tui.pick_end ? -1 : 1);
    
    if (g_pick.search)
        pick_search_row((Str){g_pick.query, g_pick.query_n}, settings);
    if (kind == PICK_INFO && !g_tui.notice_n)
        tui_notice(STR("Up/Down reads the page - Enter or Esc closes"));
    if (act) {
        g_pick.action = *act;
        g_pick.has_action = true;
        
        if (act->hint.n && !g_tui.notice_n) tui_notice(act->hint);
    }
    char status[sizeof g_tui.status];
    snprintf(status, sizeof status, "%.*s", (i32)title.n, title.p);
    tui_set_status(status);   
    return true;
}

static b8 pick_requery(void) {
    b8 settings = g_pick.kind == PICK_SETTINGS;
    Str query = { g_pick.query, g_pick.query_n };
    pick_filter(query, settings);
    pick_search_row(query, settings);
    return true;
}


static b8 pick_typed(i32 c) {
    if (!g_pick.search) { repaint(); return true; }
    b8 printable = (c >= 0x20 && c < 0x7f) || c >= 0x80;
    if (!printable || g_pick.query_n + 1 >= sizeof g_pick.query) return true;
    g_pick.query[g_pick.query_n++] = (char)c;
    return pick_requery();
}

static b8 pick_erase(void) {
    if (!g_pick.search) { repaint(); return true; }
    if (g_pick.query_n)
        g_pick.query_n = prev_glyph(g_pick.query, g_pick.query_n);
    return pick_requery();
}

static b8 pick_clear(void) {
    if (!g_pick.search) { repaint(); return true; }
    g_pick.query_n = 0;
    return pick_requery();
}

static b8 pick_act(const TuiSettings *set, i32 delta) {
    if (!g_tui.comp_n) return true;
    pick_settings_act(set, (Str){ g_pick.query, g_pick.query_n }, delta);
    repaint();
    return true;
}

/* The chooser's row action, on the key the list bound it to. The action may
 * reorder the list, so it says where the row went and the selection follows
 * it there rather than staying at an index that now names a different row.
 * An action that leaves no rows closes the screen: an empty list is nothing
 * to choose from, and the rows it drew are gone. */
static b8 pick_row_action(i32 key) {
    const TuiPickAction *a = &g_pick.action;
    if (!g_pick.has_action) return true;
    const TuiPickBinding *binding = NULL;
    for (size_t i = 0; i < a->n_bindings; i++)
        if (a->bindings[i].key == key) { binding = &a->bindings[i]; break; }
    if (!binding) return true;
    size_t row = g_tui.comp_n ? g_tui.comp_idx[g_tui.comp_sel] : SIZE_MAX;
    size_t moved = row;
    size_t n = binding->act(binding->ud, row, &moved);
    if (n > a->max) n = a->max;
    if (n > AGENT_MAX_POPUP) n = AGENT_MAX_POPUP;
    if (!n) return false;
    g_tui.cmd_n = n;
    pick_reselect((Str){ g_pick.query, g_pick.query_n }, false,
                  moved < n ? moved : n - 1);
    repaint();
    return true;
}

/* Enter is the key a reader reaches for, so on a settings row it does what
 * Space does rather than closing the screen, and on a read-only page there
 * is nothing to choose and it closes. */
static b8 pick_enter(const TuiSettings *set) {
    if (g_pick.kind == PICK_INFO) return false;
    if (!g_tui.comp_n) return true;
    if (popup_separator(&g_tui.cmds[g_tui.comp_idx[g_tui.comp_sel]]))
        return true;
    if (g_pick.kind == PICK_SETTINGS) return pick_act(set, 1);
    g_pick.out = g_tui.comp_idx[g_tui.comp_sel];
    g_pick.chosen = true;
    return false;
}

/* An open screen: the completion popup over a picker, the settings rows and
 * a read-only page. It owns the keyboard while it is up, so the composer
 * under it never sees a byte and binds none of these. */
#define PICK_KEYS(X)                                                          \
    X(0x0d, "Enter",     "Choose the row, or act on it",                      \
                                              return pick_enter(set);)        \
    X(0x0a, "",          "",                  return pick_enter(set);)        \
    X(0x03, "Ctrl-C",    "Close without choosing",     return false;)         \
    X(0x04, "",          "",                           return false;)         \
    X(' ',  "Space",     "Act on the settings row",                           \
                        if (!settings) return pick_typed(c);                  \
                        return pick_act(set, 1);)                             \
    X(0x0e, "Ctrl-N",    "Next row",          completion_move(1);)            \
    X(0x10, "Ctrl-P",    "Previous row",      completion_move(-1);)           \
    X(0x06, "Ctrl-F",    "Favorite the row, on a list that offers it",        \
                                              return pick_row_action(0x06);)  \
    X(0x05, "Ctrl-E",    "Configure, on a list that offers it",               \
                                              return pick_row_action(0x05);)  \
    X(0x0f, "Ctrl-O",    "Enter another value, on a list that offers it",     \
                                              return pick_row_action(0x0f);)  \
    X(0x18, "Ctrl-X",    "Delete the row, on a list that offers it",          \
                                              return pick_row_action(0x18);)  \
    X(0x13, "Ctrl-S",    "Set the small model, on a list that offers it",     \
                                              return pick_row_action(0x13);)  \
    X(0x7f, "Backspace", "Delete the query glyph before",                     \
                                              return pick_erase();)           \
    X(0x08, "",          "",                  return pick_erase();)           \
    X(0x15, "Ctrl-U",    "Clear the query",   return pick_clear();)

#define PICK_ESCAPE_KEYS(X)                                                   \
    X(KEY_NONE,  "Esc",   "Close without choosing",    return false;)         \
    X(KEY_DOWN,  "Down",  "Next row",         completion_move(1);)            \
    X(KEY_UP,    "Up",    "Previous row",     completion_move(-1);)           \
    X(KEY_RIGHT, "Right", "Act on the settings row forwards",                 \
                        if (settings) return pick_act(set, 1);)               \
    X(KEY_LEFT,  "Left",  "Act on the settings row backwards",                \
                        if (settings) return pick_act(set, -1);)

static const KeyRow k_pick_rows[]        = { PICK_KEYS(KEY_DOC) };
static const KeyRow k_pick_escape_rows[] = { PICK_ESCAPE_KEYS(KEY_DOC) };

/* One input byte applied to the open screen. False once it has closed, and
 * the caller answers with pick_close. Painting happens here, so a caller
 * that is only forwarding bytes never has to know which key changed what. */
static b8 pick_feed(i32 c) {
    if (!g_pick.active) return false;
    b8 settings = g_pick.kind == PICK_SETTINGS;
    const TuiSettings *set = g_pick.has_set ? &g_pick.set : NULL;

    if (c == -3) { repaint(); return true; }
    
    if (c < 0) return false;
    /* Pasted text is a query rather than keys, so nothing in it picks, acts
     * on a row or cancels. The escape sequence still runs: the paste-end
     * marker is one, and read_escape is what retires the paste state. */
    if (g_tui.pasting && c != 0x1b) {
        if (c == '\r' || c == '\n') return true;
        return pick_typed(c);
    }
    if (c == 0x1b) {
        i32 key = read_escape();
        switch (key) {
            PICK_ESCAPE_KEYS(KEY_CASE)
            default: scroll_key(key); break;
        }
        repaint();
        return true;
    }
    switch (c) {
        PICK_KEYS(KEY_CASE)
        default: return pick_typed(c);
    }
    repaint();
    return true;
}

/* Hands the popup back to the composer. `chosen` and `out` survive it, so a
 * caller reads its answer after the screen is gone. */
static void pick_close(void) {
    if (!g_pick.active) return;
    g_tui.cmds = g_pick.saved_cmds;
    g_tui.marks = g_pick.saved_marks;
    g_tui.cmd_n = g_pick.saved_cmd_n;
    
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.pick_end = false;
    g_tui.picking = false;
    g_tui.comp_dismissed = g_pick.saved_dismissed;
    memcpy(g_tui.notice, g_pick.saved_notice, sizeof g_tui.notice);
    g_tui.notice_n = g_pick.saved_notice_n;
    g_tui.pick_notice_n = 0;
    g_tui.keep_off = SIZE_MAX;
    g_pick.active = false;
    g_pick.modal = false;
    completion_refresh();
    memcpy(g_tui.status, g_pick.saved_status, sizeof g_tui.status);
    repaint();
}

/* A question nobody is at the keyboard for must not hold a turn open past
 * the provider's prompt cache, so a picker given a deadline answers itself
 * with the row it opened on. Any key restarts the wait: a reader who is
 * there reading is not hurried by it. */
static void pick_run(void) {
    g_pick.modal = true;
    while (g_pick.active) {
        if (g_pick.timeout_ms > 0) {
            f64 left = g_pick.deadline - agent_now_seconds();
            if (left <= 0.0) {
                g_pick.out = g_pick.fallback;
                g_pick.chosen = true;
                g_pick.expired = true;
                break;
            }
            
            i32 ms = left > 3600.0 ? 3600 * 1000 : (i32)(left * 1000.0) + 1;
            if (!input_ready(ms)) {
                
                if (g_winch) repaint();
                continue;
            }
            g_pick.deadline = agent_now_seconds()
                            + (f64)g_pick.timeout_ms / 1000.0;
        }
        if (!pick_feed(rbyte())) break;
    }
    pick_close();
}

static b8 pick_impl(Str title, const TuiCmd *items, const TuiMark *marks,
                    size_t n, size_t search_n, TuiPickAnchor anchor,
                    size_t start, PickKind kind, size_t *out,
                    const TuiSettings *set, const TuiPickAction *act,
                    Str notice, i32 timeout_ms) {
    if (!out) return false;
    if (!pick_open(title, items, marks, n, search_n, anchor, start, kind, set,
                   act, notice, true))
        return false;
    
    if (timeout_ms > 0 && start < g_tui.cmd_n) {
        g_pick.timeout_ms = timeout_ms;
        g_pick.fallback = start;
        g_pick.deadline = agent_now_seconds() + (f64)timeout_ms / 1000.0;
    }
    pick_run();
    if (g_pick.chosen) *out = g_pick.out;
    return g_pick.chosen;
}

b8 tui_pick(Str title, const TuiCmd *items, size_t n, TuiPickAnchor anchor,
            size_t start, size_t *out) {
    return pick_impl(title, items, NULL, n, n, anchor, start, PICK_CHOOSE, out,
                     NULL, NULL, (Str){0}, 0);
}

b8 tui_pick_notice(Str title, Str notice, const TuiCmd *items, size_t n,
                   TuiPickAnchor anchor, size_t start, size_t *out) {
    return pick_impl(title, items, NULL, n, n, anchor, start, PICK_CHOOSE, out,
                     NULL, NULL, notice, 0);
}

b8 tui_pick_timed(Str title, Str notice, const TuiCmd *items, size_t n,
                  TuiPickAnchor anchor, size_t start, i32 timeout_ms,
                  size_t *out, b8 *expired) {
    if (expired) *expired = false;
    b8 ok = pick_impl(title, items, NULL, n, n, anchor, start, PICK_CHOOSE,
                      out, NULL, NULL, notice, timeout_ms);
    /* g_pick outlives its screen up to the next one, which is what lets the
     * answer be read after pick_close, and this beside it. */
    if (ok && expired) *expired = g_pick.expired;
    return ok;
}

b8 tui_pick_search_count(Str title, const TuiCmd *items, size_t n,
                         size_t search_n, TuiPickAnchor anchor, size_t start,
                         size_t *out) {
    return pick_impl(title, items, NULL, n, search_n, anchor, start,
                     PICK_CHOOSE, out, NULL, NULL, (Str){0}, 0);
}

b8 tui_pick_action(Str title, size_t n, size_t search_n, TuiPickAnchor anchor,
                   size_t start, const TuiPickAction *act, size_t *out) {
    if (!act || !act->rows || !act->bindings || !act->n_bindings) return false;
    return pick_impl(title, act->rows, NULL, n, search_n, anchor, start,
                     PICK_CHOOSE, out, NULL, act, (Str){0}, 0);
}

void tui_settings(Str title, const TuiSettings *set) {
    if (!set || !set->rows || !set->build || !set->act) return;
    size_t n = set->build(set->ud);
    if (!n) return;
    size_t out = 0;
    (void)pick_impl(title, set->rows, set->marks, n, n, TUI_PICK_FIRST, 0,
                    PICK_SETTINGS, &out, set, NULL, (Str){0}, 0);
}

b8 tui_settings_open(Str title, const TuiSettings *set) {
    if (!set || !set->rows || !set->build || !set->act) return false;
    size_t n = set->build(set->ud);
    if (!n) return false;
    return pick_open(title, set->rows, set->marks, n, n, TUI_PICK_FIRST, 0,
                     PICK_SETTINGS, set, NULL, (Str){0}, false);
}

void tui_info(Str title, const TuiCmd *rows, size_t n) {
    if (!rows || !n) return;
    if (!g_tui.fullscreen) {
        tui_printf("%.*s\n", (i32)title.n, title.p);
        for (size_t i = 0; i < n; i++) {
            if (popup_separator(&rows[i]))
                tui_printf("%.*s\n", (i32)rows[i].name.n, rows[i].name.p);
            else
                tui_printf("%.*s  %.*s\n", (i32)rows[i].name.n,
                           rows[i].name.p, (i32)rows[i].desc.n,
                           rows[i].desc.p);
        }
        return;
    }
    size_t row = 0;
    (void)pick_impl(title, rows, NULL, n, n, TUI_PICK_FIRST, 0, PICK_INFO, &row,
                    NULL, NULL, (Str){0}, 0);
}

b8 tui_info_open(Str title, const TuiCmd *rows, size_t n) {
    if (!rows || !n) return false;
    return pick_open(title, rows, NULL, n, n, TUI_PICK_FIRST, 0, PICK_INFO,
                     NULL, NULL, (Str){0}, false);
}

/* Where a part's runs stand while its bytes are copied. Tabs widen and
 * control bytes vanish, so a run is remeasured as the copy passes its ends
 * rather than shifted by a constant. */
typedef struct {
    const YhlRun *run;
    size_t n;
    size_t k;      
    size_t at;     
    b8 open;
} ViewRuns;

static void view_syn_add(size_t a, size_t b, u8 kind) {
    if (a >= b || g_view.syn_n >= TUI_VIEW_RUNS) return;
    g_bulk.view_syn_a[g_view.syn_n] = (u32)a;
    g_bulk.view_syn_b[g_view.syn_n] = (u32)b;
    g_bulk.view_syn_k[g_view.syn_n++] = kind;
}


static void view_syn_step(ViewRuns *v, size_t i, size_t out, b8 last) {
    if (!v->run) return;
    if (v->open && (last || i == v->run[v->k].end)) {
        view_syn_add(v->at, out, v->run[v->k].semantic);
        v->open = false;
        v->k++;
    }
    if (last) return;
    while (!v->open && v->k < v->n && v->run[v->k].start <= i) {
        if (v->run[v->k].end <= i) { v->k++; continue; }
        v->at = out;
        v->open = true;
    }
}

b8 tui_view_open(Str title, const TuiViewPart *parts, size_t n, size_t start) {
    if (!g_tui.fullscreen || !parts || !n || g_pick.active || g_view.active
        || terminal_too_small())
        return false;
    size_t need = 0, nonempty = 0;
    for (size_t p = 0; p < n; p++) {
        if (!parts[p].text.n) continue;
        if (nonempty && need == TUI_VIEW_BYTES) return false;
        if (nonempty) need++;
        for (size_t i = 0; i < parts[p].text.n; i++) {
            u8 c = (u8)parts[p].text.p[i];
            size_t add = c == '\t' ? 4 : c == '\r' || (c < 0x20 && c != '\n')
                                              || c == 0x7f ? 0 : 1;
            if (add > TUI_VIEW_BYTES - need) return false;
            need += add;
        }
        nonempty++;
    }
    if (!need || !nonempty) return false;

    
    memset(&g_view, 0, sizeof g_view);
    size_t out = 0;
    nonempty = 0;
    for (size_t p = 0; p < n; p++) {
        Str text = parts[p].text;
        if (!text.n) continue;
        if (nonempty) g_bulk.view[out++] = '\n';
        ViewRuns runs = {0};
        if (parts[p].syntax) {
            runs.run = parts[p].syntax->run;
            runs.n = parts[p].syntax->n;
        }
        for (size_t i = 0; i < text.n; i++) {
            view_syn_step(&runs, i, out, false);
            u8 c = (u8)text.p[i];
            if (c == '\t') {
                memcpy(g_bulk.view + out, "    ", 4);
                out += 4;
            } else if (c == '\n' || c >= 0x20) {
                if (c != 0x7f) g_bulk.view[out++] = (char)c;
            }
        }
        view_syn_step(&runs, text.n, out, true);
        nonempty++;
    }
    g_view.active = true;
    g_view.start_line = start;
    g_view.text_n = out;
    g_view.title_n = title.n < sizeof g_view.title ? title.n
                                                   : sizeof g_view.title - 1;
    if (g_view.title_n) memcpy(g_view.title, title.p, g_view.title_n);
    sel_clear();
    repaint();
    return true;
}

b8 tui_screen_open(void) { return g_pick.active || g_view.active; }

/* Remember what a kill key removed, so Ctrl-Y can put it back. Text longer
 * than the buffer is dropped rather than truncated: half a kill is not what
 * the yank promised. */
static void kill_store(const char *s, size_t n) {
    if (!n || n > sizeof g_bulk.kill) return;
    memcpy(g_bulk.kill, s, n);
    g_tui.kill_n = n;
}


static void kill_range(char *buf, size_t *n, size_t *cur, size_t a, size_t b) {
    if (a >= b) return;
    kill_store(buf + a, b - a);
    memmove(buf + a, buf + b, *n - b);
    *n -= b - a;
    *cur = a;
    buf[*n] = '\0';
}

static void edit_delete(char *buf, size_t *n, size_t *cur) {
    if (*cur >= *n) return;
    size_t next = next_glyph(buf, *n, *cur);
    memmove(buf + *cur, buf + next, *n - next);
    *n -= next - *cur;
    buf[*n] = '\0';
}

static void edit_backspace(char *buf, size_t *n, size_t *cur) {
    if (*cur == 0) return;
    size_t prev = prev_glyph(buf, *cur);
    memmove(buf + prev, buf + *cur, *n - *cur);
    *n -= *cur - prev;
    *cur = prev;
    buf[*n] = '\0';
}

static void edit_insert(char c, char *buf, size_t *n, size_t *cur, size_t cap) {
    if (*n + 1 >= cap) return;
    memmove(buf + *cur + 1, buf + *cur, *n - *cur);
    buf[(*cur)++] = c;
    (*n)++;
    buf[*n] = '\0';
}


static void edit_kill_line(char *buf, size_t *n, size_t *cur) {
    size_t end = line_end(buf, *n, *cur);
    if (end == *cur && end < *n) end++;
    kill_range(buf, n, cur, *cur, end);
}


static void edit_yank(char *buf, size_t *n, size_t *cur, size_t cap) {
    size_t k = g_tui.kill_n;
    if (!k || *n + k >= cap) return;
    memmove(buf + *cur + k, buf + *cur, *n - *cur);
    memcpy(buf + *cur, g_bulk.kill, k);
    *cur += k; *n += k; buf[*n] = '\0';
}


#define EDIT_KEYS(X)                                                          \
    X(0x01, "Ctrl-A",    "Start of line, then the previous line",             \
                                        *cur = ctrl_a_start(buf, *cur);)        \
    X(0x02, "Ctrl-B",    "Back one glyph",  *cur = prev_glyph(buf, *cur);)    \
    X(0x04, "Ctrl-D",    "Delete forward",  edit_delete(buf, n, cur);)        \
    X(0x05, "Ctrl-E",    "End of line",     *cur = line_end(buf, *n, *cur);)  \
    X(0x06, "Ctrl-F",    "Forward one glyph",                                 \
                                    *cur = next_glyph(buf, *n, *cur);)        \
    X(0x7f, "Backspace", "Delete back",     edit_backspace(buf, n, cur);)     \
    X(0x08, "",          "",                edit_backspace(buf, n, cur);)     \
    X(0x0b, "Ctrl-K",    "Kill to end of line",  edit_kill_line(buf, n, cur);)\
    X(0x15, "Ctrl-U",    "Kill to start of line",                             \
               kill_range(buf, n, cur, line_start(buf, *cur), *cur);)         \
    X(0x17, "Ctrl-W",    "Kill the word before",                              \
               kill_range(buf, n, cur, prev_word(buf, *cur), *cur);)          \
    X(0x19, "Ctrl-Y",    "Put back the last kill",                            \
                                            edit_yank(buf, n, cur, cap);)

#define EDIT_ESCAPE_KEYS(X)                                                   \
    X(KEY_LEFT,      "Left",       "Back one glyph",                          \
                                        *cur = prev_glyph(buf, *cur);)        \
    X(KEY_RIGHT,     "Right",      "Forward one glyph",                       \
                                        *cur = next_glyph(buf, *n, *cur);)    \
    X(KEY_HOME,      "Home",       "Start of line",                           \
                                        *cur = line_start(buf, *cur);)        \
    X(KEY_END,       "End",        "End of line",                             \
                                        *cur = line_end(buf, *n, *cur);)      \
    X(KEY_PREV_WORD, "Ctrl-Left",  "Back one word",                           \
                                        *cur = prev_word(buf, *cur);)         \
    X(KEY_NEXT_WORD, "Ctrl-Right", "Forward one word",                        \
                                        *cur = next_word(buf, *n, *cur);)     \
    X(KEY_DELETE,    "Delete",     "Delete forward",                          \
                                        edit_delete(buf, n, cur);)            \
    X(KEY_KILL_WORD, "Alt-D",      "Kill the word after",                     \
               kill_range(buf, n, cur, *cur, next_word(buf, *n, *cur));)      \
    X(KEY_KILL_PREV_WORD, "Alt-Backspace", "Kill the word before",            \
               kill_range(buf, n, cur, prev_word(buf, *cur), *cur);)

static const KeyRow k_edit_rows[]        = { EDIT_KEYS(KEY_DOC) };
static const KeyRow k_edit_escape_rows[] = { EDIT_ESCAPE_KEYS(KEY_DOC) };

/* One plain editing byte applied to `buf`. The composer and a question share
 * this, so both answer the same readline keys. Returns whether the byte was
 * an editing key; anything else is the caller's to interpret. */
static b8 edit_byte(i32 c, char *buf, size_t *n, size_t *cur, size_t cap) {
    switch (c) {
        EDIT_KEYS(KEY_CASE)
        default:
            if ((c >= 0x20 && c < 0x7f) || c >= 0x80) {
                edit_insert((char)c, buf, n, cur, cap);
                return true;
            }
            return false;
    }
    return true;
}

/* The escape-sequence half of `edit_byte`: motion and the kills a terminal
 * sends as a sequence rather than a control byte. */
static b8 edit_escape(i32 key, char *buf, size_t *n, size_t *cur) {
    switch (key) {
        EDIT_ESCAPE_KEYS(KEY_CASE)
        default: return false;
    }
    return true;
}

/* A question the composer is borrowed for. The editor is deliberately not the
 * composer's own, since a question wants none of its history recall,
 * completion or shell mode, and a secret answer must not survive in a buffer
 * the next frame paints. The question itself sits in the notice row, where
 * every other answer to a command appears. */
static b8 ask_impl(Str question, b8 secret, char *out, size_t cap,
                   b8 edit, b8 allow_empty) {
    if (!g_tui.fullscreen || !out || cap < 2) return false;
    if (g_tui.find_open) find_close();
    if (g_view.active && !g_view.modal) view_close();
    /* The question owns the composer, which a screen opened during the turn
     * is drawn over. */
    if (g_pick.active && !g_pick.modal) pick_close();
    size_t limit = cap - 1 < TUI_ASK_MAX ? cap - 1 : TUI_ASK_MAX;
    size_t initial_n = 0;
    if (edit) {
        while (initial_n < cap && out[initial_n]) initial_n++;
        if (initial_n == cap || initial_n > limit) return false;
    }

    char saved_input[AGENT_LINE_BUF];
    size_t saved_n = g_tui.input_n, saved_cur = g_tui.input_cur;
    char saved_notice[sizeof g_tui.notice];
    size_t saved_notice_n = g_tui.notice_n;
    size_t saved_comp_n = g_tui.comp_n;
    b8 saved_editing = g_tui.editing;
    char saved_status[sizeof g_tui.status];
    memcpy(saved_input, g_bulk.input, saved_n);
    memcpy(saved_notice, g_tui.notice, sizeof saved_notice);
    memcpy(saved_status, g_tui.status, sizeof saved_status);

    g_tui.ask = true;
    g_tui.ask_secret = secret;
    g_tui.editing = true;
    /* Waiting on the user is not work, whatever the caller was doing when it
     * stopped to ask. */
    memcpy(g_tui.status, "ready", sizeof "ready");
    if (initial_n) memcpy(g_bulk.input, out, initial_n);
    g_tui.input_n = initial_n;
    g_tui.input_cur = initial_n;
    g_tui.comp_n = 0;
    char row[sizeof g_tui.notice];
    i32 rn = snprintf(row, sizeof row, "%.*s  (Esc cancels)",
                      (i32)question.n, question.p);
    tui_notice(rn > 0 ? (Str){ row, (size_t)rn < sizeof row ? (size_t)rn
                                                            : sizeof row - 1 }
                      : question);   

    b8 answered = false;
    for (;;) {
        paste_retire_if_drained();
        i32 c = rbyte();
        if (c == -3) { repaint(); continue; }
        if (c < 0) break;
        if ((c == 0x03 || c == 0x04) && !g_tui.pasting) break;
        if ((c == '\r' || c == '\n') && !g_tui.pasting) {
            answered = allow_empty || g_tui.input_n > 0;
            break;
        }
        if (g_tui.pasting && (c == '\r' || c == '\n')) continue;
        size_t n = g_tui.input_n, cur = g_tui.input_cur;
        if (g_tui.pasting && c != 0x1b) {
            if ((c >= 0x20 && c < 0x7f) || c >= 0x80)
                edit_insert((char)c, g_bulk.input, &n, &cur, limit + 1);
        } else if (c == 0x1b) {
            i32 key = read_escape();
            if (key == KEY_NONE) break;             
            if (!edit_escape(key, g_bulk.input, &n, &cur)) scroll_key(key);
        } else if (c == 0x0c) {
            g_tui.frame_valid = false;
        } else {
            
            edit_byte(c, g_bulk.input, &n, &cur, limit + 1);
        }
        g_tui.input_n = n;
        g_tui.input_cur = cur;
        repaint();
    }

    size_t n = answered ? g_tui.input_n : 0;
    memcpy(out, g_bulk.input, n);
    out[n] = '\0';

    
    memset(g_bulk.input, 0, g_tui.input_n);
    g_tui.ask = false;
    g_tui.ask_secret = false;
    g_tui.editing = saved_editing;
    /* Answering a prompt is not work: the status the ask interrupted is the
     * one the caller is still in, and submitting a field must not leave the
     * form looking busy while it waits for the next. */
    memcpy(g_tui.status, saved_status, sizeof g_tui.status);
    memcpy(g_bulk.input, saved_input, saved_n);
    g_tui.input_n = saved_n;
    g_tui.input_cur = saved_cur;
    g_tui.comp_n = saved_comp_n;
    memcpy(g_tui.notice, saved_notice, sizeof saved_notice);
    g_tui.notice_n = saved_notice_n;
    repaint();
    return answered;
}

b8 tui_ask(Str question, b8 secret, char *out, size_t cap) {
    return ask_impl(question, secret, out, cap, false, false);
}

b8 tui_ask_edit(Str question, b8 allow_empty, char *inout, size_t cap) {
    return ask_impl(question, false, inout, cap, true, allow_empty);
}

typedef enum { ED_EDIT = 0, ED_SUBMIT, ED_EOF, ED_REWIND, ED_EXPAND,
               ED_MODE, ED_ATTACH } EdAction;


static b8 composer_vertical(i32 dir, char *buf, size_t *n, size_t *cur) {
    if (!g_tui.hist_nav && composer_move_row(dir, buf, *n, cur)) return false;
    b8 recalled = history_recall(dir, buf, n, cur);
    
    g_tui.hist_nav = recalled && g_tui.hist && history_browsing(g_tui.hist);
    return recalled;
}

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
    if (c == '\n' && was_cr) return;   

    char run[4];
    size_t run_n = 0;
    if (c == '\r' || c == '\n') run[run_n++] = '\n';
    else if (c == '\t') while (run_n < sizeof run) run[run_n++] = ' ';
    else if ((c >= 0x20 && c < 0x7f) || c >= 0x80) run[run_n++] = (char)c;
    else return;

    char *buf = g_bulk.input;
    size_t n = g_tui.input_n, cur = g_tui.input_cur;
    if (n + run_n >= sizeof g_bulk.input) return;
    memmove(buf + cur + run_n, buf + cur, n - cur);
    memcpy(buf + cur, run, run_n);
    cur += run_n; n += run_n; buf[n] = '\0';
    g_tui.input_n = n;
    g_tui.input_cur = cur;
}

/* ---- the search box ------------------------------------------------------
 * A mode of the composer rather than a screen of its own: it is driven from
 * the same byte-at-a-time editor, so it works at the prompt and while a turn
 * streams, and the draft it covers is never touched.
 */
static void find_requery(void) { find_seek(); }

void tui_set_find_expand(void (*fn)(void *ud), void *ud) {
    g_tui.find_expand = fn;
    g_tui.find_expand_ud = ud;
}

void tui_find_open(void) {
    if (!g_tui.fullscreen) return;
    g_tui.find_open = true;
    g_tui.find_wrapped = false;
    
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.comp_dismissed = true;
    g_tui.esc_armed = false;
    find_requery();   
    repaint();
}


static void find_close(void) {
    g_tui.find_open = false;
    g_tui.find_cur = SIZE_MAX;
    g_tui.find_wrapped = false;
    repaint();
}

static void find_type(i32 c) {
    if (g_tui.find_q_n + 1 > sizeof g_tui.find_q) return;
    g_tui.find_q[g_tui.find_q_n++] = (char)c;
    find_requery();
}

static void find_erase(void) {
    if (!g_tui.find_q_n) return;
    g_tui.find_q_n = prev_glyph(g_tui.find_q, g_tui.find_q_n);
    find_requery();
}

static void find_clear(void) {
    g_tui.find_q_n = 0;
    find_requery();
}

static void find_kill_word(void) {
    g_tui.find_q_n = prev_word(g_tui.find_q, g_tui.find_q_n);
    find_requery();
}

/* The re-render changes every offset, which the next refresh sees as a new
 * epoch and answers by counting again. Unset while no caller offers it, and
 * then the key does nothing rather than reaching another binding. */
static void find_expand_now(void) {
    if (g_tui.find_expand) g_tui.find_expand(g_tui.find_expand_ud);
}

#define FIND_KEYS(X)                                                          \
    X(0x0d, "Enter",     "Previous match",             find_step(-1);)        \
    X(0x0a, "",          "",                           find_step(-1);)        \
    X(0x10, "Ctrl-P",    "Previous match",             find_step(-1);)        \
    X(0x0e, "Ctrl-N",    "Next match",                 find_step(1);)         \
    X(0x07, "Ctrl-G",    "Close the box",              find_close();)         \
    X(0x05, "Ctrl-E",    "Show a capped tool output in full",                 \
                                                       find_expand_now();)    \
    X(0x0c, "Ctrl-L",    "Repaint the screen",  g_tui.frame_valid = false;)   \
    X(0x7f, "Backspace", "Delete the glyph before",     find_erase();)        \
    X(0x08, "",          "",                            find_erase();)        \
    X(0x15, "Ctrl-U",    "Clear the query",             find_clear();)        \
    X(0x17, "Ctrl-W",    "Delete the word before",      find_kill_word();)

#define FIND_ESCAPE_KEYS(X)                                                   \
    X(KEY_UP,   "Up",   "Previous match",   find_step(-1);)                   \
    X(KEY_DOWN, "Down", "Next match",       find_step(1);)                    \
    X(KEY_NONE, "Esc",  "Close the box",    find_close();)                    \
    X(KEY_MOUSE_DOWN, "Click", "Start a selection",                           \
                    sel_begin(g_mouse.row, g_mouse.col); keep_sel = true;)    \
    X(KEY_MOUSE_DRAG, "Drag",  "Extend the selection",                        \
                    sel_extend(g_mouse.row, g_mouse.col); keep_sel = true;)   \
    X(KEY_MOUSE_UP,   "",      "",          sel_finish(); keep_sel = true;)   \
    X(KEY_MOUSE_MOVE, "",      "",          keep_sel = true;)

static const KeyRow k_find_rows[]        = { FIND_KEYS(KEY_DOC) };
static const KeyRow k_find_escape_rows[] = { FIND_ESCAPE_KEYS(KEY_DOC) };


static void find_key(i32 c) {
    if (g_tui.pasting && c != 0x1b) {
        if ((c >= 0x20 && c < 0x7f) || c >= 0x80) find_type(c);
        sel_clear();
        repaint();
        return;
    }
    if (c == 0x1b) {
        i32 key = read_escape();
        b8 keep_sel = false;
        switch (key) {
            FIND_ESCAPE_KEYS(KEY_CASE)
            
            default: scroll_key(key); break;
        }
        if (!keep_sel) sel_clear();
        repaint();
        return;
    }
    switch (c) {
        FIND_KEYS(KEY_CASE)
        default:
            if ((c >= 0x20 && c < 0x7f) || c >= 0x80) find_type(c);
            break;
    }
    sel_clear();
    repaint();
}

/* What one composer key acts on. The bindings say what they did through the
 * flags rather than repeating the bookkeeping, and the trailer in editor_key
 * reads them back once. `done` is an early return: the trailer is skipped,
 * which is what a key that submits or exits wants. */
typedef struct {
    char    *buf;
    size_t   n, cur, cap;
    EdAction action;
    b8       done;
    
    b8       keep_sel;
    b8       recalled;
    
    b8       vertical, keep_nav;
    b8       was_armed;
} Ed;

#define ED_RETURN(e, a) do { (e)->action = (a); (e)->done = true; } while (0)

/* Take the highlighted entry. True when the popup closed with it, which a
 * command does and a path does not: a path is text in a message rather than
 * a command, so accepting one leaves the composer where it is. */
static b8 completion_take(void) {
    b8 path = g_tui.path_mode;
    if (completion_would_change()) completion_accept();
    if (path) return false;
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.comp_dismissed = true;
    return true;
}


static void ed_enter(Ed *e) {
    sel_clear();
    if (!g_tui.comp_n) { ED_RETURN(e, ED_SUBMIT); return; }
    b8 ran = completion_take();
    if (g_tui.attach_n) { ED_RETURN(e, ED_ATTACH); return; }
    ED_RETURN(e, ran ? ED_SUBMIT : ED_EDIT);
}


static void ed_tab(Ed *e) {
    if (!g_tui.comp_n) return;
    sel_clear();
    (void)completion_take();
    ED_RETURN(e, g_tui.attach_n ? ED_ATTACH : ED_EDIT);
}


static void ed_vertical(Ed *e, i32 dir) {
    if (g_tui.comp_n) {
        sel_clear();
        completion_move(dir);
        ED_RETURN(e, ED_EDIT);
        return;
    }
    /* History keeps the keys until the draft is edited, so a recalled entry
     * is browsed rather than walked. */
    e->vertical = true;
    e->keep_nav = true;
    e->recalled = composer_vertical(dir, e->buf, &e->n, &e->cur);
}


static void ed_delete_or_eof(Ed *e) {
    if (!e->n) { ED_RETURN(e, ED_EOF); return; }
    edit_delete(e->buf, &e->n, &e->cur);
}

static void ed_mouse_up(Ed *e) {
    
    b8 hit = !g_tui.sel_active && g_tui.click_down
          && g_tui.click_down == zone_at_cell(g_mouse.row, g_mouse.col);
    if (hit) { g_tui.click_id = g_tui.click_down; e->action = ED_EXPAND; }
    g_tui.click_down = 0;
    sel_finish();
    e->keep_sel = true;
}


static void ed_escape(Ed *e) {
    if (g_tui.comp_n) { g_tui.comp_dismissed = true; return; }
    if (e->was_armed) { e->action = ED_REWIND; return; }
    
    if (g_tui.busy && g_tui.queued_n) {
        g_tui.queued_n = 0;
        tui_notice(STR("queued message cancelled"));
        return;
    }
    if (g_tui.notice_n) { g_tui.notice_n = 0; return; }
    
    if (g_tui.busy && g_tui.interrupt) { *g_tui.interrupt = 1; return; }
    g_tui.esc_armed = true;
    tui_notice(STR("Press Escape again to edit previous message"));
}

static void ed_newline(Ed *e) {
    if (e->n + 1 >= e->cap) return;
    memmove(e->buf + e->cur + 1, e->buf + e->cur, e->n - e->cur);
    e->buf[e->cur++] = '\n';
    e->n++;
    e->buf[e->n] = '\0';
}


static void ed_viewport(Ed *e) {
    e->vertical = true;
    e->keep_nav = true;
}


static void ed_home(Ed *e) {
    if (!e->n) { scroll_to_top(); ed_viewport(e); return; }
    e->cur = line_start(e->buf, e->cur);
}

static void ed_end(Ed *e) {
    if (!e->n) { tui_scroll_to_bottom(); ed_viewport(e); return; }
    e->cur = line_end(e->buf, e->n, e->cur);
}

/* The composer. Its rows are tried before the shared editor's, so Ctrl-D
 * here overrides the editor's plain forward delete rather than duplicating
 * it. Anything this table does not name reaches edit_byte. */

#define COMPOSER_KEYS(X)                                                      \
    X(0x0d, "Enter",  "Send the message, or take the popup entry",            \
                                                        ed_enter(e);)         \
    X(0x0a, "",       "",                               ed_enter(e);)         \
    X('\t', "Tab",    "Complete the popup entry",       ed_tab(e);)           \
    X(0x0e, "Ctrl-N", "Next entry, or the next draft",  ed_vertical(e, 1);)   \
    X(0x10, "Ctrl-P", "Previous entry, or the previous draft",                \
                                                        ed_vertical(e, -1);)  \
    X(0x04, "Ctrl-D", "Delete forward, or exit on an empty composer",         \
                                                        ed_delete_or_eof(e);) \
    X(0x12, "Ctrl-R", "Search the transcript",                                \
                    tui_find_open(); ED_RETURN(e, ED_EDIT);)                  \
    /* A command rather than an edit, so a draft written around it stays. */   \
    X(0x16, "Ctrl-V", "Attach the clipboard's image to the message",           \
                    g_tui.attach_n = 0; e->action = ED_ATTACH;)                \
    X(0x0c, "Ctrl-L", "Repaint the screen",                                   \
                    g_tui.frame_valid = false;)

// Shift-Tab is a command rather than an edit, so the draft remains.
#define COMPOSER_ESCAPE_KEYS(X)                                               \
    X(KEY_NONE,      "Esc",       "Dismiss, or go back a message",            \
                                                        ed_escape(e);)        \
    X(KEY_UP,        "Up",        "Previous entry, or the previous draft",    \
                                                        ed_vertical(e, -1);)  \
    X(KEY_DOWN,      "Down",      "Next entry, or the next draft",            \
                                                        ed_vertical(e, 1);)   \
    X(KEY_HOME,      "Home",      "Start of line, or the top of the "         \
                                  "transcript on an empty composer",          \
                                                        ed_home(e);)          \
    X(KEY_END,       "End",       "End of line, or the bottom of the "        \
                                  "transcript on an empty composer",          \
                                                        ed_end(e);)           \
    X(KEY_TOP,       "Ctrl-Home", "Top of the transcript",                    \
                                        scroll_to_top(); ed_viewport(e);)     \
    X(KEY_BOTTOM,    "Ctrl-End",  "Bottom of the transcript",                 \
                                        tui_scroll_to_bottom();               \
                                        ed_viewport(e);)                      \
    X(KEY_NEWLINE,   "Alt/Shift-Enter", "Insert a line break",                \
                                                        ed_newline(e);)       \
    X(KEY_SHIFT_TAB, "Shift-Tab", "Switch between Build and Plan mode",       \
                    e->action = ED_MODE;)                                     \
    X(KEY_MOUSE_DOWN, "Click",    "Start a selection",                        \
                    sel_begin(g_mouse.row, g_mouse.col);                      \
                    e->keep_sel = true;                                       \
                    g_tui.click_down = zone_at_cell(g_mouse.row, g_mouse.col);)\
    X(KEY_MOUSE_DRAG, "Drag",     "Extend the selection",                     \
                    sel_extend(g_mouse.row, g_mouse.col);                     \
                    e->keep_sel = true;)                                      \
    X(KEY_MOUSE_MOVE, "",         "",                                         \
                    g_tui.hover_id = zone_at_cell(g_mouse.row, g_mouse.col);  \
                    e->keep_sel = true; e->vertical = true;                   \
                    e->keep_nav = true;)                                      \
    X(KEY_MOUSE_UP,   "",         "",                   ed_mouse_up(e);)

static const KeyRow k_composer_rows[] = { COMPOSER_KEYS(KEY_DOC) };
static const KeyRow k_composer_escape_rows[] = {
    COMPOSER_ESCAPE_KEYS(KEY_DOC)
};

/* The escape half of the composer. Its own rows first, then the two layers
 * below it: the shared editor's motion and kills, and the viewport keys a
 * list drawn over the transcript must not cost the reader. */
static void composer_escape(Ed *e) {
    i32 key = read_escape();
    switch (key) {
        COMPOSER_ESCAPE_KEYS(KEY_CASE)
        default:
            if (edit_escape(key, e->buf, &e->n, &e->cur)) break;
            
            if (scroll_key(key)) { e->vertical = true; e->keep_nav = true; }
            break;
    }
}

/* One input byte applied to the shared composer. The caller decides whether a
 * submit is honoured, so the same editor drives the prompt and the
 * keep-typing-while-busy path. */
static EdAction editor_key(i32 c) {
    if (g_tui.find_open) { find_key(c); return ED_EDIT; }
    if (g_tui.pasting && c != 0x1b) { paste_byte(c); return ED_EDIT; }

    Ed ed = {
        .buf = g_bulk.input,
        .n = g_tui.input_n,
        .cur = g_tui.input_cur,
        .cap = sizeof g_bulk.input,
        .action = ED_EDIT,
        .was_armed = g_tui.esc_armed,
    };
    Ed *e = &ed;
    g_tui.esc_armed = false;
    if (e->was_armed) g_tui.notice_n = 0;

    size_t before_n = e->n;

    if (c == 0x1b) composer_escape(e);
    else switch (c) {
        COMPOSER_KEYS(KEY_CASE)
        default: edit_byte(c, e->buf, &e->n, &e->cur, e->cap); break;
    }
    if (e->done) return e->action;

    if (!e->keep_sel) sel_clear();
    
    if (!e->keep_nav) g_tui.hist_nav = false;
    if (!e->vertical) g_tui.goal_col_valid = false;
    g_tui.input_n = e->n;
    g_tui.input_cur = e->cur;
    if (e->recalled) g_tui.comp_dismissed = true;
    else if (e->n != before_n) g_tui.comp_dismissed = false;
    completion_refresh();
    return e->action;
}


#define KEY_CONTEXTS(X)                                                       \
    X("[composer]",          k_composer_rows)                                 \
    X("[composer]",          k_composer_escape_rows)                          \
    X("[line editing]",      k_edit_rows)                                     \
    X("[line editing]",      k_edit_escape_rows)                              \
    X("[transcript search]", k_find_rows)                                     \
    X("[transcript search]", k_find_escape_rows)                              \
    X("[lists and screens]", k_pick_rows)                                     \
    X("[lists and screens]", k_pick_escape_rows)

static const KeyContext k_key_contexts[] = {
#define X(name, rows) { name, rows, sizeof rows / sizeof rows[0] },
    KEY_CONTEXTS(X)
#undef X
};
#define KEY_CONTEXT_N (sizeof k_key_contexts / sizeof k_key_contexts[0])

/* Every row plus a heading for each table, which overcounts the tables that
 * share one. The page has to fit the caller's array, so growing a table past
 * it is a build failure and not a page that silently stops early. */
#define X(name, rows) + (sizeof rows / sizeof rows[0]) + 1
enum { KEY_ROW_MAX = 0 KEY_CONTEXTS(X) };
#undef X
_Static_assert(KEY_ROW_MAX <= AGENT_MAX_KEY_ROWS,
               "the keys page outgrew AGENT_MAX_KEY_ROWS");

/* A duplicate inside one table is a duplicate case value and never gets this
 * far. What the compiler cannot see is the seam between a context's two
 * tables: they are separate switches, so the same label in both reaches the
 * page twice and describes a key that behaves like neither row. Reported
 * rather than asserted, since a wrong page is a wrong page and not a reason
 * to refuse to start. */
static void keys_selfcheck(void) {
    for (size_t a = 0; a < KEY_CONTEXT_N; a++) {
        const KeyContext *ka = &k_key_contexts[a];
        for (size_t i = 0; i < ka->n; i++) {
            const char *key = ka->rows[i].key;
            if (!key || !key[0]) continue;
            for (size_t b = 0; b <= a; b++) {
                const KeyContext *kb = &k_key_contexts[b];
                if (strcmp(kb->name, ka->name) != 0) continue;
                size_t upto = b == a ? i : kb->n;
                for (size_t j = 0; j < upto; j++)
                    if (kb->rows[j].key && !strcmp(kb->rows[j].key, key))
                        agent_log(AGENT_LOG_WARN,
                                  "keys: %s lists %s twice", ka->name, key);
            }
        }
    }
}

size_t tui_key_rows(TuiCmd *rows, size_t max) {
    if (!rows) return 0;
    size_t n = 0;
    const char *heading = NULL;
    for (size_t ctx = 0; ctx < KEY_CONTEXT_N; ctx++) {
        const KeyContext *k = &k_key_contexts[ctx];
        
        if (!heading || strcmp(heading, k->name) != 0) {
            if (n == max) return n;
            heading = k->name;
            rows[n++] = tui_separator(str_c(heading));
        }
        for (size_t i = 0; i < k->n; i++) {
            if (!k->rows[i].key || !k->rows[i].key[0]) continue;
            if (n == max) return n;
            rows[n++] = (TuiCmd){ str_c(k->rows[i].key),
                                  str_c(k->rows[i].help) };
        }
    }
    return n;
}


static void composer_clear(void) {
    g_tui.notice_n = 0;
    g_tui.esc_armed = false;
    g_bulk.input[0] = '\0';
    g_tui.input_n = 0;
    g_tui.input_cur = 0;
    g_tui.comp_n = 0;
    g_tui.comp_sel = 0;
    g_tui.comp_dismissed = false;
    g_tui.draft_n = 0;
    g_tui.hist_nav = false;
    g_tui.input_top = 0;
    g_tui.goal_col_valid = false;
    if (g_tui.hist) history_reset_cursor(g_tui.hist);
}

static struct {
    b8  (*fn)(Str line, void *ud);
    void *ud;
} g_busy;

void tui_set_busy_command(b8 (*fn)(Str line, void *ud), void *ud) {
    g_busy.fn = fn;
    g_busy.ud = ud;
}

/* Enter while a turn is in flight. A slash command is offered to the hook,
 * which takes only the ones that leave the running turn alone. One ordinary
 * message moves to the follow-up queue, leaving the composer free for another
 * draft. The composer is cleared before a command hook runs, since a screen
 * it opens owns the popup afterwards, and refused command text is handed
 * back. */
static void busy_submit(void) {
    if (!g_tui.input_n) return;
    if (g_bulk.input[0] != '/') {
        if (g_tui.queued_n) {
            tui_notice(STR("a message is already queued; Esc cancels it"));
            return;
        }
        memcpy(g_bulk.queued, g_bulk.input, g_tui.input_n);
        g_tui.queued_n = g_tui.input_n;
        if (g_tui.hist)
            history_add(g_tui.hist, (Str){g_bulk.input, g_tui.input_n});
        composer_clear();
        tui_notice(STR("message queued; Esc cancels it"));
        return;
    }
    if (!g_busy.fn) return;
    /* A line longer than any command is prose that happens to start with a
     * slash. It remains a draft because the fixed command handoff cannot hold
     * it, rather than being truncated into a different command. */
    char cmd[256];
    size_t n = g_tui.input_n;
    if (n >= sizeof cmd) return;
    memcpy(cmd, g_bulk.input, n);
    cmd[n] = '\0';
    composer_clear();
    if (g_busy.fn((Str){cmd, n}, g_busy.ud)) {
        if (g_tui.hist) history_add(g_tui.hist, (Str){cmd, n});
        return;
    }
    tui_set_input((Str){cmd, n});
}

/* A click on a block's truncation tail is a command rather than an edit, so
 * mid-turn it reaches the same hook the prompt would send it to. Nothing is
 * recalled from it: the gesture is the request, and it left no draft. */
static void busy_expand(void) {
    if (!g_busy.fn) return;
    char cmd[32];
    i32 len = snprintf(cmd, sizeof cmd, "/expand %u", g_tui.click_id);
    if (len > 0) g_busy.fn((Str){cmd, (size_t)len}, g_busy.ud);
}

/* Ctrl-V mid-turn. The hook refuses it like any other command that belongs
 * to the next message, which is an answer rather than a key that did
 * nothing; the draft it was pressed over is untouched either way. */

static size_t attach_command(char *out, size_t cap) {
    i32 len = g_tui.attach_n
            ? snprintf(out, cap, "/attach %.*s", (i32)g_tui.attach_n,
                       g_bulk.attach)
            : snprintf(out, cap, "/attach");
    g_tui.attach_n = 0;
    if (len <= 0) return 0;
    return (size_t)len < cap ? (size_t)len : cap - 1;
}

static void busy_attach(void) {
    char cmd[AGENT_MAX_PATH + 16];
    size_t n = attach_command(cmd, sizeof cmd);
    if (g_busy.fn && n) g_busy.fn((Str){ cmd, n }, g_busy.ud);
}


static void poll_input(void);

/* The poll path drains input without ever parking in read, so the beacon
 * rbyte writes on its way into a read reports a count from before that drain
 * and nothing corrects it while a turn holds the loop. Report the count on
 * the way out so a test can tell input taken from a screen at rest. */
void tui_poll_input(void) {
    poll_input();
    input_notice();
}

static void poll_input(void) {
    if (!g_tui.fullscreen) return;
    if (g_view.active && !g_view.modal) {
        while (g_view.active && !g_tui.input_eof && input_ready(0)) {
            i32 c = rbyte();
            if (c == -2) continue;
            if (c < 0 && c != -3) {
                g_tui.input_eof = true;
                view_close();
                return;
            }
            if (!view_feed(c)) view_close();
        }
        if (g_view.active || !input_buffered()) {
            if (g_winch != 0) repaint();
            return;
        }
    }
    /* A screen opened mid-turn owns the keyboard, and reading it here is what
     * keeps it live: nothing below it may take a byte from under it. */
    if (g_pick.active && !g_pick.modal) {
        while (g_pick.active && !g_tui.input_eof && input_ready(0)) {
            i32 c = rbyte();
            if (c == -2) continue;
            if (c < 0 && c != -3) { g_tui.input_eof = true; pick_close(); return; }
            if (!pick_feed(c)) pick_close();
        }
        
        if (g_pick.active || !input_buffered()) {
            if (g_winch != 0) repaint();
            return;
        }
    }
    b8 dirty = g_winch != 0;
    
    if (g_tui.activity_n
        && agent_now_seconds() - g_tui.last_paint >= 1.0 / 10.0)
        dirty = true;
    while (!g_tui.input_eof && input_ready(0)) {
        i32 c = rbyte();
        if (c == -3) { dirty = true; continue; }
        if (c == -2) continue;
        if (c < 0) { g_tui.input_eof = true; break; }
        /* Ctrl-C and Ctrl-D belong to the prompt rather than to a live turn.
         * Enter reaches the editor so an open popup can complete an entry and
         * so a command the turn can afford is submitted where it stands. */
        if ((c == 0x03 || c == 0x04) && !g_tui.pasting) continue;
        EdAction action = editor_key(c);
        if (action == ED_SUBMIT) {
            busy_submit();
            if (g_pick.active && !g_pick.modal) { repaint(); return; }
        } else if (action == ED_EXPAND) busy_expand();
        else if (action == ED_ATTACH) busy_attach();
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

    /* A screen the user opened mid-turn stays up when the turn ends; the
     * prompt waits behind it rather than reading keys meant for it. */
    if (g_view.active && !g_view.modal) view_run();
    if (g_pick.active && !g_pick.modal) pick_run();

    g_tui.editing = true;
    tui_set_status(g_tui.setup_hint.n ? "setup" : "ready"); 

    for (;;) {
        paste_retire_if_drained();
        i32 c = rbyte();
        if (c == -3) { repaint(); continue; }
        if (c == -2 || (c == 0x03 && !g_tui.pasting)) {
            composer_clear();
            repaint();
            continue;
        }
        if (c < 0) { *out_n = 0; return false; }

        EdAction action = editor_key(c);
        if (action == ED_EOF) { *out_n = 0; return false; }
        if (action == ED_REWIND || action == ED_EXPAND || action == ED_MODE
            || action == ED_ATTACH) {
            
            char cmd[AGENT_MAX_PATH + 16];
            size_t n;
            if (action == ED_ATTACH) n = attach_command(cmd, sizeof cmd);
            else {
                i32 len;
                if (action == ED_REWIND)
                    len = snprintf(cmd, sizeof cmd, "/rewind");
                else if (action == ED_MODE)
                    len = snprintf(cmd, sizeof cmd, "/mode");
                else
                    len = snprintf(cmd, sizeof cmd, "/expand %u", g_tui.click_id);
                n = len > 0 ? (size_t)len : 0;
            }
            if (n >= cap) n = cap - 1;
            memcpy(buf, cmd, n);
            buf[n] = '\0';
            *out_n = n;
            repaint();
            return true;
        }
        if (action == ED_SUBMIT) {
            size_t n = g_tui.input_n < cap ? g_tui.input_n : cap - 1;
            memcpy(buf, g_bulk.input, n);
            buf[n] = '\0';
            /* Here, so the slash commands the caller consumes are recallable
             * too. */
            if (g_tui.hist) history_add(g_tui.hist, (Str){buf, n});
            composer_clear();
            *out_n = n;
            /* A message is handed to the caller and comes back as a turn, so
             * this frame must not still read ready: an emptied composer under
             * an idle status is a message that went nowhere, and it is the
             * one frame at which a starting turn looks like a finished one.
             * A command is answered where it stands and a blank line is no
             * submission, so neither claims work the caller is not doing. */
            if (n && buf[0] != '/') tui_set_status("working"); 
            else repaint();
            return true;
        }
        
        if (!input_ready(0)) repaint();
    }
}
