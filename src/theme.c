/* Colour themes.
 *
 * A slot fixes its attributes (bold, italic, underline, strike) and whether it
 * sets the foreground or the background; a theme gives each slot one colour.
 * The SGR strings are built once per load, so painting reads a prebuilt
 * string. A theme file only ever supplies parsed numbers, never bytes that go
 * to the terminal.
 */
#include "agent.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define THEME_SGR_MAX   32
#define THEME_PAGE_MAX  64
#define THEME_BUILTIN_N 5

enum {
    THEME_ATTR_BOLD = 1,
    THEME_ATTR_ITALIC = 2,
    THEME_ATTR_UNDERLINE = 4,
    THEME_ATTR_STRIKE = 8
};

#define TD    {THEME_COLOUR_DEFAULT, 0, 0, 0, 0}
#define TI(n) {THEME_COLOUR_INDEX, (n), 0, 0, 0}
#define TH(x)                                                                \
    {THEME_COLOUR_RGB, 0, (u8)(((x) >> 16) & 0xff), (u8)(((x) >> 8) & 0xff), \
     (u8)((x) & 0xff)}
#define FG 0
#define BG 1
#define B  THEME_ATTR_BOLD
#define IT THEME_ATTR_ITALIC
#define UL THEME_ATTR_UNDERLINE
#define ST THEME_ATTR_STRIKE

/* NOTE: the columns are dark, light, kanagawa-wave, kanagawa-dragon and
 * kanagawa-lotus. The kanagawa values come from kanagawa.nvim (MIT,
 * rebelot), its palette and its themes.lua role mapping. The foreground
 * slots of light and kanagawa-lotus are darkened to at least 5:1 against
 * the page; the unit suite checks it. */
