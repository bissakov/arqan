/* PDF text for the read tool.
 *
 * pdf_text turns the bytes of a PDF file into the text each page shows, in
 * the order the page draws it, so that `read` pages through a PDF like a text
 * file. Nothing is rendered: content streams run only for their text and
 * position operators, and the place of each glyph decides whether a space or
 * a line break goes before it.
 *
 * A PDF is untrusted input. Every offset, count and length it states is
 * checked against the bytes before use, nesting has a fixed depth, work per
 * page has a budget, and every buffer comes from the caller's arena. A broken
 * file gives a partial result or an error, never a read outside its bytes.
 *
 * Covered: cross-reference tables and streams, object streams, and a table
 * rebuilt by scanning when the stated one is broken; the Flate, LZW,
 * ASCIIHex, ASCII85 and RunLength filters with PNG and TIFF predictors; the
 * standard security handler with an empty user password, RC4 and AES-128;
 * Type0 and simple fonts through ToUnicode maps, encodings, Differences and
 * the encoding built into an embedded Type 1 font. Not covered: AES-256
 * encryption, text drawn as images (scans), and CJK fonts with no ToUnicode.
 */

#include "agent.h"
#include "../vendor/wuffs/bridge.h"

#include <stdlib.h>
#include <string.h>

#include "pdf_tables.c"

#define PDF_MAX_DEPTH         32u
#define PDF_MAX_OBJECTS       (1u << 23)
#define PDF_MAX_XREF_SECTIONS 64u
#define PDF_MAX_STREAM        (64u << 20)
#define PDF_MAX_PAGES         65536u
#define PDF_MAX_TREE_DEPTH    64u
#define PDF_MAX_FORM_DEPTH    8u
#define PDF_MAX_OPERANDS      512u
#define PDF_MAX_GSTATE        64u
#define PDF_MAX_FILTERS       8u
#define PDF_MAX_PAGE_OPS      (4u << 20)
#define PDF_MAX_CODESPACE     8u
#define PDF_MAX_GLYPH_TEXT    64u
#define PDF_FONT_SLOTS        1024u
#define PDF_FORM_SLOTS        64u
#define PDF_HEAD_WINDOW       1024u
#define PDF_TAIL_WINDOW       4096u
#define PDF_PAGE_ARENA_MAX    (64u << 20)
#define PDF_NOT_FOUND         ((size_t)-1)

/* ---- bytes ---------------------------------------------------------------- */

static size_t pdf_find(Str hay, size_t from, Str needle) {
    if (!needle.n || from > hay.n || hay.n - from < needle.n)
        return PDF_NOT_FOUND;
    size_t last = hay.n - needle.n;
    for (size_t i = from; i <= last;) {
        const char *hit = memchr(hay.p + i, needle.p[0], last - i + 1);
        if (!hit) break;
        i = (size_t)(hit - hay.p);
        if (!memcmp(hay.p + i, needle.p, needle.n)) return i;
        i++;
    }
    return PDF_NOT_FOUND;
}

static size_t pdf_find_last(Str hay, size_t from, Str needle) {
    if (!needle.n || hay.n < needle.n) return PDF_NOT_FOUND;
    for (size_t i = hay.n - needle.n + 1; i-- > from;)
        if (hay.p[i] == needle.p[0] && !memcmp(hay.p + i, needle.p, needle.n))
            return i;
    return PDF_NOT_FOUND;
}

static b8 pdf_space(u8 c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

static b8 pdf_delim(u8 c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']'
           || c == '{' || c == '}' || c == '/' || c == '%';
}

static b8 pdf_regular(u8 c) {
    return !pdf_space(c) && !pdf_delim(c);
}

static i32 pdf_hex_digit(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static f64 pdf_abs(f64 v) {
    return v < 0 ? -v : v;
}

static b8 pdf_u32(f64 v, u32 *out) {
    if (!(v >= 0 && v <= (f64)UINT32_MAX)) return false;
    *out = (u32)v;
    return true;
}

static size_t pdf_utf8_put(char *out, size_t cap, u32 cp) {
    char s[4];
    size_t n;
    if (cp < 0x80) {
        s[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        s[0] = (char)(0xC0 | (cp >> 6));
        s[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        s[0] = (char)(0xE0 | (cp >> 12));
        s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        s[0] = (char)(0xF0 | (cp >> 18));
        s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        s[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (n > cap) return 0;
    memcpy(out, s, n);
    return n;
}

/* ---- lexer ----------------------------------------------------------------
 * One token at a time over a byte range, for objects, content streams, CMaps
 * and the clear text of a Type 1 font alike. A string or name token keeps its
 * raw bytes; StrIter decodes them on demand, so a content stream shows text
 * without copying it. */

typedef enum {
    TK_EOF,
    TK_NUM,
    TK_NAME,
    TK_STR,
    TK_HEX,
    TK_WORD,
    TK_ARRAY_OPEN,
    TK_ARRAY_CLOSE,
    TK_DICT_OPEN,
    TK_DICT_CLOSE,
    TK_OTHER,
} TokKind;

typedef struct {
    TokKind kind;
    b8 is_int;
    f64 num;
    Str raw;
} Tok;

typedef struct {
    const char *p;
    size_t n, i;
} Lex;

static size_t number_prefix(Str s, f64 *out, b8 *is_int) {
    size_t i = 0;
    b8 neg = false, digits = false, dot = false;
    while (i < s.n && (s.p[i] == '+' || s.p[i] == '-')) {
        if (s.p[i] == '-') neg = true;
        i++;
    }
    f64 v = 0, scale = 1;
    for (; i < s.n; i++) {
        char c = s.p[i];
        if (c >= '0' && c <= '9') {
            digits = true;
            if (dot) {
                scale /= 10;
                v += (c - '0') * scale;
            } else {
                v = v * 10 + (c - '0');
            }
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            break;
        }
    }
    if (!digits) return 0;
    *out = neg ? -v : v;
    *is_int = !dot;
    return i;
}

static void lex_skip(Lex *lx) {
    while (lx->i < lx->n) {
        u8 c = (u8)lx->p[lx->i];
        if (pdf_space(c)) {
            lx->i++;
        } else if (c == '%') {
            while (lx->i < lx->n && lx->p[lx->i] != '\n'
                   && lx->p[lx->i] != '\r')
                lx->i++;
        } else {
            break;
        }
    }
}

static Tok lex_next(Lex *lx) {
    Tok t = {.kind = TK_EOF};
    lex_skip(lx);
    if (lx->i >= lx->n) return t;
    size_t s = lx->i;
    u8 c = (u8)lx->p[s];
    t.raw = (Str){lx->p + s, 1};
    switch (c) {
        case '[':
            lx->i++;
            t.kind = TK_ARRAY_OPEN;
            return t;
        case ']':
            lx->i++;
            t.kind = TK_ARRAY_CLOSE;
            return t;
        case '<': {
            if (s + 1 < lx->n && lx->p[s + 1] == '<') {
                lx->i += 2;
                t.kind = TK_DICT_OPEN;
                return t;
            }
            size_t e = s + 1;
            while (e < lx->n && lx->p[e] != '>') e++;
            t.kind = TK_HEX;
            t.raw = (Str){lx->p + s + 1, e - s - 1};
            lx->i = e < lx->n ? e + 1 : e;
            return t;
        }
        case '>':
            if (s + 1 < lx->n && lx->p[s + 1] == '>') {
                lx->i += 2;
                t.kind = TK_DICT_CLOSE;
                return t;
            }
            lx->i++;
            t.kind = TK_OTHER;
            return t;
        case '(': {
            size_t e = s + 1;
            u32 depth = 1;
            while (e < lx->n) {
                char ch = lx->p[e];
                if (ch == '\\') {
                    e += 2;
                    continue;
                }
                if (ch == '(')
                    depth++;
                else if (ch == ')' && --depth == 0)
                    break;
                e++;
            }
            if (e > lx->n) e = lx->n;
            t.kind = TK_STR;
            t.raw = (Str){lx->p + s + 1, e - s - 1};
            lx->i = e < lx->n ? e + 1 : e;
            return t;
        }
        case ')':
        case '{':
        case '}':
            lx->i++;
            t.kind = TK_OTHER;
            return t;
        case '/': {
            size_t e = s + 1;
            while (e < lx->n && pdf_regular((u8)lx->p[e])) e++;
            t.kind = TK_NAME;
            t.raw = (Str){lx->p + s + 1, e - s - 1};
            lx->i = e;
            return t;
        }
        default: break;
    }
    size_t e = s;
    while (e < lx->n && pdf_regular((u8)lx->p[e])) e++;
    if (e == s) e = s + 1;
    t.raw = (Str){lx->p + s, e - s};
    lx->i = e;
    t.kind = TK_WORD;
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        if (number_prefix(t.raw, &t.num, &t.is_int)) t.kind = TK_NUM;
    }
    return t;
}

static b8 tok_word(const Tok *t, Str w) {
    return t->kind == TK_WORD && str_eq(t->raw, w);
}

/* ---- strings -------------------------------------------------------------- */

typedef struct {
    const char *p;
    size_t n, i;
    b8 hex;
} StrIter;

static i32 str_iter_next(StrIter *it) {
    if (it->hex) {
        i32 hi = -1;
        while (it->i < it->n) {
            i32 v = pdf_hex_digit((u8)it->p[it->i++]);
            if (v < 0) continue;
            if (hi < 0)
                hi = v;
            else
                return hi * 16 + v;
        }
        return hi < 0 ? -1 : hi * 16;
    }
    while (it->i < it->n) {
        u8 c = (u8)it->p[it->i++];
        if (c == '\r') {
            if (it->i < it->n && it->p[it->i] == '\n') it->i++;
            return '\n';
        }
        if (c != '\\') return c;
        if (it->i >= it->n) return -1;
        c = (u8)it->p[it->i++];
        switch (c) {
            case 'n': return '\n';
            case 'r': return '\r';
            case 't': return '\t';
            case 'b': return '\b';
            case 'f': return '\f';
            case '\r':
                if (it->i < it->n && it->p[it->i] == '\n') it->i++;
                continue;
            case '\n': continue;
            default: break;
        }
        if (c >= '0' && c <= '7') {
            i32 v = c - '0';
            for (u32 k = 0; k < 2 && it->i < it->n && it->p[it->i] >= '0'
                            && it->p[it->i] <= '7';
                 k++)
                v = v * 8 + (it->p[it->i++] - '0');
            return v & 0xFF;
        }
        return c;
    }
    return -1;
}

static Str pdf_str_decode(Arena *a, Str raw, b8 hex) {
    char *p = arena_alloc(a, raw.n + 1, 1);
    if (!p) return (Str){0};
    StrIter it = {raw.p, raw.n, 0, hex};
    size_t n = 0;
    for (i32 c; (c = str_iter_next(&it)) >= 0;) p[n++] = (char)c;
    p[n] = '\0';
    return (Str){p, n};
}

static Str pdf_name_decode(Arena *a, Str raw) {
    if (!memchr(raw.p, '#', raw.n)) return raw;
    char *p = arena_alloc(a, raw.n + 1, 1);
    if (!p) return (Str){0};
    size_t n = 0;
    for (size_t i = 0; i < raw.n; i++) {
        i32 hi, lo;
        if (raw.p[i] == '#' && i + 2 < raw.n
            && (hi = pdf_hex_digit((u8)raw.p[i + 1])) >= 0
            && (lo = pdf_hex_digit((u8)raw.p[i + 2])) >= 0) {
            p[n++] = (char)(hi * 16 + lo);
            i += 2;
        } else {
            p[n++] = raw.p[i];
        }
    }
    p[n] = '\0';
    return (Str){p, n};
}

/* ---- objects --------------------------------------------------------------
 * A parsed value. A dict holds its pairs flat in `items`, key then value. A
 * stream is a dict whose undecoded bytes the object table keeps beside it. */

typedef enum {
    PO_NULL,
    PO_BOOL,
    PO_NUM,
    PO_STR,
    PO_NAME,
    PO_ARRAY,
    PO_DICT,
    PO_REF,
} PdfKind;

typedef struct PdfObj PdfObj;
struct PdfObj {
    u8 kind;
    b8 flag;
    u32 n;
    union {
        f64 num;
        Str s;
        PdfObj *items;
        struct {
            u32 num, gen;
        } ref;
    } u;
};

typedef struct {
    PdfObj *p;
    u32 n, cap;
    Arena *a;
} ObjVec;

static b8 objvec_push(ObjVec *v, PdfObj o) {
    if (v->n == v->cap) {
        if (v->cap > UINT32_MAX / 2) return false;
        u32 cap = v->cap ? v->cap * 2 : 8;
        PdfObj *p = arena_new(v->a, PdfObj, cap);
        if (!p) return false;
        if (v->n) memcpy(p, v->p, v->n * sizeof *p);
        v->p = p;
        v->cap = cap;
    }
    v->p[v->n++] = o;
    return true;
}

static b8 stop_word(const Tok *t) {
    if (t->kind != TK_WORD) return false;
    static const char *const words[] = {"endobj", "stream",  "endstream", "obj",
                                        "xref",   "trailer", "startxref"};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (str_eq(t->raw, str_c(words[i]))) return true;
    return false;
}

static b8 pdf_parse_value(Arena *a, Lex *lx, Tok t, u32 depth, PdfObj *out);

static b8 pdf_parse_array(Arena *a, Lex *lx, u32 depth, PdfObj *out) {
    if (depth >= PDF_MAX_DEPTH) return false;
    ObjVec v = {.a = a};
    for (;;) {
        size_t before = lx->i;
        Tok t = lex_next(lx);
        if (t.kind == TK_EOF || t.kind == TK_ARRAY_CLOSE
            || t.kind == TK_DICT_CLOSE)
            break;
        if (stop_word(&t)) {
            lx->i = before;
            break;
        }
        PdfObj item;
        if (!pdf_parse_value(a, lx, t, depth + 1, &item)
            || !objvec_push(&v, item))
            return false;
    }
    *out = (PdfObj){.kind = PO_ARRAY, .n = v.n, .u.items = v.p};
    return true;
}

static b8 pdf_parse_dict(Arena *a, Lex *lx, u32 depth, PdfObj *out) {
    if (depth >= PDF_MAX_DEPTH) return false;
    ObjVec v = {.a = a};
    for (;;) {
        size_t before = lx->i;
        Tok k = lex_next(lx);
        if (k.kind == TK_EOF || k.kind == TK_DICT_CLOSE) break;
        if (stop_word(&k)) {
            lx->i = before;
            break;
        }
        if (k.kind != TK_NAME) continue;
        before = lx->i;
        Tok vt = lex_next(lx);
        if (vt.kind == TK_EOF || vt.kind == TK_DICT_CLOSE) break;
        if (stop_word(&vt)) {
            lx->i = before;
            break;
        }
        PdfObj key = {.kind = PO_NAME, .u.s = pdf_name_decode(a, k.raw)};
        PdfObj val;
        if (!key.u.s.p || !pdf_parse_value(a, lx, vt, depth + 1, &val)
            || !objvec_push(&v, key) || !objvec_push(&v, val))
            return false;
    }
    *out = (PdfObj){.kind = PO_DICT, .n = v.n / 2, .u.items = v.p};
    return true;
}

static b8 pdf_parse_value(Arena *a, Lex *lx, Tok t, u32 depth, PdfObj *out) {
    *out = (PdfObj){.kind = PO_NULL};
    switch (t.kind) {
        case TK_NUM: {
            *out = (PdfObj){.kind = PO_NUM, .flag = t.is_int, .u.num = t.num};
            u32 num;
            if (!t.is_int || !pdf_u32(t.num, &num)) return true;
            size_t save = lx->i;
            Tok g = lex_next(lx);
            if (g.kind == TK_NUM && g.is_int && g.num >= 0 && g.num <= 65535) {
                Tok r = lex_next(lx);
                if (tok_word(&r, STR("R"))) {
                    out->kind = PO_REF;
                    out->u.ref.num = num;
                    out->u.ref.gen = (u32)g.num;
                    return true;
                }
            }
            lx->i = save;
            return true;
        }
        case TK_NAME:
            out->kind = PO_NAME;
            out->u.s = pdf_name_decode(a, t.raw);
            return out->u.s.p != NULL;
        case TK_STR:
        case TK_HEX:
            out->kind = PO_STR;
            out->u.s = pdf_str_decode(a, t.raw, t.kind == TK_HEX);
            return out->u.s.p != NULL;
        case TK_WORD:
            if (str_eq(t.raw, STR("true")) || str_eq(t.raw, STR("false"))) {
                out->kind = PO_BOOL;
                out->flag = str_eq(t.raw, STR("true"));
            }
            return true;
        case TK_ARRAY_OPEN: return pdf_parse_array(a, lx, depth, out);
        case TK_DICT_OPEN: return pdf_parse_dict(a, lx, depth, out);
        default: return true;
    }
}

static PdfObj *dict_get(PdfObj *dict, Str key) {
    if (!dict || dict->kind != PO_DICT) return NULL;
    for (u32 i = 0; i < dict->n; i++)
        if (str_eq(dict->u.items[2 * i].u.s, key))
            return &dict->u.items[2 * i + 1];
    return NULL;
}

static b8 obj_is_name(const PdfObj *o, Str name) {
    return o && o->kind == PO_NAME && str_eq(o->u.s, name);
}

static b8 obj_num(const PdfObj *o, f64 *out) {
    if (!o || o->kind != PO_NUM) return false;
    *out = o->u.num;
    return true;
}

/* ---- document -------------------------------------------------------------
 * The cross-reference table as parallel arrays, one entry per object number.
 * An entry is free, at a file offset, or at an index in an object stream. */

enum { XREF_FREE, XREF_FILE, XREF_STM };
enum { OBJ_NEW, OBJ_LOADING, OBJ_DONE, OBJ_FAILED };
enum { CRYPT_NONE, CRYPT_RC4, CRYPT_AES };

typedef struct {
    u32 n, cap;
    u8 *type;
    u8 *state;
    u16 *gen;
    u32 *idx;
    u64 *off;
    PdfObj **obj;
    Str *raw;
} Xref;

typedef struct {
    u32 count, first;
    u32 *nums;
    u32 *offs;
    Str data;
    b8 bad;
} ObjStm;

typedef struct {
    u8 method;
    u8 key[16];
    u32 key_n;
} Crypt;

typedef struct PdfFont PdfFont;

typedef struct {
    Str file;
    Arena *a;
    Arena *page;
    Xref x;
    ObjStm **stm;
    u32 stm_n;
    PdfObj *root, *encrypt, *id;
    b8 rebuilt, crypt_ready, loading_xref;
    Crypt crypt;
    AgentInflate *zs;
    const PdfObj *font_key[PDF_FONT_SLOTS];
    PdfFont *font_val[PDF_FONT_SLOTS];
} Doc;

static PdfObj *doc_obj(Doc *d, u32 num, Str *raw);
static b8 xref_rebuild(Doc *d);

static PdfObj *pdf_resolve(Doc *d, PdfObj *o) {
    for (u32 hops = 0; o && o->kind == PO_REF && hops < 8; hops++)
        o = doc_obj(d, o->u.ref.num, NULL);
    if (!o || o->kind == PO_REF || o->kind == PO_NULL) return NULL;
    return o;
}

static PdfObj *dget(Doc *d, PdfObj *dict, Str key) {
    return pdf_resolve(d, dict_get(dict, key));
}

static b8 dget_num(Doc *d, PdfObj *dict, Str key, f64 *out) {
    return obj_num(dget(d, dict, key), out);
}

static b8 xref_reserve(Doc *d, u64 need) {
    Xref *x = &d->x;
    if (need <= x->cap) {
        if (need > x->n) x->n = (u32)need;
        return true;
    }
    if (need > PDF_MAX_OBJECTS) return false;
    u64 cap = x->cap ? x->cap : 256;
    while (cap < need) cap *= 2;
    if (cap > PDF_MAX_OBJECTS) cap = PDF_MAX_OBJECTS;
    size_t c = (size_t)cap;
    Xref nx = {.n = (u32)need, .cap = (u32)c};
    nx.type = arena_new(d->a, u8, c);
    nx.state = arena_new(d->a, u8, c);
    nx.gen = arena_new(d->a, u16, c);
    nx.idx = arena_new(d->a, u32, c);
    nx.off = arena_new(d->a, u64, c);
    nx.obj = arena_new(d->a, PdfObj *, c);
    nx.raw = arena_new(d->a, Str, c);
    if (!nx.type || !nx.state || !nx.gen || !nx.idx || !nx.off || !nx.obj
        || !nx.raw)
        return false;
    memset(nx.type, 0, c);
    memset(nx.state, 0, c);
    memset(nx.gen, 0, c * sizeof *nx.gen);
    memset(nx.idx, 0, c * sizeof *nx.idx);
    memset(nx.off, 0, c * sizeof *nx.off);
    memset(nx.obj, 0, c * sizeof *nx.obj);
    memset(nx.raw, 0, c * sizeof *nx.raw);
    if (x->cap) {
        memcpy(nx.type, x->type, x->cap);
        memcpy(nx.state, x->state, x->cap);
        memcpy(nx.gen, x->gen, x->cap * sizeof *nx.gen);
        memcpy(nx.idx, x->idx, x->cap * sizeof *nx.idx);
        memcpy(nx.off, x->off, x->cap * sizeof *nx.off);
        memcpy(nx.obj, x->obj, x->cap * sizeof *nx.obj);
        memcpy(nx.raw, x->raw, x->cap * sizeof *nx.raw);
        if (x->n > nx.n) nx.n = x->n;
    }
    *x = nx;
    return true;
}

static void xref_set(Doc *d, u64 num, u8 type, u64 off, u32 idx, b8 replace) {
    if (num >= PDF_MAX_OBJECTS || !xref_reserve(d, num + 1)) return;
    if (d->x.type[num] != XREF_FREE && !replace) return;
    d->x.type[num] = type;
    d->x.off[num] = off;
    d->x.idx[num] = idx;
}

/* ---- MD5, RC4 and AES-128 --------------------------------------------------
 * Only what the standard security handler needs to read a document whose user
 * password is empty: MD5 for the keys, RC4 and AES-CBC to decrypt. */

typedef struct {
    u32 h[4];
    u64 len;
    u8 buf[64];
    size_t n;
} Md5;

static const u32 k_md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const u8 k_md5_shift[16] = {7, 12, 17, 22, 5, 9,  14, 20,
                                   4, 11, 16, 23, 6, 10, 15, 21};

static void md5_block(u32 h[4], const u8 *p) {
    u32 m[16];
    for (u32 i = 0; i < 16; i++)
        m[i] = (u32)p[4 * i] | (u32)p[4 * i + 1] << 8 | (u32)p[4 * i + 2] << 16
               | (u32)p[4 * i + 3] << 24;
    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    for (u32 i = 0; i < 64; i++) {
        u32 f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        u32 s = k_md5_shift[(i / 16) * 4 + (i & 3)];
        u32 x = a + f + k_md5_k[i] + m[g];
        a = d;
        d = c;
        c = b;
        b = b + ((x << s) | (x >> (32 - s)));
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

static void md5_init(Md5 *s) {
    *s = (Md5){.h = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}};
}

static void md5_update(Md5 *s, const void *data, size_t n) {
    const u8 *p = data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->n;
        if (take > n) take = n;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        n -= take;
        if (s->n == 64) {
            md5_block(s->h, s->buf);
            s->n = 0;
        }
    }
}

static void md5_final(Md5 *s, u8 out[16]) {
    u64 bits = s->len * 8;
    u8 pad[72] = {0x80};
    size_t pad_n = s->n < 56 ? 56 - s->n : 120 - s->n;
    md5_update(s, pad, pad_n);
    u8 len[8];
    for (u32 i = 0; i < 8; i++) len[i] = (u8)(bits >> (8 * i));
    md5_update(s, len, 8);
    for (u32 i = 0; i < 4; i++)
        for (u32 k = 0; k < 4; k++) out[4 * i + k] = (u8)(s->h[i] >> (8 * k));
}

static void rc4(const u8 *key, size_t key_n, u8 *data, size_t n) {
    u8 s[256];
    for (u32 i = 0; i < 256; i++) s[i] = (u8)i;
    u32 j = 0;
    for (u32 i = 0; i < 256; i++) {
        j = (j + s[i] + key[i % key_n]) & 255;
        u8 t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
    u32 i = 0;
    j = 0;
    for (size_t k = 0; k < n; k++) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        u8 t = s[i];
        s[i] = s[j];
        s[j] = t;
        data[k] ^= s[(s[i] + s[j]) & 255];
    }
}

typedef struct {
    u32 rounds;
    u8 rk[240];
} Aes;

static u8 aes_xtime(u8 x) {
    return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

static u8 aes_mul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = aes_xtime(a);
        b >>= 1;
    }
    return r;
}

static void aes_init(Aes *s, const u8 *key, u32 key_n) {
    u32 nk = key_n / 4;
    s->rounds = nk + 6;
    u32 total = 4 * (s->rounds + 1);
    memcpy(s->rk, key, key_n);
    u8 rcon = 1;
    for (u32 i = nk; i < total; i++) {
        u8 t[4];
        memcpy(t, s->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            u8 first = t[0];
            t[0] = (u8)(k_aes_sbox[t[1]] ^ rcon);
            t[1] = k_aes_sbox[t[2]];
            t[2] = k_aes_sbox[t[3]];
            t[3] = k_aes_sbox[first];
            rcon = aes_xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (u32 k = 0; k < 4; k++) t[k] = k_aes_sbox[t[k]];
        }
        for (u32 k = 0; k < 4; k++)
            s->rk[4 * i + k] = (u8)(s->rk[4 * (i - nk) + k] ^ t[k]);
    }
}

static void aes_decrypt_block(const Aes *s, u8 b[16]) {
    for (u32 k = 0; k < 16; k++) b[k] ^= s->rk[16 * s->rounds + k];
    for (u32 round = s->rounds; round-- > 0;) {
        u8 t[16];
        for (u32 c = 0; c < 4; c++)
            for (u32 r = 0; r < 4; r++)
                t[r + 4 * ((c + r) & 3)] = k_aes_inv_sbox[b[r + 4 * c]];
        for (u32 k = 0; k < 16; k++) b[k] = (u8)(t[k] ^ s->rk[16 * round + k]);
        if (!round) break;
        for (u32 c = 0; c < 4; c++) {
            u8 *col = b + 4 * c;
            u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = (u8)(aes_mul(a0, 14) ^ aes_mul(a1, 11) ^ aes_mul(a2, 13)
                          ^ aes_mul(a3, 9));
            col[1] = (u8)(aes_mul(a0, 9) ^ aes_mul(a1, 14) ^ aes_mul(a2, 11)
                          ^ aes_mul(a3, 13));
            col[2] = (u8)(aes_mul(a0, 13) ^ aes_mul(a1, 9) ^ aes_mul(a2, 14)
                          ^ aes_mul(a3, 11));
            col[3] = (u8)(aes_mul(a0, 11) ^ aes_mul(a1, 13) ^ aes_mul(a2, 9)
                          ^ aes_mul(a3, 14));
        }
    }
}

/* The first 16 bytes are the IV. A short or ragged tail is dropped, and the
 * padding is removed only when it is well formed. */
static b8 aes_cbc_decrypt(const u8 *key, u32 key_n, Str in, Arena *a,
                          Str *out) {
    size_t blocks = in.n >= 16 ? (in.n - 16) / 16 : 0;
    u8 *p = arena_alloc(a, blocks * 16 + 1, 1);
    if (!p) return false;
    Aes s;
    aes_init(&s, key, key_n);
    u8 prev[16] = {0};
    if (in.n >= 16) memcpy(prev, in.p, 16);
    for (size_t i = 0; i < blocks; i++) {
        u8 block[16], cipher[16];
        memcpy(cipher, in.p + 16 + 16 * i, 16);
        memcpy(block, cipher, 16);
        aes_decrypt_block(&s, block);
        for (u32 k = 0; k < 16; k++) p[16 * i + k] = (u8)(block[k] ^ prev[k]);
        memcpy(prev, cipher, 16);
    }
    size_t n = blocks * 16;
    if (n) {
        u8 pad = p[n - 1];
        b8 ok = pad >= 1 && pad <= 16;
        for (u32 k = 0; ok && k < pad; k++) ok = p[n - 1 - k] == pad;
        if (ok) n -= pad;
    }
    *out = (Str){(const char *)p, n};
    return true;
}

/* ---- filters ---------------------------------------------------------------
 * Each takes the whole input and leaves its output in the arena, refusing to
 * grow past PDF_MAX_STREAM. A stream that breaks off keeps what decoded. */

static b8 arena_grow_top(Arena *a, u8 *p, size_t cap, size_t want) {
    if ((u8 *)p + cap != a->base + a->off || want - cap > a->cap - a->off)
        return false;
    a->off += want - cap;
    return true;
}

static b8 inflate_all(Doc *d, Str in, Arena *a, Str *out) {
    if (!d->zs) {
        d->zs = arena_alloc(d->a, agent_inflate_size(), 16);
        if (!d->zs) return false;
    }
    if (agent_inflate_begin(d->zs)) return false;
    size_t cap = in.n < PDF_MAX_STREAM / 4 ? in.n * 4 + 1024 : PDF_MAX_STREAM;
    if (cap > PDF_MAX_STREAM) cap = PDF_MAX_STREAM;
    u8 *p = arena_alloc(a, cap, 1);
    if (!p) return false;
    size_t len = 0, used = 0;
    for (;;) {
        size_t took = 0, wrote = 0;
        i32 st = agent_inflate(d->zs, in.p + used, in.n - used, &took, p + len,
                               cap - len, &wrote);
        used += took;
        len += wrote;
        if (st == AGENT_INFLATE_DONE) break;
        if (st == AGENT_INFLATE_BAD) {
            if (!len) return false;
            break;
        }
        if (cap >= PDF_MAX_STREAM) break;
        size_t want = cap * 2 < PDF_MAX_STREAM ? cap * 2 : PDF_MAX_STREAM;
        if (!arena_grow_top(a, p, cap, want)) {
            u8 *np = arena_alloc(a, want, 1);
            if (!np) break;
            memcpy(np, p, len);
            p = np;
        }
        cap = want;
    }
    *out = (Str){(const char *)p, len};
    return true;
}

static u8 pdf_paeth(u8 a, u8 b, u8 c) {
    i32 p = (i32)a + b - c;
    i32 pa = p > a ? p - a : a - p;
    i32 pb = p > b ? p - b : b - p;
    i32 pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static b8 predict(Doc *d, PdfObj *parms, Str in, Arena *a, Str *out) {
    f64 pred = 1, colors = 1, bpc = 8, cols = 1;
    dget_num(d, parms, STR("Predictor"), &pred);
    dget_num(d, parms, STR("Colors"), &colors);
    dget_num(d, parms, STR("BitsPerComponent"), &bpc);
    dget_num(d, parms, STR("Columns"), &cols);
    *out = in;
    if (pred < 2) return true;
    if (!(colors >= 1 && colors <= 32 && cols >= 1 && cols <= (1u << 20))
        || !(bpc == 1 || bpc == 2 || bpc == 4 || bpc == 8 || bpc == 16))
        return false;
    size_t bits = (size_t)colors * (size_t)bpc;
    size_t bpp = (bits + 7) / 8;
    size_t row = (bits * (size_t)cols + 7) / 8;
    if (pred == 2) {
        if (bpc != 8) return true;
        u8 *p = arena_alloc(a, in.n + 1, 1);
        if (!p) return false;
        memcpy(p, in.p, in.n);
        for (size_t r = 0; r + row <= in.n; r += row)
            for (size_t i = bpp; i < row; i++)
                p[r + i] = (u8)(p[r + i] + p[r + i - bpp]);
        *out = (Str){(const char *)p, in.n};
        return true;
    }
    size_t rows = in.n / (row + 1);
    u8 *p = arena_alloc_array(a, rows ? rows : 1, row, 1);
    if (!p) return false;
    const u8 *src = (const u8 *)in.p;
    for (size_t r = 0; r < rows; r++) {
        u8 type = src[r * (row + 1)];
        const u8 *s = src + r * (row + 1) + 1;
        u8 *o = p + r * row;
        const u8 *up = r ? o - row : NULL;
        for (size_t i = 0; i < row; i++) {
            u8 left = i >= bpp ? o[i - bpp] : 0;
            u8 above = up ? up[i] : 0;
            u8 corner = up && i >= bpp ? up[i - bpp] : 0;
            u8 v = s[i];
            switch (type) {
                case 1: v = (u8)(v + left); break;
                case 2: v = (u8)(v + above); break;
                case 3: v = (u8)(v + ((u32)left + above) / 2); break;
                case 4: v = (u8)(v + pdf_paeth(left, above, corner)); break;
                default: break;
            }
            o[i] = v;
        }
    }
    *out = (Str){(const char *)p, rows * row};
    return true;
}

static b8 lzw_decode(Str in, b8 early, Arena *a, Str *out) {
    static const u32 k_max = 4096;
    u16 prefix[4096];
    u8 suffix[4096];
    u16 length[4096];
    u8 stack[4097];
    for (u32 i = 0; i < 256; i++) {
        prefix[i] = 0;
        suffix[i] = (u8)i;
        length[i] = 1;
    }
    Buf b;
    buf_init(&b, a, in.n * 2 + 64);
    u32 next = 258, width = 9, acc = 0, have = 0;
    i32 prev = -1;
    size_t pos = 0;
    for (;;) {
        while (have < width && pos < in.n) {
            acc = (acc << 8) | (u8)in.p[pos++];
            have += 8;
        }
        if (have < width) break;
        u32 code = (acc >> (have - width)) & ((1u << width) - 1);
        have -= width;
        if (code == 256) {
            next = 258;
            width = 9;
            prev = -1;
            continue;
        }
        if (code == 257) break;
        if (prev < 0) {
            if (code > 255) break;
            buf_putc(&b, (char)code);
            prev = (i32)code;
            continue;
        }
        u32 walk = code < next ? code : (u32)prev;
        if (code > next) break;
        u32 n = length[walk];
        for (u32 i = n, c = walk; i-- > 0;) {
            stack[i] = suffix[c];
            c = prefix[c];
        }
        if (code == next) stack[n++] = stack[0];
        buf_put(&b, stack, n);
        if (b.n > PDF_MAX_STREAM) break;
        if (next < k_max) {
            prefix[next] = (u16)prev;
            suffix[next] = stack[0];
            length[next] = (u16)(length[prev] + 1);
            next++;
        }
        if (next + (early ? 1u : 0u) >= (1u << width) && width < 12) width++;
        prev = (i32)code;
    }
    if (!buf_ok(&b)) return false;
    *out = (Str){b.p, b.n};
    return true;
}

static b8 hex_decode(Str in, Arena *a, Str *out) {
    char *p = arena_alloc(a, in.n / 2 + 2, 1);
    if (!p) return false;
    size_t n = 0;
    i32 hi = -1;
    for (size_t i = 0; i < in.n && in.p[i] != '>'; i++) {
        i32 v = pdf_hex_digit((u8)in.p[i]);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            p[n++] = (char)(hi * 16 + v);
            hi = -1;
        }
    }
    if (hi >= 0) p[n++] = (char)(hi * 16);
    *out = (Str){p, n};
    return true;
}

static b8 a85_decode(Str in, Arena *a, Str *out) {
    char *p = arena_alloc_array(a, in.n + 2, 4, 1);
    if (!p) return false;
    size_t n = 0;
    u64 tuple = 0;
    u32 count = 0;
    size_t i = 0;
    if (in.n >= 2 && in.p[0] == '<' && in.p[1] == '~') i = 2;
    for (; i < in.n; i++) {
        u8 c = (u8)in.p[i];
        if (c == '~') break;
        if (c == 'z' && count == 0) {
            memset(p + n, 0, 4);
            n += 4;
            continue;
        }
        if (c < '!' || c > 'u') continue;
        tuple = tuple * 85 + (u64)(c - '!');
        if (++count == 5) {
            for (u32 k = 0; k < 4; k++) p[n++] = (char)(tuple >> (24 - 8 * k));
            tuple = 0;
            count = 0;
        }
    }
    if (count > 1) {
        for (u32 k = count; k < 5; k++) tuple = tuple * 85 + 84;
        for (u32 k = 0; k < count - 1; k++)
            p[n++] = (char)(tuple >> (24 - 8 * k));
    }
    *out = (Str){p, n};
    return true;
}

static b8 runlength_decode(Str in, Arena *a, Str *out) {
    Buf b;
    buf_init(&b, a, in.n * 2 + 64);
    for (size_t i = 0; i < in.n && b.n <= PDF_MAX_STREAM;) {
        u8 len = (u8)in.p[i++];
        if (len == 128) break;
        if (len < 128) {
            size_t take = (size_t)len + 1;
            if (take > in.n - i) take = in.n - i;
            buf_put(&b, in.p + i, take);
            i += take;
        } else if (i < in.n) {
            char c = in.p[i++];
            for (u32 k = 0; k < 257u - len; k++) buf_putc(&b, c);
        }
    }
    if (!buf_ok(&b)) return false;
    *out = (Str){b.p, b.n};
    return true;
}

static u32 object_key(const Doc *d, u32 num, u32 gen, u8 key[16]) {
    Md5 m;
    md5_init(&m);
    md5_update(&m, d->crypt.key, d->crypt.key_n);
    u8 tail[9] = {(u8)num,
                  (u8)(num >> 8),
                  (u8)(num >> 16),
                  (u8)gen,
                  (u8)(gen >> 8),
                  's',
                  'A',
                  'l',
                  'T'};
    md5_update(&m, tail, d->crypt.method == CRYPT_AES ? 9 : 5);
    md5_final(&m, key);
    u32 n = d->crypt.key_n + 5;
    return n > 16 ? 16 : n;
}

static b8 decrypt_stream(Doc *d, u32 num, Str in, Arena *a, Str *out) {
    u8 key[16];
    u32 key_n = object_key(d, num, num < d->x.n ? d->x.gen[num] : 0, key);
    if (d->crypt.method == CRYPT_AES)
        return aes_cbc_decrypt(key, 16, in, a, out);
    u8 *p = arena_alloc(a, in.n + 1, 1);
    if (!p) return false;
    memcpy(p, in.p, in.n);
    rc4(key, key_n, p, in.n);
    *out = (Str){(const char *)p, in.n};
    return true;
}

static b8 apply_filter(Doc *d, PdfObj *name, PdfObj *parms, Str in, Arena *a,
                       Str *out) {
    if (!name || name->kind != PO_NAME) return false;
    Str f = name->u.s;
    if (str_eq(f, STR("FlateDecode")) || str_eq(f, STR("Fl"))) {
        Str raw;
        return inflate_all(d, in, a, &raw) && predict(d, parms, raw, a, out);
    }
    if (str_eq(f, STR("LZWDecode")) || str_eq(f, STR("LZW"))) {
        f64 early = 1;
        dget_num(d, parms, STR("EarlyChange"), &early);
        Str raw;
        return lzw_decode(in, early != 0, a, &raw)
               && predict(d, parms, raw, a, out);
    }
    if (str_eq(f, STR("ASCIIHexDecode")) || str_eq(f, STR("AHx")))
        return hex_decode(in, a, out);
    if (str_eq(f, STR("ASCII85Decode")) || str_eq(f, STR("A85")))
        return a85_decode(in, a, out);
    if (str_eq(f, STR("RunLengthDecode")) || str_eq(f, STR("RL")))
        return runlength_decode(in, a, out);
    if (str_eq(f, STR("Crypt"))) {
        *out = in;
        return true;
    }
    return false;
}

static b8 stream_decode(Doc *d, PdfObj *dict, Str raw, u32 num, b8 decrypt,
                        Arena *a, Str *out) {
    Str data = raw;
    if (decrypt && d->crypt.method != CRYPT_NONE
        && !decrypt_stream(d, num, raw, a, &data))
        return false;
    PdfObj *f = dget(d, dict, STR("Filter"));
    PdfObj *p = dget(d, dict, STR("DecodeParms"));
    if (!f) {
        *out = data;
        return true;
    }
    if (f->kind == PO_NAME)
        return apply_filter(d, f, p && p->kind == PO_DICT ? p : NULL, data, a,
                            out);
    if (f->kind != PO_ARRAY || f->n > PDF_MAX_FILTERS) return false;
    for (u32 i = 0; i < f->n; i++) {
        PdfObj *parm = NULL;
        if (p && p->kind == PO_ARRAY && i < p->n)
            parm = pdf_resolve(d, &p->u.items[i]);
        else if (p && p->kind == PO_DICT && f->n == 1)
            parm = p;
        if (parm && parm->kind != PO_DICT) parm = NULL;
        if (!apply_filter(d, pdf_resolve(d, &f->u.items[i]), parm, data, a,
                          &data))
            return false;
    }
    *out = data;
    return true;
}

/* ---- objects by number -----------------------------------------------------
 * An object loads on first use and stays parsed. A stated offset that does
 * not hold the object it should is the sign of a broken table: the table is
 * then rebuilt by scanning the file, once, and the lookup tried again. */

static Str stream_bounds(Doc *d, PdfObj *dict, size_t start) {
    Str f = d->file;
    if (start > f.n) start = f.n;
    f64 len;
    if (dget_num(d, dict, STR("Length"), &len) && len >= 0
        && len <= (f64)(f.n - start)) {
        Lex lx = {f.p, f.n, start + (size_t)len};
        lex_skip(&lx);
        if (f.n - lx.i >= 9 && !memcmp(f.p + lx.i, "endstream", 9))
            return (Str){f.p + start, (size_t)len};
    }
    size_t end = pdf_find(f, start, STR("endstream"));
    if (end == PDF_NOT_FOUND) end = f.n;
    if (end > start && f.p[end - 1] == '\n') end--;
    if (end > start && f.p[end - 1] == '\r') end--;
    return (Str){f.p + start, end - start};
}

static b8 parse_indirect(Doc *d, u64 off, u32 *num, u32 *gen, PdfObj *obj,
                         Str *raw) {
    *raw = (Str){0};
    if (off >= d->file.n) return false;
    Lex lx = {d->file.p, d->file.n, (size_t)off};
    Tok tn = lex_next(&lx), tg = lex_next(&lx), to = lex_next(&lx);
    if (tn.kind != TK_NUM || !tn.is_int || !pdf_u32(tn.num, num)
        || tg.kind != TK_NUM || !tg.is_int || !(tg.num >= 0 && tg.num <= 65535)
        || !tok_word(&to, STR("obj")))
        return false;
    *gen = (u32)tg.num;
    if (!pdf_parse_value(d->a, &lx, lex_next(&lx), 0, obj)) return false;
    if (obj->kind != PO_DICT) return true;
    Tok s = lex_next(&lx);
    if (!tok_word(&s, STR("stream"))) return true;
    size_t start = lx.i;
    if (start < d->file.n && d->file.p[start] == '\r') start++;
    if (start < d->file.n && d->file.p[start] == '\n') start++;
    *raw = stream_bounds(d, obj, start);
    return true;
}

static b8 objstm_get(Doc *d, u32 snum, u32 idx, u32 num, PdfObj *out);

static PdfObj *doc_obj(Doc *d, u32 num, Str *raw) {
    if (raw) *raw = (Str){0};
    for (u32 attempt = 0; attempt < 2; attempt++) {
        if (num >= d->x.n) return NULL;
        u8 state = d->x.state[num];
        if (state == OBJ_DONE) {
            if (raw) *raw = d->x.raw[num];
            return d->x.obj[num];
        }
        if (state != OBJ_NEW) return NULL;
        u8 type = d->x.type[num];
        if (type == XREF_FREE) {
            d->x.state[num] = OBJ_FAILED;
            return NULL;
        }
        d->x.state[num] = OBJ_LOADING;
        u64 off = d->x.off[num];
        u32 idx = d->x.idx[num];
        PdfObj *o = arena_new(d->a, PdfObj, 1);
        Str r = {0};
        u32 got = 0, gen = 0;
        b8 ok = false;
        if (o && type == XREF_FILE)
            ok = parse_indirect(d, off, &got, &gen, o, &r) && got == num;
        else if (o)
            ok = off < UINT32_MAX && objstm_get(d, (u32)off, idx, num, o);
        if (num >= d->x.n) return NULL;
        if (ok) {
            d->x.state[num] = OBJ_DONE;
            d->x.obj[num] = o;
            d->x.raw[num] = r;
            d->x.gen[num] = (u16)gen;
            if (raw) *raw = r;
            return o;
        }
        d->x.state[num] = OBJ_FAILED;
        if (!o || d->rebuilt || d->loading_xref || !xref_rebuild(d))
            return NULL;
    }
    return NULL;
}

static ObjStm *objstm_load(Doc *d, u32 snum) {
    if (snum >= d->x.n) return NULL;
    if (d->stm_n < d->x.n) {
        ObjStm **p = arena_new(d->a, ObjStm *, d->x.n);
        if (!p) return NULL;
        memset(p, 0, d->x.n * sizeof *p);
        if (d->stm_n) memcpy(p, d->stm, d->stm_n * sizeof *p);
        d->stm = p;
        d->stm_n = d->x.n;
    }
    if (d->stm[snum]) return d->stm[snum]->bad ? NULL : d->stm[snum];
    ObjStm *s = arena_new(d->a, ObjStm, 1);
    if (!s) return NULL;
    *s = (ObjStm){.bad = true};
    d->stm[snum] = s;
    Str raw;
    PdfObj *o = doc_obj(d, snum, &raw);
    f64 count, first;
    Str data;
    if (!o || !raw.p || !dget_num(d, o, STR("N"), &count)
        || !dget_num(d, o, STR("First"), &first)
        || !stream_decode(d, o, raw, snum, true, d->a, &data))
        return NULL;
    if (!(first >= 0 && first <= (f64)data.n && count >= 0
          && count <= first / 2 + 1))
        return NULL;
    u32 n = (u32)count;
    s->nums = arena_new(d->a, u32, n ? n : 1);
    s->offs = arena_new(d->a, u32, n ? n : 1);
    if (!s->nums || !s->offs) return NULL;
    Lex lx = {data.p, (size_t)first, 0};
    u32 k = 0;
    for (; k < n; k++) {
        Tok a = lex_next(&lx), b = lex_next(&lx);
        if (a.kind != TK_NUM || b.kind != TK_NUM || !pdf_u32(a.num, &s->nums[k])
            || !pdf_u32(b.num, &s->offs[k]))
            break;
    }
    s->count = k;
    s->first = (u32)first;
    s->data = data;
    s->bad = false;
    return s;
}

static b8 objstm_get(Doc *d, u32 snum, u32 idx, u32 num, PdfObj *out) {
    ObjStm *s = objstm_load(d, snum);
    if (!s) return false;
    u32 k = idx;
    if (k >= s->count || s->nums[k] != num) {
        for (k = 0; k < s->count && s->nums[k] != num; k++) {}
        if (k == s->count) return false;
    }
    u64 start = (u64)s->first + s->offs[k];
    if (start >= s->data.n) return false;
    Lex lx = {s->data.p, s->data.n, (size_t)start};
    return pdf_parse_value(d->a, &lx, lex_next(&lx), 0, out);
}

static void objstm_register(Doc *d) {
    for (u32 num = 1; num < d->x.n; num++) {
        if (d->x.type[num] != XREF_FILE) continue;
        PdfObj *o = doc_obj(d, num, NULL);
        if (!o || !obj_is_name(dget(d, o, STR("Type")), STR("ObjStm")))
            continue;
        ObjStm *s = objstm_load(d, num);
        for (u32 k = 0; s && k < s->count; k++)
            xref_set(d, s->nums[k], XREF_STM, num, k, false);
    }
}

/* ---- cross-reference table ------------------------------------------------ */

static void trailer_take(Doc *d, PdfObj *t) {
    if (!d->root) d->root = dict_get(t, STR("Root"));
    if (!d->encrypt) d->encrypt = dict_get(t, STR("Encrypt"));
    if (!d->id) d->id = dict_get(t, STR("ID"));
}

static b8 xref_stream_at(Doc *d, u64 off, PdfObj *trailer) {
    u32 num, gen;
    Str raw, data;
    if (!parse_indirect(d, off, &num, &gen, trailer, &raw)
        || trailer->kind != PO_DICT || !raw.p)
        return false;
    PdfObj *w = dget(d, trailer, STR("W"));
    if (!w || w->kind != PO_ARRAY || w->n < 3) return false;
    u32 width[3];
    for (u32 i = 0; i < 3; i++) {
        f64 v;
        if (!obj_num(&w->u.items[i], &v) || v < 0 || v > 8) return false;
        width[i] = (u32)v;
    }
    size_t entry = (size_t)width[0] + width[1] + width[2];
    if (!entry || !stream_decode(d, trailer, raw, num, false, d->a, &data))
        return false;
    f64 size = 0;
    dget_num(d, trailer, STR("Size"), &size);
    PdfObj all[2] = {{.kind = PO_NUM, .u.num = 0},
                     {.kind = PO_NUM, .u.num = size}};
    PdfObj *index = dget(d, trailer, STR("Index"));
    PdfObj *ranges = all;
    u32 pairs = 1;
    if (index && index->kind == PO_ARRAY) {
        ranges = index->u.items;
        pairs = index->n / 2;
    }
    size_t pos = 0;
    for (u32 r = 0; r < pairs; r++) {
        f64 start, count;
        if (!obj_num(&ranges[2 * r], &start)
            || !obj_num(&ranges[2 * r + 1], &count) || start < 0 || count < 0
            || start + count > PDF_MAX_OBJECTS)
            break;
        for (u64 k = 0; k < (u64)count && data.n - pos >= entry; k++) {
            u64 field[3] = {0, 0, 0};
            for (u32 i = 0; i < 3; i++)
                for (u32 b = 0; b < width[i]; b++)
                    field[i] = field[i] << 8 | (u8)data.p[pos++];
            u64 type = width[0] ? field[0] : 1;
            u64 objnum = (u64)start + k;
            if (type == 1)
                xref_set(d, objnum, XREF_FILE, field[1], 0, false);
            else if (type == 2 && field[2] <= UINT32_MAX)
                xref_set(d, objnum, XREF_STM, field[1], (u32)field[2], false);
        }
    }
    return true;
}

static b8 xref_table_at(Doc *d, Lex *lx, PdfObj *trailer) {
    for (;;) {
        Tok a = lex_next(lx);
        if (tok_word(&a, STR("trailer"))) {
            Tok t = lex_next(lx);
            return t.kind == TK_DICT_OPEN
                   && pdf_parse_value(d->a, lx, t, 0, trailer);
        }
        Tok b = lex_next(lx);
        if (a.kind != TK_NUM || b.kind != TK_NUM || a.num < 0 || b.num < 0
            || a.num + b.num > PDF_MAX_OBJECTS)
            return false;
        u64 start = (u64)a.num, count = (u64)b.num;
        for (u64 k = 0; k < count; k++) {
            Tok o = lex_next(lx), g = lex_next(lx), f = lex_next(lx);
            if (o.kind != TK_NUM || g.kind != TK_NUM || f.kind != TK_WORD)
                return false;
            if (str_eq(f.raw, STR("n")) && o.num > 0 && o.num < (f64)d->file.n)
                xref_set(d, start + k, XREF_FILE, (u64)o.num, 0, false);
        }
    }
}

static b8 xref_load(Doc *d) {
    Str f = d->file;
    size_t from = f.n > PDF_TAIL_WINDOW ? f.n - PDF_TAIL_WINDOW : 0;
    size_t at = pdf_find_last(f, from, STR("startxref"));
    if (at == PDF_NOT_FOUND) return false;
    Lex lx = {f.p, f.n, at + 9};
    Tok t = lex_next(&lx);
    if (t.kind != TK_NUM || !(t.num >= 0 && t.num < (f64)f.n)) return false;
    u64 off = (u64)t.num, seen[PDF_MAX_XREF_SECTIONS];
    u32 sections = 0;
    b8 any = false;
    d->loading_xref = true;
    while (sections < PDF_MAX_XREF_SECTIONS) {
        b8 loop = false;
        for (u32 i = 0; i < sections; i++) loop = loop || seen[i] == off;
        if (loop) break;
        seen[sections++] = off;
        PdfObj trailer = {0};
        Lex x = {f.p, f.n, (size_t)off};
        Tok k = lex_next(&x);
        b8 ok;
        if (tok_word(&k, STR("xref"))) {
            ok = xref_table_at(d, &x, &trailer);
            f64 stm;
            PdfObj hybrid;
            if (ok && obj_num(dict_get(&trailer, STR("XRefStm")), &stm)
                && stm >= 0 && stm < (f64)f.n)
                xref_stream_at(d, (u64)stm, &hybrid);
        } else {
            ok = xref_stream_at(d, off, &trailer);
        }
        if (!ok) break;
        any = true;
        trailer_take(d, &trailer);
        f64 prev;
        if (!obj_num(dict_get(&trailer, STR("Prev")), &prev)
            || !(prev >= 0 && prev < (f64)f.n))
            break;
        off = (u64)prev;
    }
    d->loading_xref = false;
    return any && d->root;
}

static b8 xref_rebuild(Doc *d) {
    d->rebuilt = true;
    d->x = (Xref){0};
    d->stm = NULL;
    d->stm_n = 0;
    Str f = d->file;
    for (size_t i = pdf_find(f, 0, STR("obj")); i != PDF_NOT_FOUND;
         i = pdf_find(f, i + 3, STR("obj"))) {
        if (i + 3 < f.n && pdf_regular((u8)f.p[i + 3])) continue;
        size_t j = i;
        while (j > 0 && pdf_space((u8)f.p[j - 1])) j--;
        size_t gen_end = j;
        while (j > 0 && f.p[j - 1] >= '0' && f.p[j - 1] <= '9') j--;
        if (j == gen_end || gen_end - j > 5) continue;
        size_t gen_start = j;
        while (j > 0 && pdf_space((u8)f.p[j - 1])) j--;
        size_t num_end = j;
        while (j > 0 && f.p[j - 1] >= '0' && f.p[j - 1] <= '9') j--;
        if (num_end == gen_start || j == num_end || num_end - j > 8) continue;
        if (j > 0 && pdf_regular((u8)f.p[j - 1])) continue;
        u64 num = 0;
        for (size_t k = j; k < num_end; k++)
            num = num * 10 + (u64)(f.p[k] - '0');
        if (num) xref_set(d, num, XREF_FILE, j, 0, true);
    }
    for (size_t end = f.n; !d->root;) {
        size_t at = pdf_find_last((Str){f.p, end}, 0, STR("trailer"));
        if (at == PDF_NOT_FOUND) break;
        Lex lx = {f.p, f.n, at + 7};
        Tok t = lex_next(&lx);
        PdfObj tr;
        if (t.kind == TK_DICT_OPEN && pdf_parse_value(d->a, &lx, t, 0, &tr))
            trailer_take(d, &tr);
        end = at;
    }
    for (u32 num = 1; !d->root && num < d->x.n; num++) {
        if (d->x.type[num] != XREF_FILE) continue;
        PdfObj *o = doc_obj(d, num, NULL);
        if (o && obj_is_name(dget(d, o, STR("Type")), STR("XRef")))
            trailer_take(d, o);
    }
    if (d->crypt_ready) objstm_register(d);
    return d->x.n > 0;
}

static void find_catalog(Doc *d) {
    for (u32 num = 1; num < d->x.n; num++) {
        PdfObj *o = doc_obj(d, num, NULL);
        if (o && obj_is_name(dget(d, o, STR("Type")), STR("Catalog"))) {
            d->root = o;
            return;
        }
    }
}

/* ---- standard security handler ---------------------------------------------
 * Revisions 2 to 4 with an empty user password: the key comes from the owner
 * hash, the permissions and the file ID, and the user hash proves it right.
 * Strings are left encrypted, since no text comes from a string outside a
 * content stream, and content streams are decrypted whole. */

static const u8 k_pdf_pad[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
    0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
    0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A,
};

static b8 crypt_user_ok(const u8 *key, u32 key_n, u32 rev, Str u, Str id0) {
    u8 x[32];
    if (rev < 3) {
        memcpy(x, k_pdf_pad, 32);
        rc4(key, key_n, x, 32);
        return !memcmp(x, u.p, 32);
    }
    Md5 m;
    md5_init(&m);
    md5_update(&m, k_pdf_pad, 32);
    md5_update(&m, id0.p, id0.n);
    md5_final(&m, x);
    for (u32 i = 0; i < 20; i++) {
        u8 k[16] = {0};
        for (u32 j = 0; j < key_n; j++) k[j] = (u8)(key[j] ^ i);
        rc4(k, key_n, x, 16);
    }
    return !memcmp(x, u.p, 16);
}

static b8 crypt_setup(Doc *d, char *err, size_t err_cap) {
    d->crypt_ready = true;
    if (!d->encrypt || d->encrypt->kind == PO_NULL) return true;
    PdfObj *e = pdf_resolve(d, d->encrypt);
    if (!e || e->kind != PO_DICT) {
        snprintf(err, err_cap, "its encryption dictionary is missing");
        return false;
    }
    if (!obj_is_name(dget(d, e, STR("Filter")), STR("Standard"))) {
        snprintf(err, err_cap, "it uses a security handler read cannot open");
        return false;
    }
    f64 v = 0, rev = 0, bits = 0, perms = 0;
    dget_num(d, e, STR("V"), &v);
    dget_num(d, e, STR("R"), &rev);
    b8 has_bits = dget_num(d, e, STR("Length"), &bits);
    dget_num(d, e, STR("P"), &perms);
    if (v >= 5 || rev >= 5) {
        snprintf(err, err_cap,
                 "it is encrypted with AES-256, which read cannot decrypt");
        return false;
    }
    PdfObj *o = dget(d, e, STR("O")), *u = dget(d, e, STR("U"));
    if (!o || o->kind != PO_STR || o->u.s.n < 32 || !u || u->kind != PO_STR
        || u->u.s.n < 32 || rev < 2
        || !(perms >= -4294967296.0 && perms <= 4294967295.0)) {
        snprintf(err, err_cap, "its encryption dictionary is malformed");
        return false;
    }
    u8 method = CRYPT_RC4;
    u32 key_n = v < 2 ? 5 : has_bits ? (u32)(bits / 8) : 16;
    if (v >= 4) {
        PdfObj *stmf = dget(d, e, STR("StmF"));
        Str stm_name = stmf && stmf->kind == PO_NAME ? stmf->u.s : STR("");
        PdfObj *cf = dget(d, dget(d, e, STR("CF")), stm_name);
        PdfObj *cfm = dget(d, cf, STR("CFM"));
        if (!stmf || obj_is_name(stmf, STR("Identity")) || !cf
            || obj_is_name(cfm, STR("None")))
            method = CRYPT_NONE;
        else if (obj_is_name(cfm, STR("AESV2")))
            method = CRYPT_AES;
        else if (!obj_is_name(cfm, STR("V2"))) {
            snprintf(err, err_cap, "it uses a cipher read cannot decrypt");
            return false;
        }
        if (method == CRYPT_AES) key_n = 16;
    }
    if (key_n < 5) key_n = 5;
    if (key_n > 16) key_n = 16;
    Str id0 = {0};
    PdfObj *id = pdf_resolve(d, d->id);
    if (id && id->kind == PO_ARRAY && id->n && id->u.items[0].kind == PO_STR)
        id0 = id->u.items[0].u.s;
    PdfObj *meta = dget(d, e, STR("EncryptMetadata"));
    u32 p = (u32)(i64)perms;
    u8 le[4] = {(u8)p, (u8)(p >> 8), (u8)(p >> 16), (u8)(p >> 24)};
    u8 hash[16];
    Md5 m;
    md5_init(&m);
    md5_update(&m, k_pdf_pad, 32);
    md5_update(&m, o->u.s.p, 32);
    md5_update(&m, le, 4);
    md5_update(&m, id0.p, id0.n);
    if (rev >= 4 && meta && meta->kind == PO_BOOL && !meta->flag)
        md5_update(&m, "\xff\xff\xff\xff", 4);
    md5_final(&m, hash);
    if (rev >= 3) {
        for (u32 i = 0; i < 50; i++) {
            md5_init(&m);
            md5_update(&m, hash, key_n);
            md5_final(&m, hash);
        }
    }
    if (!crypt_user_ok(hash, key_n, (u32)rev, u->u.s, id0)) {
        snprintf(err, err_cap,
                 "it is protected by a password; read opens only PDFs that "
                 "have no password to open them");
        return false;
    }
    d->crypt.method = method;
    d->crypt.key_n = key_n;
    memcpy(d->crypt.key, hash, key_n);
    return true;
}

static b8 doc_open(Doc *d, char *err, size_t err_cap) {
    if (!xref_load(d)) xref_rebuild(d);
    if (!crypt_setup(d, err, err_cap)) return false;
    if (d->rebuilt) objstm_register(d);
    if (!pdf_resolve(d, d->root) && !d->rebuilt) xref_rebuild(d);
    if (!pdf_resolve(d, d->root)) find_catalog(d);
    if (!pdf_resolve(d, d->root)) {
        snprintf(err, err_cap, "it has no document catalog; it may be damaged");
        return false;
    }
    return true;
}

/* ---- glyph names ---------------------------------------------------------- */

static u32 glyph_lookup(Str name) {
    size_t lo = 0, hi = sizeof k_pdf_glyphs / sizeof k_pdf_glyphs[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *g = k_pdf_glyphs[mid].name;
        size_t gn = strlen(g);
        i32 c = memcmp(g, name.p, gn < name.n ? gn : name.n);
        if (!c) c = gn < name.n ? -1 : gn > name.n ? 1 : 0;
        if (!c) return k_pdf_glyphs[mid].cp;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static u32 glyph_hex(Str s) {
    u32 v = 0;
    for (size_t i = 0; i < s.n; i++) {
        i32 h = pdf_hex_digit((u8)s.p[i]);
        if (h < 0) return 0;
        v = v * 16 + (u32)h;
    }
    return v;
}

/* A name from the glyph list, uniXXXX or uXXXX[XX], a ligature written with
 * underscores, or a variant (a.sc, parenleftbig) of one of those. Zero when
 * the name means nothing, as with g12 or cid34. */
static u32 glyph_cp(Str name) {
    if (!name.n || name.n > 64) return 0;
    u32 cp = glyph_lookup(name);
    if (cp) return cp;
    if (name.n >= 7 && str_starts(name, STR("uni")))
        return glyph_hex((Str){name.p + 3, 4});
    if (name.n >= 5 && name.n <= 7 && name.p[0] == 'u') {
        cp = glyph_hex(str_drop(name, 1));
        if (cp && cp <= 0x10FFFF) return cp;
    }
    static const struct {
        const char *name;
        u32 cp;
    } k_ligatures[] = {
        {"f_f", 0xFB00},   {"f_i", 0xFB01},   {"f_l", 0xFB02},
        {"f_f_i", 0xFB03}, {"f_f_l", 0xFB04},
    };
    for (size_t i = 0; i < sizeof k_ligatures / sizeof k_ligatures[0]; i++)
        if (str_eq(name, str_c(k_ligatures[i].name))) return k_ligatures[i].cp;
    const char *dot = memchr(name.p, '.', name.n);
    if (dot && dot > name.p)
        return glyph_cp((Str){name.p, (size_t)(dot - name.p)});
    static const char *const k_sizes[] = {"display", "bigg", "Bigg",
                                          "big",     "Big",  "text"};
    for (size_t i = 0; i < sizeof k_sizes / sizeof k_sizes[0]; i++) {
        Str s = str_c(k_sizes[i]);
        if (name.n > s.n && !memcmp(name.p + name.n - s.n, s.p, s.n))
            return glyph_lookup((Str){name.p, name.n - s.n});
    }
    return 0;
}

/* ---- fonts -----------------------------------------------------------------
 * What the text needs from a font: how codes split out of a string, what text
 * each code stands for, and how far each advances. A ToUnicode map wins over
 * the encoding, since it is the writer's own statement of the text. */

typedef struct {
    u32 lo, hi, seq;
    u32 cp;
    u32 *multi;
    u32 multi_n;
} UniRange;

typedef struct {
    u32 lo, hi, seq;
    u32 cid;
    f64 width;
} CodeRange;

typedef struct {
    u32 bytes, lo, hi;
} CodeSpace;

struct PdfFont {
    b8 composite;
    u32 nspace;
    CodeSpace space[PDF_MAX_CODESPACE];
    UniRange *uni;
    u32 nuni, cuni;
    CodeRange *cid;
    u32 ncid, ccid;
    CodeRange *wid;
    u32 nwid, cwid;
    u32 seq;
    f64 dw, scale, em;
    u32 enc[256];
    f64 width[256];
    u32 *gid_uni;
    u32 ngid;
    u16 *cid_gid;
    u32 ncid_gid;
};

static void *vec_room(Arena *a, void *p, u32 n, u32 *cap, size_t size) {
    if (n < *cap) return p;
    if (*cap > (1u << 22)) return NULL;
    u32 c = *cap ? *cap * 2 : 16;
    void *np = arena_alloc_array(a, c, size, 8);
    if (!np) return NULL;
    if (n) memcpy(np, p, (size_t)n * size);
    *cap = c;
    return np;
}

static void font_add_uni(Doc *d, PdfFont *f, u32 lo, u32 hi, const u32 *cps,
                         u32 n) {
    UniRange *p = vec_room(d->a, f->uni, f->nuni, &f->cuni, sizeof *p);
    if (!p) return;
    f->uni = p;
    UniRange r = {.lo = lo, .hi = hi, .seq = f->seq++};
    if (n == 1) {
        r.cp = cps[0];
    } else if (n > 1) {
        r.multi = arena_new(d->a, u32, n);
        if (!r.multi) return;
        memcpy(r.multi, cps, n * sizeof *cps);
        r.multi_n = n;
    }
    f->uni[f->nuni++] = r;
}

static void font_add_range(Doc *d, CodeRange **v, u32 *n, u32 *cap,
                           CodeRange r) {
    CodeRange *p = vec_room(d->a, *v, *n, cap, sizeof *p);
    if (!p) return;
    *v = p;
    p[(*n)++] = r;
}

static b8 hex_code(Str raw, u32 *code, u32 *bytes) {
    StrIter it = {raw.p, raw.n, 0, true};
    u32 v = 0, n = 0;
    for (i32 c; (c = str_iter_next(&it)) >= 0;) {
        if (n == 4) return false;
        v = v << 8 | (u32)c;
        n++;
    }
    if (!n) return false;
    *code = v;
    *bytes = n;
    return true;
}

static u32 utf16_cps(Str raw, u32 out[PDF_MAX_GLYPH_TEXT]) {
    StrIter it = {raw.p, raw.n, 0, true};
    u32 units[PDF_MAX_GLYPH_TEXT], nu = 0, n = 0;
    for (i32 b1; nu < PDF_MAX_GLYPH_TEXT && (b1 = str_iter_next(&it)) >= 0;) {
        i32 b2 = str_iter_next(&it);
        units[nu++] = b2 < 0 ? (u32)b1 : (u32)(b1 << 8 | b2);
        if (b2 < 0) break;
    }
    for (u32 i = 0; i < nu; i++) {
        u32 u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < nu && units[i + 1] >= 0xDC00
            && units[i + 1] <= 0xDFFF) {
            u = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            i++;
        }
        out[n++] = u;
    }
    return n;
}

static void cmap_parse(Doc *d, Str data, PdfFont *f, b8 to_unicode) {
    Lex lx = {data.p, data.n, 0};
    u32 cps[PDF_MAX_GLYPH_TEXT];
    for (;;) {
        Tok t = lex_next(&lx);
        if (t.kind == TK_EOF) break;
        if (t.kind != TK_WORD) continue;
        if (str_eq(t.raw, STR("begincodespacerange"))) {
            for (;;) {
                Tok a = lex_next(&lx);
                if (a.kind != TK_HEX) break;
                Tok b = lex_next(&lx);
                u32 lo, hi, n1, n2;
                if (b.kind != TK_HEX) break;
                if (!to_unicode && hex_code(a.raw, &lo, &n1)
                    && hex_code(b.raw, &hi, &n2) && n1 == n2
                    && f->nspace < PDF_MAX_CODESPACE)
                    f->space[f->nspace++] = (CodeSpace){n1, lo, hi};
            }
        } else if (str_eq(t.raw, STR("beginbfchar")) && to_unicode) {
            for (;;) {
                Tok a = lex_next(&lx);
                if (a.kind != TK_HEX) break;
                Tok b = lex_next(&lx);
                u32 code, n;
                if (b.kind != TK_HEX && b.kind != TK_NAME) break;
                if (!hex_code(a.raw, &code, &n)) continue;
                if (b.kind == TK_HEX) {
                    font_add_uni(d, f, code, code, cps, utf16_cps(b.raw, cps));
                } else if ((cps[0] = glyph_cp(b.raw))) {
                    font_add_uni(d, f, code, code, cps, 1);
                }
            }
        } else if (str_eq(t.raw, STR("beginbfrange")) && to_unicode) {
            for (;;) {
                Tok a = lex_next(&lx);
                if (a.kind != TK_HEX) break;
                Tok b = lex_next(&lx), c = lex_next(&lx);
                u32 lo, hi, n;
                if (b.kind != TK_HEX) break;
                b8 ok = hex_code(a.raw, &lo, &n) && hex_code(b.raw, &hi, &n)
                        && lo <= hi;
                if (c.kind == TK_HEX) {
                    if (ok)
                        font_add_uni(d, f, lo, hi, cps, utf16_cps(c.raw, cps));
                } else if (c.kind == TK_ARRAY_OPEN) {
                    for (u32 k = 0;; k++) {
                        Tok e = lex_next(&lx);
                        if (e.kind != TK_HEX) break;
                        if (ok && k <= hi - lo)
                            font_add_uni(d, f, lo + k, lo + k, cps,
                                         utf16_cps(e.raw, cps));
                    }
                } else {
                    break;
                }
            }
        } else if ((str_eq(t.raw, STR("begincidrange"))
                    || str_eq(t.raw, STR("begincidchar")))
                   && !to_unicode) {
            b8 range = str_eq(t.raw, STR("begincidrange"));
            for (;;) {
                Tok a = lex_next(&lx);
                if (a.kind != TK_HEX) break;
                Tok b = range ? lex_next(&lx) : a;
                Tok c = lex_next(&lx);
                u32 lo, hi, n, cid;
                if (b.kind != TK_HEX || c.kind != TK_NUM) break;
                if (hex_code(a.raw, &lo, &n) && hex_code(b.raw, &hi, &n)
                    && lo <= hi && pdf_u32(c.num, &cid))
                    font_add_range(d, &f->cid, &f->ncid, &f->ccid,
                                   (CodeRange){lo, hi, f->seq++, cid, 0});
            }
        }
    }
}

static int uni_cmp(const void *pa, const void *pb) {
    const UniRange *a = pa, *b = pb;
    if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

static int range_cmp(const void *pa, const void *pb) {
    const CodeRange *a = pa, *b = pb;
    if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

static const UniRange *uni_find(const PdfFont *f, u32 code) {
    size_t lo = 0, hi = f->nuni;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->uni[mid].lo <= code)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t k = lo; k > 0 && lo - k < 8; k--)
        if (f->uni[k - 1].hi >= code) return &f->uni[k - 1];
    return NULL;
}

static const CodeRange *range_find(const CodeRange *r, u32 n, u32 code) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (r[mid].lo <= code)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t k = lo; k > 0 && lo - k < 8; k--)
        if (r[k - 1].hi >= code) return &r[k - 1];
    return NULL;
}

static b8 stream_of(Doc *d, PdfObj *ref, Arena *a, Str *out) {
    if (!ref || ref->kind != PO_REF) return false;
    Str raw;
    PdfObj *s = doc_obj(d, ref->u.ref.num, &raw);
    return s && raw.p && stream_decode(d, s, raw, ref->u.ref.num, true, a, out);
}

static b8 type1_encoding(Doc *d, PdfObj *desc, PdfFont *f) {
    PdfObj *ref = dict_get(desc, STR("FontFile"));
    Str data;
    if (!stream_of(d, ref, d->page, &data)) return false;
    f64 clear;
    PdfObj *s = doc_obj(d, ref->u.ref.num, NULL);
    if (dget_num(d, s, STR("Length1"), &clear) && clear > 0
        && clear < (f64)data.n)
        data.n = (size_t)clear;
    size_t at = pdf_find(data, 0, STR("/Encoding"));
    if (at == PDF_NOT_FOUND) return false;
    Lex lx = {data.p, data.n, at + 9};
    Tok t = lex_next(&lx);
    if (tok_word(&t, STR("StandardEncoding"))) {
        for (u32 c = 0; c < 256; c++) f->enc[c] = k_enc_standard[c];
        return true;
    }
    b8 any = false;
    for (; t.kind != TK_EOF; t = lex_next(&lx)) {
        if (tok_word(&t, STR("def")) || tok_word(&t, STR("eexec"))) break;
        if (!tok_word(&t, STR("dup"))) continue;
        Tok code = lex_next(&lx), name = lex_next(&lx), put = lex_next(&lx);
        if (code.kind == TK_NUM && code.num >= 0 && code.num < 256
            && name.kind == TK_NAME && tok_word(&put, STR("put"))) {
            f->enc[(u32)code.num] = glyph_cp(name.raw);
            any = true;
        }
    }
    return any;
}

/* ---- embedded font programs ------------------------------------------------
 * The encoding built into a CFF font, for a simple font that states none, and
 * the cmap of a TrueType font, for a composite font with no ToUnicode map.
 * Only the tables that name or number glyphs are read, and every offset in
 * them is checked against the font's bytes. */

#define PDF_MAX_GLYPHS     65536u
#define PDF_MAX_CMAP_STEPS (1u << 20)

static u32 be16_at(Str s, size_t at) {
    return (u32)((u8)s.p[at] << 8 | (u8)s.p[at + 1]);
}

static u32 be32_at(Str s, size_t at) {
    return be16_at(s, at) << 16 | be16_at(s, at + 2);
}

typedef struct {
    Str data;
    u32 count, off_size;
    size_t offs_at, base, end;
} CffIndex;

static u64 cff_off(const CffIndex *ix, u32 i) {
    u64 v = 0;
    const char *p = ix->data.p + ix->offs_at + (size_t)i * ix->off_size;
    for (u32 k = 0; k < ix->off_size; k++) v = v << 8 | (u8)p[k];
    return v;
}

static b8 cff_index(Str s, size_t at, CffIndex *ix) {
    *ix = (CffIndex){.data = s};
    if (at > s.n || s.n - at < 2) return false;
    ix->count = be16_at(s, at);
    if (!ix->count) {
        ix->end = at + 2;
        return true;
    }
    if (s.n - at < 3) return false;
    ix->off_size = (u8)s.p[at + 2];
    ix->offs_at = at + 3;
    if (ix->off_size < 1 || ix->off_size > 4
        || (u64)(ix->count + 1) * ix->off_size > s.n - ix->offs_at)
        return false;
    ix->base = ix->offs_at + (size_t)(ix->count + 1) * ix->off_size - 1;
    u64 last = cff_off(ix, ix->count);
    if (last < 1 || last > s.n - ix->base) return false;
    ix->end = ix->base + (size_t)last;
    return true;
}

static b8 cff_item(const CffIndex *ix, u32 i, Str *out) {
    if (i >= ix->count) return false;
    u64 a = cff_off(ix, i), b = cff_off(ix, i + 1);
    if (a < 1 || b < a || b > ix->end - ix->base) return false;
    *out = (Str){ix->data.p + ix->base + a, (size_t)(b - a)};
    return true;
}

typedef struct {
    f64 charset, encoding, charstrings;
    b8 cid;
} CffTop;

static void cff_top(Str dict, CffTop *t) {
    f64 stack[48];
    u32 n = 0;
    for (size_t i = 0; i < dict.n;) {
        u8 b0 = (u8)dict.p[i++];
        if (b0 <= 21) {
            u32 op = b0;
            if (b0 == 12) {
                if (i >= dict.n) return;
                op = 1200 + (u8)dict.p[i++];
            }
            if (n && op == 15) t->charset = stack[n - 1];
            if (n && op == 16) t->encoding = stack[n - 1];
            if (n && op == 17) t->charstrings = stack[n - 1];
            if (op == 1230) t->cid = true;
            n = 0;
            continue;
        }
        f64 v = 0;
        size_t left = dict.n - i;
        if (b0 == 28 && left >= 2) {
            v = (f64)(i16)(u16)be16_at(dict, i);
            i += 2;
        } else if (b0 == 29 && left >= 4) {
            v = (f64)(i32)be32_at(dict, i);
            i += 4;
        } else if (b0 == 30) {
            while (i < dict.n && ((u8)dict.p[i] & 0x0F) != 0x0F
                   && ((u8)dict.p[i] >> 4) != 0x0F)
                i++;
            i++;
        } else if (b0 >= 32 && b0 <= 246) {
            v = (f64)b0 - 139;
        } else if (b0 >= 247 && b0 <= 250 && left >= 1) {
            v = (f64)(b0 - 247) * 256 + (u8)dict.p[i++] + 108;
        } else if (b0 >= 251 && b0 <= 254 && left >= 1) {
            v = -(f64)(b0 - 251) * 256 - (u8)dict.p[i++] - 108;
        } else {
            return;
        }
        if (n < 48) stack[n++] = v;
    }
}

static b8 cff_charset(Str s, f64 off, u32 nglyphs, u16 *sid) {
    memset(sid, 0, nglyphs * sizeof *sid);
    if (off == 0) {
        for (u32 g = 1; g < nglyphs && g < 229; g++) sid[g] = (u16)g;
        return true;
    }
    if (!(off > 2 && off < (f64)s.n)) return false;
    size_t at = (size_t)off;
    u8 format = (u8)s.p[at++];
    u32 g = 1;
    if (format == 0) {
        for (; g < nglyphs && s.n - at >= 2; g++, at += 2)
            sid[g] = (u16)be16_at(s, at);
        return true;
    }
    if (format != 1 && format != 2) return false;
    size_t need = format == 1 ? 3 : 4;
    while (g < nglyphs && s.n - at >= need) {
        u32 first = be16_at(s, at);
        u32 left = format == 1 ? (u8)s.p[at + 2] : be16_at(s, at + 2);
        at += need;
        for (u32 k = 0; k <= left && g < nglyphs; k++, g++)
            sid[g] = (u16)(first + k);
    }
    return true;
}

static Str cff_sid_name(const CffIndex *strings, u32 sid) {
    if (sid < 391) return str_c(k_cff_std_strings[sid]);
    Str s;
    return cff_item(strings, sid - 391, &s) ? s : (Str){0};
}

static b8 cff_encoding(Str s, f64 off, u32 nglyphs, const u16 *sid,
                       const CffIndex *strings, PdfFont *f) {
    if (off == 0) {
        for (u32 c = 0; c < 256; c++) f->enc[c] = k_enc_standard[c];
        return true;
    }
    if (!(off > 1 && off < (f64)s.n)) return false;
    size_t at = (size_t)off;
    u8 format = (u8)s.p[at++];
    u32 gid = 1;
    if ((format & 0x7F) == 0 && at < s.n) {
        u32 n = (u8)s.p[at++];
        for (u32 i = 0; i < n && at < s.n; i++, gid++) {
            u8 code = (u8)s.p[at++];
            if (gid < nglyphs)
                f->enc[code] = glyph_cp(cff_sid_name(strings, sid[gid]));
        }
    } else if ((format & 0x7F) == 1 && at < s.n) {
        u32 ranges = (u8)s.p[at++];
        for (u32 r = 0; r < ranges && s.n - at >= 2; r++, at += 2) {
            u32 first = (u8)s.p[at], left = (u8)s.p[at + 1];
            for (u32 k = 0; k <= left && first + k < 256; k++, gid++)
                if (gid < nglyphs)
                    f->enc[first + k] =
                        glyph_cp(cff_sid_name(strings, sid[gid]));
        }
    } else {
        return false;
    }
    if ((format & 0x80) && at < s.n) {
        u32 sups = (u8)s.p[at++];
        for (u32 i = 0; i < sups && s.n - at >= 3; i++, at += 3)
            f->enc[(u8)s.p[at]] =
                glyph_cp(cff_sid_name(strings, be16_at(s, at + 1)));
    }
    return true;
}

static b8 cff_font_encoding(Doc *d, PdfObj *desc, PdfFont *f) {
    PdfObj *ref = dict_get(desc, STR("FontFile3"));
    Str s;
    if (!stream_of(d, ref, d->page, &s) || s.n < 4) return false;
    PdfObj *stm = doc_obj(d, ref->u.ref.num, NULL);
    if (!obj_is_name(dget(d, stm, STR("Subtype")), STR("Type1C"))) return false;
    CffIndex names, top, strings, glyphs;
    Str dict;
    if (!cff_index(s, (u8)s.p[2], &names) || !cff_index(s, names.end, &top)
        || !cff_index(s, top.end, &strings) || !cff_item(&top, 0, &dict))
        return false;
    CffTop t = {0};
    cff_top(dict, &t);
    if (t.cid || !(t.charstrings > 0 && t.charstrings < (f64)s.n)
        || !cff_index(s, (size_t)t.charstrings, &glyphs) || !glyphs.count
        || glyphs.count > PDF_MAX_GLYPHS)
        return false;
    u16 *sid = arena_new(d->page, u16, glyphs.count);
    return sid && cff_charset(s, t.charset, glyphs.count, sid)
           && cff_encoding(s, t.encoding, glyphs.count, sid, &strings, f);
}

static b8 tt_table(Str s, const char *tag, Str *out) {
    if (s.n < 12) return false;
    u32 n = be16_at(s, 4);
    if ((u64)n * 16 > s.n - 12) return false;
    for (u32 i = 0; i < n; i++) {
        size_t rec = 12 + (size_t)i * 16;
        if (memcmp(s.p + rec, tag, 4)) continue;
        u32 off = be32_at(s, rec + 8), len = be32_at(s, rec + 12);
        if (off > s.n || len > s.n - off) return false;
        *out = (Str){s.p + off, len};
        return true;
    }
    return false;
}

static void tt_map(PdfFont *f, u32 gid, u32 cp) {
    if (gid && gid < f->ngid && !f->gid_uni[gid] && cp >= 0x20)
        f->gid_uni[gid] = cp;
}

static u32 tt_symbol_cp(u32 c, b8 symbol) {
    if (c >= 0xF000 && c <= 0xF0FF) c &= 0xFF;
    if (c > 0xFF) return c;
    return symbol ? k_enc_symbol[c] : k_enc_winansi[c];
}

static void tt_cmap_format4(Str t, PdfFont *f, b8 remap, b8 symbol) {
    if (t.n < 14) return;
    u32 segs = be16_at(t, 6) / 2;
    size_t ends = 14, starts = ends + 2 * (size_t)segs + 2;
    size_t deltas = starts + 2 * (size_t)segs,
           ranges = deltas + 2 * (size_t)segs;
    if (ranges + 2 * (size_t)segs > t.n) return;
    u32 steps = 0;
    for (u32 i = 0; i < segs; i++) {
        u32 end = be16_at(t, ends + 2 * i), start = be16_at(t, starts + 2 * i);
        u32 delta = be16_at(t, deltas + 2 * i);
        size_t ro_at = ranges + 2 * (size_t)i;
        u32 ro = be16_at(t, ro_at);
        for (u32 c = start; c <= end && c < 0xFFFF; c++) {
            if (++steps > PDF_MAX_CMAP_STEPS) return;
            u32 gid;
            if (!ro) {
                gid = (c + delta) & 0xFFFF;
            } else {
                size_t at = ro_at + ro + 2 * (size_t)(c - start);
                if (at + 2 > t.n) break;
                gid = be16_at(t, at);
                if (gid) gid = (gid + delta) & 0xFFFF;
            }
            tt_map(f, gid, remap ? tt_symbol_cp(c, symbol) : c);
        }
    }
}

static void tt_cmap_format12(Str t, PdfFont *f) {
    if (t.n < 16) return;
    u32 groups = be32_at(t, 12);
    if ((u64)groups * 12 > t.n - 16) return;
    u32 steps = 0;
    for (u32 i = 0; i < groups; i++) {
        size_t at = 16 + (size_t)i * 12;
        u32 start = be32_at(t, at), end = be32_at(t, at + 4);
        u32 gid = be32_at(t, at + 8);
        for (u32 c = start; c <= end && c <= 0x10FFFF && gid < f->ngid;
             c++, gid++) {
            if (++steps > PDF_MAX_CMAP_STEPS) return;
            tt_map(f, gid, c);
        }
    }
}

static void tt_cmap(Str font, PdfFont *f, b8 symbol) {
    Str cmap;
    if (!tt_table(font, "cmap", &cmap) || cmap.n < 4) return;
    u32 n = be16_at(cmap, 2);
    if ((u64)n * 8 > cmap.n - 4) return;
    i32 best = -1, best_rank = 0;
    for (u32 i = 0; i < n; i++) {
        size_t rec = 4 + (size_t)i * 8;
        u32 platform = be16_at(cmap, rec), encoding = be16_at(cmap, rec + 2);
        u32 off = be32_at(cmap, rec + 4);
        if (off > cmap.n - 2) continue;
        u32 format = be16_at(cmap, off);
        i32 rank = 0;
        if (platform == 3 && encoding == 10 && format == 12)
            rank = 4;
        else if (platform == 3 && encoding == 1 && format == 4)
            rank = 3;
        else if (platform == 0 && (format == 4 || format == 12))
            rank = 2;
        else if (platform == 3 && encoding == 0 && format == 4)
            rank = 1;
        if (rank > best_rank) {
            best = (i32)off;
            best_rank = rank;
        }
    }
    if (best < 0) return;
    Str t = str_drop(cmap, (size_t)best);
    if (be16_at(t, 0) == 12)
        tt_cmap_format12(t, f);
    else
        tt_cmap_format4(t, f, best_rank == 1, symbol);
}

static void font_truetype_text(Doc *d, PdfObj *cf, PdfFont *f, b8 symbol) {
    if (!obj_is_name(dget(d, cf, STR("Subtype")), STR("CIDFontType2"))) return;
    PdfObj *desc = dget(d, cf, STR("FontDescriptor"));
    Str font, maxp;
    if (!stream_of(d, dict_get(desc, STR("FontFile2")), d->page, &font)) return;
    u32 ng = PDF_MAX_GLYPHS;
    if (tt_table(font, "maxp", &maxp) && maxp.n >= 6) ng = be16_at(maxp, 4);
    if (!ng) return;
    f->gid_uni = arena_new(d->a, u32, ng);
    if (!f->gid_uni) return;
    memset(f->gid_uni, 0, ng * sizeof *f->gid_uni);
    f->ngid = ng;
    tt_cmap(font, f, symbol);
    PdfObj *map = dict_get(cf, STR("CIDToGIDMap"));
    Str gids;
    if (map && map->kind == PO_REF && stream_of(d, map, d->page, &gids)) {
        f->ncid_gid =
            (u32)(gids.n / 2 < PDF_MAX_GLYPHS ? gids.n / 2 : PDF_MAX_GLYPHS);
        f->cid_gid = arena_new(d->a, u16, f->ncid_gid ? f->ncid_gid : 1);
        if (!f->cid_gid) {
            f->ngid = 0;
            return;
        }
        for (u32 i = 0; i < f->ncid_gid; i++)
            f->cid_gid[i] = (u16)be16_at(gids, 2 * (size_t)i);
    }
}

static const u16 *encoding_named(Str name) {
    if (str_eq(name, STR("WinAnsiEncoding"))) return k_enc_winansi;
    if (str_eq(name, STR("MacRomanEncoding"))) return k_enc_macroman;
    if (str_eq(name, STR("StandardEncoding"))) return k_enc_standard;
    return NULL;
}

static b8 name_has(Str s, Str part) {
    return pdf_find(s, 0, part) != PDF_NOT_FOUND;
}

static void font_simple_encoding(Doc *d, PdfObj *fd, PdfObj *desc, PdfFont *f,
                                 b8 type3, b8 truetype) {
    PdfObj *enc = dget(d, fd, STR("Encoding"));
    PdfObj *diffs = NULL;
    const u16 *base = NULL;
    if (enc && enc->kind == PO_NAME) {
        base = encoding_named(enc->u.s);
    } else if (enc && enc->kind == PO_DICT) {
        PdfObj *b = dget(d, enc, STR("BaseEncoding"));
        if (b && b->kind == PO_NAME) base = encoding_named(b->u.s);
        diffs = dget(d, enc, STR("Differences"));
    }
    PdfObj *bf = dget(d, fd, STR("BaseFont"));
    Str font_name = bf && bf->kind == PO_NAME ? bf->u.s : STR("");
    if (!base
        && (type3
            || (!type1_encoding(d, desc, f)
                && !cff_font_encoding(d, desc, f)))) {
        if (name_has(font_name, STR("Symbol")))
            base = k_enc_symbol;
        else if (truetype)
            base = k_enc_winansi;
        else
            base = k_enc_standard;
    }
    if (base)
        for (u32 c = 0; c < 256; c++) f->enc[c] = base[c];
    if (!diffs || diffs->kind != PO_ARRAY) return;
    f64 code = 0;
    for (u32 i = 0; i < diffs->n; i++) {
        PdfObj *it = pdf_resolve(d, &diffs->u.items[i]);
        if (obj_num(it, &code)) continue;
        if (it && it->kind == PO_NAME && code >= 0 && code < 256) {
            u32 cp = glyph_cp(it->u.s);
            if (!cp && type3 && code >= 0x21 && code <= 0x7E) cp = (u32)code;
            f->enc[(u32)code] = cp;
            code++;
        }
    }
}

typedef struct {
    const PdfWidth *w;
    size_t n;
    u16 fixed;
} Base14;

#define BASE14(t) ((Base14){t, sizeof t / sizeof t[0], 0})

static Base14 base14_widths(Str name) {
    const char *plus = memchr(name.p, '+', name.n);
    if (plus && plus - name.p == 6) name = str_drop(name, 7);
    b8 bold = name_has(name, STR("Bold")) || name_has(name, STR("Black"));
    b8 italic = name_has(name, STR("Italic")) || name_has(name, STR("Oblique"));
    if (name_has(name, STR("Courier"))) return (Base14){NULL, 0, 600};
    if (name_has(name, STR("Helvetica")) || name_has(name, STR("Arial")))
        return bold ? BASE14(k_w_helvetica_bold) : BASE14(k_w_helvetica);
    if (name_has(name, STR("Times")))
        return bold && italic ? BASE14(k_w_times_bold_italic)
               : bold         ? BASE14(k_w_times_bold)
               : italic       ? BASE14(k_w_times_italic)
                              : BASE14(k_w_times);
    if (name_has(name, STR("Symbol"))) return BASE14(k_w_symbol);
    return (Base14){0};
}

static f64 base14_width(Base14 b, u32 cp) {
    if (b.fixed) return b.fixed;
    size_t lo = 0, hi = b.n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (b.w[mid].cp == cp) return b.w[mid].width;
        if (b.w[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static void font_simple_widths(Doc *d, PdfObj *fd, PdfObj *desc, PdfFont *f) {
    f64 missing = 0, first = 0;
    PdfObj *w = dget(d, fd, STR("Widths"));
    dget_num(d, desc, STR("MissingWidth"), &missing);
    if (!w || w->kind != PO_ARRAY) {
        PdfObj *bf = dget(d, fd, STR("BaseFont"));
        Base14 b = base14_widths(bf && bf->kind == PO_NAME ? bf->u.s : STR(""));
        if (!missing) missing = 500;
        for (u32 c = 0; c < 256; c++) {
            f64 v =
                f->enc[c] && (b.n || b.fixed) ? base14_width(b, f->enc[c]) : 0;
            f->width[c] = (v ? v : missing) * f->scale;
        }
        return;
    }
    for (u32 c = 0; c < 256; c++) f->width[c] = missing * f->scale;
    dget_num(d, fd, STR("FirstChar"), &first);
    for (u32 i = 0; i < w->n; i++) {
        f64 code = first + i, v;
        if (code >= 0 && code < 256
            && obj_num(pdf_resolve(d, &w->u.items[i]), &v))
            f->width[(u32)code] = v * f->scale;
    }
}

static void font_cid_widths(Doc *d, PdfObj *cf, PdfFont *f) {
    f64 dw = 1000;
    dget_num(d, cf, STR("DW"), &dw);
    f->dw = dw * f->scale;
    PdfObj *w = dget(d, cf, STR("W"));
    if (!w || w->kind != PO_ARRAY) return;
    for (u32 i = 0; i < w->n;) {
        f64 c0, c1, v;
        PdfObj *next = i + 1 < w->n ? pdf_resolve(d, &w->u.items[i + 1]) : NULL;
        if (!obj_num(pdf_resolve(d, &w->u.items[i]), &c0) || c0 < 0
            || c0 > (f64)UINT32_MAX || !next)
            break;
        if (next->kind == PO_ARRAY) {
            for (u32 k = 0; k < next->n && c0 + k <= (f64)UINT32_MAX; k++)
                if (obj_num(pdf_resolve(d, &next->u.items[k]), &v))
                    font_add_range(d, &f->wid, &f->nwid, &f->cwid,
                                   (CodeRange){(u32)c0 + k, (u32)c0 + k,
                                               f->seq++, 0, v * f->scale});
            i += 2;
            continue;
        }
        if (i + 2 >= w->n || !obj_num(next, &c1)
            || !obj_num(pdf_resolve(d, &w->u.items[i + 2]), &v) || c1 < c0
            || c1 > (f64)UINT32_MAX)
            break;
        font_add_range(
            d, &f->wid, &f->nwid, &f->cwid,
            (CodeRange){(u32)c0, (u32)c1, f->seq++, 0, v * f->scale});
        i += 3;
    }
}

/* A Type3 font gives no em size, so the reader takes an average glyph to be
 * half an em wide, in glyph space, and scales that to text space by `a`. */
static f64 type3_em(Doc *d, PdfObj *fd, f64 a) {
    PdfObj *w = dget(d, fd, STR("Widths"));
    f64 sum = 0, v;
    u32 n = 0;
    for (u32 i = 0; w && w->kind == PO_ARRAY && i < w->n; i++) {
        if (obj_num(pdf_resolve(d, &w->u.items[i]), &v) && v > 0) {
            sum += v;
            n++;
        }
    }
    return n ? 2 * sum / n * pdf_abs(a) : 1000 * pdf_abs(a);
}

static PdfFont *font_load(Doc *d, PdfObj *fd) {
    PdfFont *f = arena_new(d->a, PdfFont, 1);
    if (!f) return NULL;
    memset(f, 0, sizeof *f);
    f->scale = 0.001;
    f->em = 1;
    PdfObj *subtype = dget(d, fd, STR("Subtype"));
    b8 type3 = obj_is_name(subtype, STR("Type3"));
    Str data;
    PdfObj *cf = NULL;
    if (obj_is_name(subtype, STR("Type0"))) {
        f->composite = true;
        PdfObj *df = dget(d, fd, STR("DescendantFonts"));
        cf = df && df->kind == PO_ARRAY && df->n
                 ? pdf_resolve(d, &df->u.items[0])
                 : NULL;
        font_cid_widths(d, cf, f);
        if (stream_of(d, dict_get(fd, STR("Encoding")), d->page, &data))
            cmap_parse(d, data, f, false);
        if (f->ncid) qsort(f->cid, f->ncid, sizeof *f->cid, range_cmp);
        if (f->nwid) qsort(f->wid, f->nwid, sizeof *f->wid, range_cmp);
    } else {
        PdfObj *desc = dget(d, fd, STR("FontDescriptor"));
        PdfObj *fm = dget(d, fd, STR("FontMatrix"));
        f64 a, dd;
        if (type3 && fm && fm->kind == PO_ARRAY && fm->n == 6
            && obj_num(&fm->u.items[0], &a) && obj_num(&fm->u.items[3], &dd)
            && a != 0 && dd != 0) {
            f->scale = a;
            f->em = pdf_abs(dd / a);
        }
        font_simple_encoding(d, fd, desc, f, type3,
                             obj_is_name(subtype, STR("TrueType")));
        font_simple_widths(d, fd, desc, f);
        if (type3) f->em *= type3_em(d, fd, f->scale);
    }
    if (stream_of(d, dict_get(fd, STR("ToUnicode")), d->page, &data))
        cmap_parse(d, data, f, true);
    if (f->nuni) qsort(f->uni, f->nuni, sizeof *f->uni, uni_cmp);
    if (f->composite && !f->nuni) {
        PdfObj *bf = dget(d, fd, STR("BaseFont"));
        font_truetype_text(d, cf, f,
                           bf && bf->kind == PO_NAME
                               && name_has(bf->u.s, STR("Symbol")));
    }
    return f;
}

static PdfFont *font_get(Doc *d, PdfObj *fd) {
    u32 slot = (u32)(((uintptr_t)fd >> 4) % PDF_FONT_SLOTS);
    for (u32 probe = 0; probe < PDF_FONT_SLOTS; probe++) {
        u32 i = (slot + probe) % PDF_FONT_SLOTS;
        if (d->font_key[i] == fd) return d->font_val[i];
        if (!d->font_key[i]) {
            PdfFont *f = font_load(d, fd);
            if (f) {
                d->font_key[i] = fd;
                d->font_val[i] = f;
            }
            return f;
        }
    }
    return font_load(d, fd);
}

static u32 font_cid(const PdfFont *f, u32 code) {
    const CodeRange *c = range_find(f->cid, f->ncid, code);
    return c ? c->cid + (code - c->lo) : code;
}

static f64 font_width(const PdfFont *f, u32 code) {
    if (!f->composite) return f->width[code & 0xFF];
    const CodeRange *w = range_find(f->wid, f->nwid, font_cid(f, code));
    return w ? w->width : f->dw;
}

/* The code points `code` stands for in `out`. Zero when the font maps it to
 * nothing; -1 when nothing in the font says what it is. */
static i32 font_text(const PdfFont *f, u32 code, u32 out[PDF_MAX_GLYPH_TEXT]) {
    const UniRange *r = uni_find(f, code);
    if (r && r->multi_n) {
        memcpy(out, r->multi, r->multi_n * sizeof *out);
        out[r->multi_n - 1] += code - r->lo;
        return (i32)r->multi_n;
    }
    if (r && r->cp >= 0x20) {
        out[0] = r->cp + (code - r->lo);
        return 1;
    }
    if (!f->composite && code < 256 && f->enc[code]) {
        out[0] = f->enc[code];
        return 1;
    }
    if (!r && f->composite && f->ngid) {
        u32 cid = font_cid(f, code), gid = cid;
        if (f->cid_gid) gid = cid < f->ncid_gid ? f->cid_gid[cid] : 0;
        if (gid < f->ngid && f->gid_uni[gid]) {
            out[0] = f->gid_uni[gid];
            return 1;
        }
    }
    return r ? 0 : -1;
}

static b8 code_complete(const PdfFont *f, u32 code, u32 len) {
    if (!f->nspace) return len >= 2;
    u32 longest = 1;
    for (u32 i = 0; i < f->nspace; i++) {
        const CodeSpace *s = &f->space[i];
        if (s->bytes == len && code >= s->lo && code <= s->hi) return true;
        if (s->bytes > longest) longest = s->bytes;
    }
    return len >= longest;
}

/* ---- page content -----------------------------------------------------------
 * The content stream runs for its text state alone. Each glyph's start and
 * end are taken to device space and then to coordinates along its baseline
 * (u) and across it (v), so rotated pages lay out like upright ones. A jump
 * in v starts a line, a gap in u wider than a fraction of the font size
 * becomes a space, and everything else follows in drawing order. */

typedef struct {
    f64 a, b, c, d, e, f;
} Mat;

static Mat mat_mul(Mat x, Mat y) {
    return (Mat){
        x.a * y.a + x.b * y.c,       x.a * y.b + x.b * y.d,
        x.c * y.a + x.d * y.c,       x.c * y.b + x.d * y.d,
        x.e * y.a + x.f * y.c + y.e, x.e * y.b + x.f * y.d + y.f,
    };
}

static const Mat k_identity = {1, 0, 0, 1, 0, 0};

typedef struct {
    Mat ctm;
    PdfFont *font;
    f64 size, tc, tw, th, tl, rise;
} GState;

/* INVARIANT: assigned whole at the start of each page. */
typedef struct {
    b8 any, space, line_empty;
    i32 dir;
    u32 newlines, last_cp;
    f64 line_v, line_size, start_u, end_u;
    u32 accent;
    size_t accent_at;
    f64 accent_end;
} Layout;

typedef struct {
    TokKind kind;
    f64 num;
    Str raw;
} Operand;

typedef struct {
    Doc *d;
    Buf *out;
    PdfFont *fallback;
    Layout lay;
    GState gs;
    GState stack[PDF_MAX_GSTATE];
    u32 sp, lost;
    Mat tm, tlm;
    u32 op_count, glyphs, unmapped;
    b8 over_budget;
    u32 form_num[PDF_FORM_SLOTS];
    Str form_data[PDF_FORM_SLOTS];
    u32 forms;
    Operand *ops[PDF_MAX_FORM_DEPTH + 1];
} Page;

static void out_newline(Page *pg, b8 blank) {
    Buf *b = pg->out;
    while (b->n && b->p[b->n - 1] == ' ') b->n--;
    u32 want = blank ? 2 : 1;
    while (pg->lay.newlines < want) {
        buf_putc(b, '\n');
        pg->lay.newlines++;
    }
    pg->lay.space = false;
    pg->lay.line_empty = true;
}

static u32 symbol_piece(u32 cp) {
    static const u16 k_pieces[] = {
        0x23AF, 0x23D0, 0x23AF, 0x00AE, 0x00A9, 0x2122, 0x239B, 0x239C, 0x239D,
        0x23A1, 0x23A2, 0x23A3, 0x23A7, 0x23A8, 0x23A9, 0x23AA, 0x23AE, 0x239E,
        0x239F, 0x23A0, 0x23A4, 0x23A5, 0x23A6, 0x23AB, 0x23AC, 0x23AD,
    };
    if (cp >= 0xF8E5 && cp <= 0xF8FE) return k_pieces[cp - 0xF8E5];
    return cp;
}

static u32 accent_compose(u32 accent, u32 base) {
    size_t lo = 0, hi = sizeof k_pdf_accents / sizeof k_pdf_accents[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const PdfAccent *e = &k_pdf_accents[mid];
        if (e->accent == accent && e->base == base) return e->composed;
        if (e->accent < accent || (e->accent == accent && e->base < base))
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static b8 is_accent(u32 cp) {
    return accent_compose(cp, 'e') || accent_compose(cp, 'c')
           || accent_compose(cp, 'a');
}

static void out_cp(Page *pg, u32 cp) {
    static const char *const k_ligature_text[] = {"ff",  "fi", "fl", "ffi",
                                                  "ffl", "st", "st"};
    Layout *l = &pg->lay;
    if (cp >= 0xFB00 && cp <= 0xFB06) {
        buf_puts(pg->out, str_c(k_ligature_text[cp - 0xFB00]));
        l->space = l->line_empty = false;
        l->newlines = 0;
        return;
    }
    if (cp == '\t' || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A)
        || cp == 0x3000)
        cp = ' ';
    if (cp == 0xAD) cp = '-';
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)
        || (cp >= 0x200B && cp <= 0x200D) || cp == 0xFEFF)
        return;
    cp = symbol_piece(cp);
    if ((cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0xE000 && cp <= 0xF8FF)
        || cp > 0x10FFFF) {
        pg->unmapped++;
        cp = 0xFFFD;
    }
    if (cp == ' ') {
        if (l->space || l->line_empty) return;
        buf_putc(pg->out, ' ');
        l->space = true;
        return;
    }
    char s[4];
    size_t len = pdf_utf8_put(s, sizeof s, cp);
    buf_put(pg->out, s, len);
    l->space = l->line_empty = false;
    l->newlines = 0;
}

static void layout_glyph(Page *pg, const u32 *cps, i32 n, f64 u0, f64 v, f64 u1,
                         f64 size, i32 dir) {
    Layout *l = &pg->lay;
    if (size < 0.01) size = 0.01;
    if (n == 0) {
        if (l->any && dir == l->dir && pdf_abs(v - l->line_v) < l->line_size)
            l->end_u = u1 > l->end_u ? u1 : l->end_u;
        return;
    }
    if (!l->any) {
        l->any = true;
        l->dir = dir;
        l->line_v = v;
        l->line_size = size;
    } else if (dir != l->dir) {
        out_newline(pg, true);
        l->dir = dir;
        l->line_v = v;
        l->line_size = size;
    } else {
        f64 dv = v - l->line_v;
        f64 ref = size > l->line_size ? size : l->line_size;
        b8 went_back = pdf_abs(dv) > 0.3 * ref && u0 < l->end_u - 2 * size;
        if (pdf_abs(dv) > 0.6 * ref || went_back) {
            out_newline(pg, pdf_abs(dv) > 1.8 * ref);
            l->line_v = v;
            l->line_size = size;
        } else {
            f64 gap = u0 - l->end_u;
            if (n == 1 && cps[0] == l->last_cp
                && pdf_abs(u0 - l->start_u) < 0.1 * size)
                return;
            if (n == 1 && cps[0] == 0xFFFD && l->last_cp == 0xFFFD) {
                l->end_u = u1 > l->end_u ? u1 : l->end_u;
                return;
            }
            if (n == 1 && l->accent && u0 < l->accent_end - 0.1 * size
                && l->accent_at <= pg->out->n) {
                u32 composed = accent_compose(l->accent, cps[0]);
                if (composed) {
                    pg->out->n = l->accent_at;
                    l->accent = 0;
                    out_cp(pg, composed);
                    pg->glyphs++;
                    l->start_u = u0;
                    l->end_u = u1 > l->end_u ? u1 : l->end_u;
                    l->last_cp = composed;
                    return;
                }
            }
            if (gap > 0.15 * size)
                out_cp(pg, ' ');
            else if (gap < -4 * ref)
                out_newline(pg, false);
            if (size >= l->line_size) l->line_v = v;
            l->line_size = size;
        }
    }
    size_t at = pg->out->n;
    for (i32 i = 0; i < n; i++) out_cp(pg, cps[i]);
    l->accent = n == 1 && is_accent(cps[0]) ? cps[0] : 0;
    l->accent_at = at;
    l->accent_end = u1 > u0 ? u1 : u0;
    pg->glyphs++;
    l->start_u = u0;
    l->end_u = u1 > u0 ? u1 : u0;
    l->last_cp = cps[n - 1];
}

static void show_glyph(Page *pg, PdfFont *f, u32 code, u32 len) {
    GState *g = &pg->gs;
    f64 w0 = font_width(f, code);
    Mat m = mat_mul(pg->tm, g->ctm);
    f64 x0 = g->rise * m.c + m.e, y0 = g->rise * m.d + m.f;
    f64 adv = w0 * g->size * g->th;
    f64 x1 = x0 + adv * m.a, y1 = y0 + adv * m.b;
    f64 vc = pdf_abs(m.c), vd = pdf_abs(m.d);
    f64 size = pdf_abs(g->size) * f->em * (vc > vd ? vc : vd);
    i32 dir =
        pdf_abs(m.a) >= pdf_abs(m.b) ? (m.a >= 0 ? 0 : 2) : (m.b >= 0 ? 1 : 3);
    f64 u0, v, u1;
    switch (dir) {
        case 0: u0 = x0, v = y0, u1 = x1; break;
        case 1: u0 = y0, v = -x0, u1 = y1; break;
        case 2: u0 = -x0, v = -y0, u1 = -x1; break;
        default: u0 = -y0, v = x0, u1 = -y1; break;
    }
    u32 cps[PDF_MAX_GLYPH_TEXT];
    i32 n = font_text(f, code, cps);
    if (n < 0 && !f->composite && code == ' ') {
        cps[0] = ' ';
        n = 1;
    }
    if (n < 0) {
        pg->unmapped++;
        cps[0] = 0xFFFD;
        n = 1;
    }
    layout_glyph(pg, cps, n, u0, v, u1, size, dir);
    b8 word_space = len == 1 && code == 32;
    f64 tx = (w0 * g->size + g->tc + (word_space ? g->tw : 0)) * g->th;
    pg->tm.e += tx * pg->tm.a;
    pg->tm.f += tx * pg->tm.b;
}

static void show_text(Page *pg, const Operand *s) {
    if (s->kind != TK_STR && s->kind != TK_HEX) return;
    PdfFont *f = pg->gs.font ? pg->gs.font : pg->fallback;
    if (!f) return;
    StrIter it = {s->raw.p, s->raw.n, 0, s->kind == TK_HEX};
    u32 code = 0, len = 0;
    for (i32 b; (b = str_iter_next(&it)) >= 0;) {
        if (!f->composite) {
            show_glyph(pg, f, (u32)b, 1);
            continue;
        }
        code = code << 8 | (u32)b;
        len++;
        if (code_complete(f, code, len)) {
            show_glyph(pg, f, code, len);
            code = len = 0;
        }
    }
    if (len) show_glyph(pg, f, code, len);
}

static void text_move(Page *pg, f64 tx, f64 ty) {
    Mat t = {1, 0, 0, 1, tx, ty};
    pg->tlm = mat_mul(t, pg->tlm);
    pg->tm = pg->tlm;
}

static b8 arg_nums(const Operand *ops, u32 n, u32 want, f64 *out) {
    if (n < want) return false;
    for (u32 i = 0; i < want; i++) {
        const Operand *o = &ops[n - want + i];
        if (o->kind != TK_NUM) return false;
        out[i] = o->num;
    }
    return true;
}

static Str arg_name(Page *pg, const Operand *ops, u32 n, u32 back) {
    if (n < back || ops[n - back].kind != TK_NAME) return (Str){0};
    Str name = pdf_name_decode(pg->d->page, ops[n - back].raw);
    return name.p ? name : (Str){0};
}

static void run_content(Page *pg, Str data, PdfObj *res, u32 depth);

static void run_form(Page *pg, Str name, PdfObj *res, u32 depth) {
    Doc *d = pg->d;
    if (depth >= PDF_MAX_FORM_DEPTH || !name.p) return;
    PdfObj *ref = dict_get(dget(d, res, STR("XObject")), name);
    if (!ref || ref->kind != PO_REF) return;
    u32 num = ref->u.ref.num;
    Str raw, data = {0};
    PdfObj *form = doc_obj(d, num, &raw);
    if (!form || !raw.p
        || !obj_is_name(dget(d, form, STR("Subtype")), STR("Form")))
        return;
    u32 slot = 0;
    while (slot < pg->forms && pg->form_num[slot] != num) slot++;
    if (slot < pg->forms) {
        data = pg->form_data[slot];
    } else {
        if (!stream_decode(d, form, raw, num, true, d->page, &data)) return;
        if (pg->forms < PDF_FORM_SLOTS) {
            pg->form_num[pg->forms] = num;
            pg->form_data[pg->forms++] = data;
        }
    }
    PdfObj *fres = dget(d, form, STR("Resources"));
    if (!fres || fres->kind != PO_DICT) fres = res;
    Mat m = k_identity;
    PdfObj *mx = dget(d, form, STR("Matrix"));
    if (mx && mx->kind == PO_ARRAY && mx->n == 6) {
        f64 v[6];
        b8 ok = true;
        for (u32 i = 0; i < 6; i++) ok = ok && obj_num(&mx->u.items[i], &v[i]);
        if (ok) m = (Mat){v[0], v[1], v[2], v[3], v[4], v[5]};
    }
    GState saved = pg->gs;
    Mat tm = pg->tm, tlm = pg->tlm;
    u32 sp = pg->sp, lost = pg->lost;
    pg->gs.ctm = mat_mul(m, pg->gs.ctm);
    run_content(pg, data, fres, depth + 1);
    pg->gs = saved;
    pg->tm = tm;
    pg->tlm = tlm;
    pg->sp = sp;
    pg->lost = lost;
}

static void skip_inline_image(Lex *lx) {
    for (;;) {
        Tok t = lex_next(lx);
        if (t.kind == TK_EOF) return;
        if (tok_word(&t, STR("ID"))) break;
    }
    for (size_t i = lx->i + 1; i + 2 <= lx->n; i++) {
        if (lx->p[i] == 'E' && lx->p[i + 1] == 'I'
            && pdf_space((u8)lx->p[i - 1])
            && (i + 2 == lx->n || pdf_space((u8)lx->p[i + 2]))) {
            lx->i = i + 2;
            return;
        }
    }
    lx->i = lx->n;
}

static void run_op(Page *pg, Str op, const Operand *ops, u32 n, PdfObj *res,
                   u32 depth) {
    GState *g = &pg->gs;
    f64 v[6];
    if (op.n == 1) {
        switch (op.p[0]) {
            case 'q':
                if (pg->sp < PDF_MAX_GSTATE)
                    pg->stack[pg->sp++] = *g;
                else
                    pg->lost++;
                return;
            case 'Q':
                if (pg->lost)
                    pg->lost--;
                else if (pg->sp)
                    *g = pg->stack[--pg->sp];
                return;
            case '\'':
                text_move(pg, 0, -g->tl);
                if (n) show_text(pg, &ops[n - 1]);
                return;
            case '"':
                if (n >= 3 && arg_nums(ops, n - 1, 2, v)) {
                    g->tw = v[0];
                    g->tc = v[1];
                }
                text_move(pg, 0, -g->tl);
                if (n) show_text(pg, &ops[n - 1]);
                return;
            default: return;
        }
    }
    if (op.n != 2) return;
    char a = op.p[0], b = op.p[1];
    if (a == 'c' && b == 'm' && arg_nums(ops, n, 6, v)) {
        g->ctm = mat_mul((Mat){v[0], v[1], v[2], v[3], v[4], v[5]}, g->ctm);
    } else if (a == 'B' && b == 'T') {
        pg->tm = pg->tlm = k_identity;
    } else if (a == 'D' && b == 'o') {
        run_form(pg, arg_name(pg, ops, n, 1), res, depth);
    } else if (a != 'T') {
        return;
    } else if (b == 'f' && arg_nums(ops, n, 1, v)) {
        Str name = arg_name(pg, ops, n, 2);
        PdfObj *fd =
            pdf_resolve(pg->d, dict_get(dget(pg->d, res, STR("Font")), name));
        g->font = fd && fd->kind == PO_DICT ? font_get(pg->d, fd) : NULL;
        g->size = v[0];
    } else if (b == 'c' && arg_nums(ops, n, 1, v)) {
        g->tc = v[0];
    } else if (b == 'w' && arg_nums(ops, n, 1, v)) {
        g->tw = v[0];
    } else if (b == 'z' && arg_nums(ops, n, 1, v)) {
        g->th = v[0] / 100;
    } else if (b == 'L' && arg_nums(ops, n, 1, v)) {
        g->tl = v[0];
    } else if (b == 's' && arg_nums(ops, n, 1, v)) {
        g->rise = v[0];
    } else if (b == 'd' && arg_nums(ops, n, 2, v)) {
        text_move(pg, v[0], v[1]);
    } else if (b == 'D' && arg_nums(ops, n, 2, v)) {
        g->tl = -v[1];
        text_move(pg, v[0], v[1]);
    } else if (b == 'm' && arg_nums(ops, n, 6, v)) {
        pg->tm = pg->tlm = (Mat){v[0], v[1], v[2], v[3], v[4], v[5]};
    } else if (b == '*') {
        text_move(pg, 0, -g->tl);
    } else if (b == 'j' && n) {
        show_text(pg, &ops[n - 1]);
    } else if (b == 'J') {
        u32 open = n;
        while (open > 0 && ops[open - 1].kind != TK_ARRAY_OPEN) open--;
        if (!open) return;
        for (u32 i = open; i < n && ops[i].kind != TK_ARRAY_CLOSE; i++) {
            if (ops[i].kind == TK_NUM) {
                f64 tx = -ops[i].num / 1000 * g->size * g->th;
                pg->tm.e += tx * pg->tm.a;
                pg->tm.f += tx * pg->tm.b;
            } else {
                show_text(pg, &ops[i]);
            }
        }
    }
}

static void run_content(Page *pg, Str data, PdfObj *res, u32 depth) {
    if (!pg->ops[depth])
        pg->ops[depth] = arena_new(pg->d->page, Operand, PDF_MAX_OPERANDS);
    Operand *ops = pg->ops[depth];
    if (!ops) {
        pg->over_budget = true;
        return;
    }
    Lex lx = {data.p, data.n, 0};
    u32 n = 0;
    for (;;) {
        Tok t = lex_next(&lx);
        if (t.kind == TK_EOF) break;
        if (t.kind == TK_WORD && !tok_word(&t, STR("true"))
            && !tok_word(&t, STR("false")) && !tok_word(&t, STR("null"))) {
            if (++pg->op_count > PDF_MAX_PAGE_OPS) {
                pg->over_budget = true;
                return;
            }
            if (tok_word(&t, STR("BI")))
                skip_inline_image(&lx);
            else
                run_op(pg, t.raw, ops, n, res, depth);
            n = 0;
            continue;
        }
        if (n < PDF_MAX_OPERANDS) ops[n++] = (Operand){t.kind, t.num, t.raw};
    }
}

/* ---- pages ------------------------------------------------------------------ */

typedef struct {
    PdfObj **page;
    PdfObj **res;
    u32 n, cap;
    u8 *seen;
    u32 seen_n;
} Pages;

static void pages_push(Doc *d, Pages *pg, PdfObj *page, PdfObj *res) {
    if (pg->n == pg->cap) {
        u32 cap = pg->cap ? pg->cap * 2 : 64;
        PdfObj **p = arena_new(d->a, PdfObj *, cap);
        PdfObj **r = arena_new(d->a, PdfObj *, cap);
        if (!p || !r) return;
        if (pg->n) {
            memcpy(p, pg->page, pg->n * sizeof *p);
            memcpy(r, pg->res, pg->n * sizeof *r);
        }
        pg->page = p;
        pg->res = r;
        pg->cap = cap;
    }
    pg->page[pg->n] = page;
    pg->res[pg->n++] = res;
}

static void pages_walk(Doc *d, Pages *pg, PdfObj *ref, PdfObj *res, u32 depth) {
    if (depth > PDF_MAX_TREE_DEPTH || pg->n >= PDF_MAX_PAGES) return;
    if (ref && ref->kind == PO_REF && ref->u.ref.num < pg->seen_n) {
        if (pg->seen[ref->u.ref.num]) return;
        pg->seen[ref->u.ref.num] = 1;
    }
    PdfObj *node = pdf_resolve(d, ref);
    if (!node || node->kind != PO_DICT) return;
    PdfObj *r = dget(d, node, STR("Resources"));
    if (r && r->kind == PO_DICT) res = r;
    PdfObj *kids = dget(d, node, STR("Kids"));
    if (kids && kids->kind == PO_ARRAY
        && !obj_is_name(dget(d, node, STR("Type")), STR("Page"))) {
        for (u32 i = 0; i < kids->n; i++)
            pages_walk(d, pg, &kids->u.items[i], res, depth + 1);
        return;
    }
    pages_push(d, pg, node, res);
}

static void pages_scan(Doc *d, Pages *pg) {
    for (u32 num = 1; num < d->x.n && pg->n < PDF_MAX_PAGES; num++) {
        PdfObj *o = doc_obj(d, num, NULL);
        if (!o || !obj_is_name(dget(d, o, STR("Type")), STR("Page"))) continue;
        PdfObj *res = NULL, *node = o;
        for (u32 up = 0; up < PDF_MAX_TREE_DEPTH && node && !res; up++) {
            res = dget(d, node, STR("Resources"));
            if (res && res->kind != PO_DICT) res = NULL;
            node = dget(d, node, STR("Parent"));
        }
        pages_push(d, pg, o, res);
    }
}

static b8 page_content(Doc *d, PdfObj *page, Str *out) {
    PdfObj *c = dict_get(page, STR("Contents"));
    PdfObj *list = pdf_resolve(d, c);
    if (!c || !list) {
        *out = (Str){"", 0};
        return true;
    }
    if (list->kind != PO_ARRAY) {
        if (c->kind != PO_REF) return false;
        return stream_of(d, c, d->page, out);
    }
    Str parts[256];
    u32 count = 0;
    size_t total = 0;
    for (u32 i = 0; i < list->n && count < 256; i++) {
        Str s;
        if (!stream_of(d, &list->u.items[i], d->page, &s)) continue;
        parts[count++] = s;
        total += s.n + 1;
    }
    char *p = arena_alloc(d->page, total + 1, 1);
    if (!p) return false;
    size_t at = 0;
    for (u32 i = 0; i < count; i++) {
        memcpy(p + at, parts[i].p, parts[i].n);
        at += parts[i].n;
        p[at++] = '\n';
    }
    *out = (Str){p, at};
    return true;
}

/* ---- entry points ------------------------------------------------------------ */

b8 pdf_sniff(Str bytes) {
    Str head = {bytes.p, bytes.n < PDF_HEAD_WINDOW ? bytes.n : PDF_HEAD_WINDOW};
    return pdf_find(head, 0, STR("%PDF-")) != PDF_NOT_FOUND;
}

b8 pdf_text(Str bytes, Arena *a, Buf *out, PdfStats *stats, char *err,
            size_t err_cap) {
    *stats = (PdfStats){0};
    size_t room = a->cap - a->off;
    size_t page_cap =
        room / 3 < PDF_PAGE_ARENA_MAX ? room / 3 : PDF_PAGE_ARENA_MAX;
    void *page_mem = arena_alloc(a, page_cap, 16);
    Doc *d = arena_new(a, Doc, 1);
    Arena page;
    if (!page_mem || !d) {
        snprintf(err, err_cap, "not enough memory to read it");
        return false;
    }
    arena_init(&page, page_mem, page_cap);
    memset(d, 0, sizeof *d);
    d->file = bytes;
    d->a = a;
    d->page = &page;
    if (!doc_open(d, err, err_cap)) return false;
    PdfFont *fallback = arena_new(a, PdfFont, 1);
    Pages pages = {0};
    pages.seen_n = d->x.n;
    pages.seen = arena_new(a, u8, pages.seen_n ? pages.seen_n : 1);
    if (!fallback || !pages.seen) {
        snprintf(err, err_cap, "not enough memory to read it");
        return false;
    }
    memset(fallback, 0, sizeof *fallback);
    fallback->scale = 0.001;
    fallback->em = 1;
    for (u32 c = 0; c < 256; c++) {
        fallback->enc[c] = k_enc_standard[c];
        fallback->width[c] = 0.5;
    }
    memset(pages.seen, 0, pages.seen_n);
    PdfObj *root = pdf_resolve(d, d->root);
    pages_walk(d, &pages, dict_get(root, STR("Pages")), NULL, 0);
    if (!pages.n) pages_scan(d, &pages);
    if (!pages.n) {
        snprintf(err, err_cap, "it has no pages");
        return false;
    }
    stats->pages = pages.n;
    Page *pg = arena_new(a, Page, 1);
    if (!pg) {
        snprintf(err, err_cap, "not enough memory to read it");
        return false;
    }
    for (u32 i = 0; i < pages.n; i++) {
        arena_reset(&page);
        *pg = (Page){.d = d, .out = out, .fallback = fallback};
        pg->lay = (Layout){.line_empty = true, .newlines = 2};
        pg->gs = (GState){.ctm = k_identity, .th = 1};
        pg->tm = pg->tlm = k_identity;
        if (i) buf_putc(out, '\n');
        buf_putf(out, "--- page %u of %u ---\n", i + 1, pages.n);
        Str content;
        PdfObj *res = pages.res[i];
        if (!page_content(d, pages.page[i], &content)) {
            buf_puts(out, STR("[this page could not be read]\n"));
            stats->pages_failed++;
            continue;
        }
        run_content(pg, content, res, 0);
        out_newline(pg, false);
        if (pg->over_budget) {
            buf_puts(out,
                     STR("[the rest of this page was too complex to read]\n"));
            stats->pages_failed++;
        }
        if (pg->glyphs)
            stats->pages_with_text++;
        else
            buf_puts(out, STR("[no text on this page]\n"));
        stats->unmapped += pg->unmapped;
    }
    if (!buf_ok(out)) {
        snprintf(err, err_cap, "its text does not fit in memory");
        return false;
    }
    return true;
}