#define THEME_SLOTS(X)                                                         \
    X(PAGE_FG, "page_fg", FG, 0, TD, TI(236), TH(0xDCD7BA), TH(0xC5C9C5),      \
      TH(0x545464))                                                            \
    X(PAGE_BG, "page_bg", BG, 0, TD, TI(231), TH(0x1F1F28), TH(0x181616),      \
      TH(0xF2ECBC))                                                            \
    X(TEXT, "text", FG, 0, TI(253), TI(236), TH(0xDCD7BA), TH(0xC5C9C5),       \
      TH(0x545464))                                                            \
    X(MUTED, "muted", FG, 0, TI(245), TI(241), TH(0x727169), TH(0x7A8382),     \
      TH(0x64635D))                                                            \
    X(SUBTLE, "subtle", FG, 0, TI(250), TI(239), TH(0xC8C093), TH(0xA6A69C),   \
      TH(0x5E5B51))                                                            \
    X(ACCENT, "accent", FG, B, TI(81), TI(24), TH(0x7FB4CA), TH(0x8BA4B0),     \
      TH(0x3A6979))                                                            \
    X(MARKER, "marker", FG, B, TI(75), TI(25), TH(0x7E9CD8), TH(0x949FB5),     \
      TH(0x496392))                                                            \
    X(SUCCESS, "success", FG, B, TI(114), TI(22), TH(0x98BB6C), TH(0x87A987),  \
      TH(0x55693C))                                                            \
    X(WARNING, "warning", FG, B, TI(221), TI(94), TH(0xFF9E3B), TH(0xFF9E3B),  \
      TH(0x8F5500))                                                            \
    X(ERROR, "error", FG, B, TI(203), TI(160), TH(0xE82424), TH(0xE82424),     \
      TH(0xC21E1E))                                                            \
    X(THINKING, "thinking", FG, B, TI(177), TI(91), TH(0x957FB8),              \
      TH(0x8992A7), TH(0x624C83))                                              \
    X(MONO, "mono", FG, 0, TI(180), TI(58), TH(0xE6C384), TH(0xC4B28A),        \
      TH(0x696438))                                                            \
    X(STRIKE, "strike", FG, ST, TI(253), TI(236), TH(0xDCD7BA), TH(0xC5C9C5),  \
      TH(0x545464))                                                            \
    X(USER_RULE, "user_rule", FG, 0, TI(81), TI(24), TH(0x7FB4CA),             \
      TH(0x8BA4B0), TH(0x3A6979))                                              \
    X(LINK, "link", FG, UL, TI(81), TI(24), TH(0x7FB4CA), TH(0x8BA4B0),        \
      TH(0x3A6979))                                                            \
    X(LINK_HOVER, "link_hover", FG, B | UL, TI(81), TI(24), TH(0x7FB4CA),      \
      TH(0x8BA4B0), TH(0x3A6979))                                              \
    X(PANEL_BG, "panel_bg", BG, 0, TI(236), TI(254), TH(0x2A2A37),             \
      TH(0x282727), TH(0xE7DBA0))                                              \
    X(USER_BG, "user_bg", BG, 0, TI(238), TI(253), TH(0x363646), TH(0x393836), \
      TH(0xE4D794))                                                            \
    X(CODE_BG, "code_bg", BG, 0, TI(235), TI(255), TH(0x181820), TH(0x12120F), \
      TH(0xDCD5AC))                                                            \
    X(USER_CODE_BG, "user_code_bg", BG, 0, TI(236), TI(254), TH(0x2A2A37),     \
      TH(0x282727), TH(0xE7DBA0))                                              \
    X(POPUP_BG, "popup_bg", BG, 0, TI(237), TI(252), TH(0x223249),             \
      TH(0x223249), TH(0xC7D7E0))                                              \
    X(POPUP_SELECTED_BG, "popup_selected_bg", BG, 0, TI(24), TI(153),          \
      TH(0x2D4F67), TH(0x2D4F67), TH(0x9FB5C9))                                \
    X(FIND_BG, "find_bg", BG, 0, TI(94), TI(223), TH(0x49443C), TH(0x49443C),  \
      TH(0xF9D791))                                                            \
    X(FIND_CURRENT_BG, "find_current_bg", BG, 0, TI(214), TI(214),             \
      TH(0xFF9E3B), TH(0xFF9E3B), TH(0xE98A00))                                \
    X(FIND_CURRENT_FG, "find_current_fg", FG, 0, TI(16), TI(16), TH(0x16161D), \
      TH(0x0D0C0C), TH(0x1F1F28))                                              \
    X(SYNTAX_COMMENT, "syntax_comment", FG, IT, TI(245), TI(241),              \
      TH(0x727169), TH(0x737C73), TH(0x64635D))                                \
    X(SYNTAX_STRING, "syntax_string", FG, 0, TI(114), TI(22), TH(0x98BB6C),    \
      TH(0x8A9A7B), TH(0x55693C))                                              \
    X(SYNTAX_NUMBER, "syntax_number", FG, 0, TI(221), TI(94), TH(0xD27E99),    \
      TH(0xA292A3), TH(0x954C65))                                              \
    X(SYNTAX_KEYWORD, "syntax_keyword", FG, 0, TI(177), TI(91), TH(0x957FB8),  \
      TH(0x8992A7), TH(0x624C83))                                              \
    X(SYNTAX_TYPE, "syntax_type", FG, 0, TI(81), TI(24), TH(0x7AA89F),         \
      TH(0x8EA4A2), TH(0x4B6863))                                              \
    X(SYNTAX_FUNCTION, "syntax_function", FG, 0, TI(75), TI(25), TH(0x7E9CD8), \
      TH(0x8BA4B0), TH(0x496392))                                              \
    X(SYNTAX_BUILTIN, "syntax_builtin", FG, 0, TI(180), TI(58), TH(0xFFA066),  \
      TH(0xB6927B), TH(0x975100))

typedef struct {
    const char *name;
    u8 bg;
    u8 attrs;
} ThemeSlotSpec;

#define THEME_SPEC_ROW(id, nm, bg, at, d, l, w, dr, lo) \
    [THEME_##id] = {nm, bg, at},
#define THEME_VALUE_ROW(id, nm, bg, at, d, l, w, dr, lo) \
    [THEME_##id] = {d, l, w, dr, lo},
#define THEME_COUNT_ROW(...) +1

/* INVARIANT: every ThemeSlot has exactly one THEME_SLOTS row; a missing row
 * would leave a slot zeroed, which reads as the terminal default. */
_Static_assert(0 THEME_SLOTS(THEME_COUNT_ROW) == THEME_SLOT_N,
               "THEME_SLOTS must have one row per ThemeSlot");

static const ThemeSlotSpec k_theme_slots[THEME_SLOT_N] = {
    THEME_SLOTS(THEME_SPEC_ROW)};

static const ThemeColour k_theme_builtin[THEME_SLOT_N][THEME_BUILTIN_N] = {
    THEME_SLOTS(THEME_VALUE_ROW)};

/* INVARIANT: the order of these names is the column order of THEME_SLOTS,
 * and "dark" comes first because it is the fallback. */
static const char *const k_theme_names[] = {
    "dark", "light", "kanagawa-wave", "kanagawa-dragon", "kanagawa-lotus"};
_Static_assert(sizeof k_theme_names / sizeof k_theme_names[0]
                   == THEME_BUILTIN_N,
               "one built-in name per THEME_SLOTS column");

#undef TD
#undef TI
#undef TH
#undef FG
#undef BG
#undef B
#undef IT
#undef UL
#undef ST

static struct {
    char sgr[THEME_SLOT_N][THEME_SGR_MAX];
    char page[THEME_PAGE_MAX];
    char cursor[THEME_SGR_MAX];
    char name[AGENT_MAX_THEME_NAME + 1];
    b8 truecolor;
} g_theme;

/* ---- colour values ------------------------------------------------------- */

static i32 theme_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

b8 theme_colour_parse(Str v, ThemeColour *out) {
    ThemeColour c = {0};
    if (str_eq(v, STR("default"))) {
        c.kind = THEME_COLOUR_DEFAULT;
    } else if (v.n == 7 && v.p[0] == '#') {
        u8 rgb[3];
        for (size_t i = 0; i < 3; i++) {
            i32 hi = theme_hex_digit(v.p[1 + i * 2]);
            i32 lo = theme_hex_digit(v.p[2 + i * 2]);
            if (hi < 0 || lo < 0) return false;
            rgb[i] = (u8)(hi * 16 + lo);
        }
        c.kind = THEME_COLOUR_RGB;
        c.r = rgb[0];
        c.g = rgb[1];
        c.b = rgb[2];
    } else if (v.n >= 1 && v.n <= 3) {
        u32 n = 0;
        for (size_t i = 0; i < v.n; i++) {
            if (v.p[i] < '0' || v.p[i] > '9') return false;
            n = n * 10 + (u32)(v.p[i] - '0');
        }
        if (n > 255) return false;
        c.kind = THEME_COLOUR_INDEX;
        c.index = (u8)n;
    } else {
        return false;
    }
    *out = c;
    return true;
}

static u32 theme_dist(u32 r, u32 g, u32 b, u32 r2, u32 g2, u32 b2) {
    u32 dr = r > r2 ? r - r2 : r2 - r;
    u32 dg = g > g2 ? g - g2 : g2 - g;
    u32 db = b > b2 ? b - b2 : b2 - b;
    return dr * dr + dg * dg + db * db;
}

static u32 theme_cube_step(u8 v, u32 *level) {
    static const u32 levels[6] = {0, 95, 135, 175, 215, 255};
    u32 best = 0;
    for (u32 i = 1; i < 6; i++) {
        u32 d = levels[i] > v ? levels[i] - v : v - levels[i];
        u32 bd = levels[best] > v ? levels[best] - v : v - levels[best];
        if (d < bd) best = i;
    }
    *level = levels[best];
    return best;
}

u8 theme_nearest_256(u8 r, u8 g, u8 b) {
    u32 lr, lg, lb;
    u32 ir = theme_cube_step(r, &lr);
    u32 ig = theme_cube_step(g, &lg);
    u32 ib = theme_cube_step(b, &lb);
    u32 cube_d = theme_dist(r, g, b, lr, lg, lb);
    u32 grey_i = 0, grey_d = UINT32_MAX;
    for (u32 i = 0; i < 24; i++) {
        u32 v = 8 + 10 * i;
        u32 d = theme_dist(r, g, b, v, v, v);
        if (d < grey_d) {
            grey_d = d;
            grey_i = i;
        }
    }
    if (grey_d < cube_d) return (u8)(232 + grey_i);
    return (u8)(16 + 36 * ir + 6 * ig + ib);
}

/* ---- SGR strings --------------------------------------------------------- */

static size_t theme_colour_params(char *out, size_t cap, b8 bg, ThemeColour c) {
    char lead = bg ? '4' : '3';
    i32 n;
    if (c.kind == THEME_COLOUR_RGB && g_theme.truecolor)
        n = snprintf(out, cap, "%c8;2;%u;%u;%u", lead, (unsigned)c.r,
                     (unsigned)c.g, (unsigned)c.b);
    else if (c.kind == THEME_COLOUR_RGB)
        n = snprintf(out, cap, "%c8;5;%u", lead,
                     (unsigned)theme_nearest_256(c.r, c.g, c.b));
    else if (c.kind == THEME_COLOUR_INDEX)
        n = snprintf(out, cap, "%c8;5;%u", lead, (unsigned)c.index);
    else
        n = snprintf(out, cap, "%c9", lead);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

static void theme_build(ThemeSlot s, ThemeColour c) {
    const ThemeSlotSpec *sp = &k_theme_slots[s];
    static const struct {
        u8 flag;
        char code;
    } codes[] = {{THEME_ATTR_BOLD, '1'},
                 {THEME_ATTR_ITALIC, '3'},
                 {THEME_ATTR_UNDERLINE, '4'},
                 {THEME_ATTR_STRIKE, '9'}};
    char attrs[2 * sizeof codes / sizeof codes[0] + 1] = {0};
    size_t an = 0;
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        if (!(sp->attrs & codes[i].flag)) continue;
        attrs[an++] = codes[i].code;
        attrs[an++] = ';';
    }
    char colour[THEME_SGR_MAX];
    char *out = g_theme.sgr[s];
    i32 n = 0;
    if (theme_colour_params(colour, sizeof colour, sp->bg, c))
        n = snprintf(out, THEME_SGR_MAX, "\033[%s%sm", attrs, colour);
    if (n <= 0 || n >= THEME_SGR_MAX) out[0] = '\0';
}

/* The reset that returns to the page colours. It stays empty when both are
 * the terminal's own, so a theme that leaves the page alone sends nothing. */
static void theme_build_page(ThemeColour fg, ThemeColour bg) {
    char *out = g_theme.page;
    out[0] = '\0';
    if (fg.kind == THEME_COLOUR_DEFAULT && bg.kind == THEME_COLOUR_DEFAULT)
        return;
    char f[THEME_SGR_MAX], b[THEME_SGR_MAX];
    if (!theme_colour_params(f, sizeof f, false, fg)
        || !theme_colour_params(b, sizeof b, true, bg))
        return;
    i32 n = snprintf(out, THEME_PAGE_MAX, "\033[0;%s;%sm", f, b);
    if (n <= 0 || n >= THEME_PAGE_MAX) out[0] = '\0';
}

static b8 theme_rgb(ThemeColour c, u8 rgb[3]) {
    static const u8 levels[6] = {0, 95, 135, 175, 215, 255};
    if (c.kind == THEME_COLOUR_RGB) {
        rgb[0] = c.r;
        rgb[1] = c.g;
        rgb[2] = c.b;
        return true;
    }
    if (c.kind != THEME_COLOUR_INDEX || c.index < 16) return false;
    if (c.index >= 232) {
        rgb[0] = rgb[1] = rgb[2] = (u8)(8 + 10 * (c.index - 232));
        return true;
    }
    u32 i = c.index - 16u;
    rgb[0] = levels[i / 36];
    rgb[1] = levels[i / 6 % 6];
    rgb[2] = levels[i % 6];
    return true;
}

/* The cursor takes the page foreground, so it shows on the painted page
 * whatever colour the terminal gives it. It stays empty for the terminal's
 * own colours, and for 0-15, whose colours only the terminal knows. */
static void theme_build_cursor(ThemeColour fg) {
    char *out = g_theme.cursor;
    out[0] = '\0';
    u8 rgb[3];
    if (!theme_rgb(fg, rgb)) return;
    i32 n = snprintf(out, THEME_SGR_MAX, "\033]12;#%02x%02x%02x\a",
                     (unsigned)rgb[0], (unsigned)rgb[1], (unsigned)rgb[2]);
    if (n <= 0 || n >= THEME_SGR_MAX) out[0] = '\0';
}

static void theme_apply(const ThemeColour *c, Str name) {
    const char *ct = getenv("COLORTERM");
    g_theme.truecolor =
        ct && (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit"));
    ThemeColour fg = c[THEME_PAGE_FG], bg = c[THEME_PAGE_BG];
    for (size_t s = 0; s < THEME_SLOT_N; s++) {
        ThemeColour v = c[s];
        if (s != THEME_PAGE_FG && s != THEME_PAGE_BG
            && v.kind == THEME_COLOUR_DEFAULT)
            v = k_theme_slots[s].bg ? bg : fg;
        theme_build((ThemeSlot)s, v);
    }
    theme_build_page(fg, bg);
    theme_build_cursor(fg);
    size_t n = name.n < AGENT_MAX_THEME_NAME ? name.n : AGENT_MAX_THEME_NAME;
    memcpy(g_theme.name, name.p, n);
    g_theme.name[n] = '\0';
}

static void theme_builtin_colours(size_t b, ThemeColour *out) {
    for (size_t s = 0; s < THEME_SLOT_N; s++) out[s] = k_theme_builtin[s][b];
}

static void theme_ready(void) {
    if (g_theme.name[0]) return;
    ThemeColour c[THEME_SLOT_N];
    theme_builtin_colours(0, c);
    theme_apply(c, str_c(k_theme_names[0]));
}

const char *theme_sgr(ThemeSlot s) {
    theme_ready();
    return s < THEME_SLOT_N ? g_theme.sgr[s] : "";
}

const char *theme_page(void) {
    theme_ready();
    return g_theme.page;
}

const char *theme_cursor(void) {
    theme_ready();
    return g_theme.cursor;
}

Str theme_current(void) {
    if (!g_theme.name[0]) return str_c(k_theme_names[0]);
    return str_c(g_theme.name);
}

/* ---- names and files ----------------------------------------------------- */

static b8 theme_name_ok(Str name) {
    if (!name.n || name.n > AGENT_MAX_THEME_NAME) return false;
    for (size_t i = 0; i < name.n; i++) {
        char c = name.p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
              || c == '-'))
            return false;
    }
    return true;
}

static size_t theme_builtin_find(Str name) {
    for (size_t b = 0; b < THEME_BUILTIN_N; b++)
        if (str_eq(name, str_c(k_theme_names[b]))) return b;
    return THEME_BUILTIN_N;
}

static b8 theme_is_file(Str path) {
    char z[AGENT_MAX_PATH];
    if (!path.n || path.n >= sizeof z) return false;
    memcpy(z, path.p, path.n);
    z[path.n] = '\0';
    struct stat st;
    return stat(z, &st) == 0 && S_ISREG(st.st_mode);
}

static Str theme_file_path(Str name, Arena *scratch) {
    char rel[AGENT_MAX_THEME_NAME + 16];
    i32 n = snprintf(rel, sizeof rel, "themes/%.*s.toml", (i32)name.n, name.p);
    if (n < 0 || (size_t)n >= sizeof rel) return (Str){0};
    Str paths[AGENT_MAX_CONFIG_FILES];
    size_t pn = paths_config_files((Str){rel, (size_t)n}, scratch, paths,
                                   AGENT_MAX_CONFIG_FILES);
    for (size_t i = pn; i > 0; i--)
        if (theme_is_file(paths[i - 1])) return paths[i - 1];
    return (Str){0};
}

static size_t theme_slot_find(Str key) {
    for (size_t s = 0; s < THEME_SLOT_N; s++)
        if (str_eq(key, str_c(k_theme_slots[s].name))) return s;
    return THEME_SLOT_N;
}

static void theme_read_file(Str path, ThemeColour *c, Arena *scratch) {
    Settings s;
    if (!settings_load(&s, path, scratch)) s.n = 0;
    size_t base = 0;
    Str base_name = settings_get(&s, (Str){0}, STR("base"));
    if (base_name.n) {
        base = theme_builtin_find(base_name);
        if (base == THEME_BUILTIN_N) {
            agent_log(AGENT_LOG_WARN,
                      "ignoring base in %.*s: it must name a built-in theme",
                      (i32)path.n, path.p);
            base = 0;
        }
    }
    theme_builtin_colours(base, c);
    for (size_t i = 0; i < s.n; i++) {
        Str key = s.key[i];
        if (!s.section[i].n && str_eq(key, STR("base"))) continue;
        size_t slot = s.section[i].n ? THEME_SLOT_N : theme_slot_find(key);
        if (slot == THEME_SLOT_N) {
            agent_log(AGENT_LOG_WARN, "unknown theme key %.*s in %.*s",
                      (i32)key.n, key.p, (i32)path.n, path.p);
            continue;
        }
        ThemeColour v;
        if (!theme_colour_parse(s.val[i], &v)) {
            agent_log(AGENT_LOG_WARN,
                      "ignoring %.*s in %.*s: a colour is 0-255, "
                      "\"#rrggbb\" or \"default\"",
                      (i32)key.n, key.p, (i32)path.n, path.p);
            continue;
        }
        c[slot] = v;
    }
}

b8 theme_load(Str name, Arena *scratch) {
    if (!theme_name_ok(name)) return false;
    ThemeColour c[THEME_SLOT_N];
    size_t b = theme_builtin_find(name);
    if (b != THEME_BUILTIN_N) {
        theme_builtin_colours(b, c);
        theme_apply(c, name);
        return true;
    }
    size_t mark = scratch->off;
    Str path = theme_file_path(name, scratch);
    if (!path.n) {
        scratch->off = mark;
        return false;
    }
    theme_read_file(path, c, scratch);
    scratch->off = mark;
    theme_apply(c, name);
    return true;
}

static void theme_list_dir(Str dir, Str *names, Str *where, size_t first,
                           size_t *n, size_t max, Arena *a) {
    char z[AGENT_MAX_PATH];
    if (!dir.n || dir.n >= sizeof z) return;
    memcpy(z, dir.p, dir.n);
    z[dir.n] = '\0';
    DIR *d = opendir(z);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        Str file = str_c(e->d_name);
        if (file.n <= 5 || !str_eq(str_drop(file, file.n - 5), STR(".toml")))
            continue;
        Str name = str_take(file, file.n - 5);
        if (!theme_name_ok(name) || theme_builtin_find(name) != THEME_BUILTIN_N)
            continue;
        Buf b;
        buf_init(&b, a, dir.n + file.n + 2);
        buf_puts(&b, dir);
        buf_putc(&b, '/');
        buf_puts(&b, file);
        if (!buf_ok(&b)) continue;
        Str path = buf_finish(&b);
        if (!path.p || !theme_is_file(path)) continue;
        size_t j = first;
        while (j < *n && !str_eq(names[j], name)) j++;
        if (j < *n) {
            where[j] = path;
            continue;
        }
        if (*n >= max) continue;
        Str owned = str_dup(a, name);
        if (!owned.p) continue;
        names[*n] = owned;
        where[*n] = path;
        (*n)++;
    }
    closedir(d);
}

static b8 theme_name_before(Str a, Str b) {
    size_t n = a.n < b.n ? a.n : b.n;
    i32 c = memcmp(a.p, b.p, n);
    return c < 0 || (c == 0 && a.n < b.n);
}

size_t theme_list(Str *names, Str *where, size_t max, Arena *a) {
    size_t n = 0;
    for (size_t b = 0; b < THEME_BUILTIN_N && n < max; b++) {
        names[n] = str_c(k_theme_names[b]);
        where[n] = (Str){0};
        n++;
    }
    size_t first = n;
    Str dirs[AGENT_MAX_CONFIG_FILES];
    size_t dn =
        paths_config_files(STR("themes"), a, dirs, AGENT_MAX_CONFIG_FILES);
    for (size_t i = 0; i < dn; i++)
        theme_list_dir(dirs[i], names, where, first, &n, max, a);
    for (size_t i = first + 1; i < n; i++) {
        Str kn = names[i], kw = where[i];
        size_t j = i;
        while (j > first && theme_name_before(kn, names[j - 1])) {
            names[j] = names[j - 1];
            where[j] = where[j - 1];
            j--;
        }
        names[j] = kn;
        where[j] = kw;
    }
    return n;
}
