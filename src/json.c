/* json.c: tiny arena JSON parser + serializer.
 *
 * DOM lives entirely in the scratch arena. Objects are a singly linked list of
 * members (ordered). Arrays are contiguous JVal arrays.
 */
#include "yoke.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static JVal *jnew(JParser *p, JType t) {
    JVal *v = arena_new(p->a, JVal, 1);
    if (!v) { p->oom = true; return NULL; }
    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}
static void skipws(JParser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c==' '||c=='\t'||c=='\n'||c=='\r') p->pos++;
        else break;
    }
}
static i32 peekc(JParser *p) { return p->pos < p->len ? (u8)p->src[p->pos] : -1; }
static i32 getc_(JParser *p) { return p->pos < p->len ? (u8)p->src[p->pos++] : -1; }

/* One \uXXXX escape is six source bytes and at most four UTF-8 bytes, and a
 * surrogate pair is twelve source bytes for four, so raw.n + 1 is always
 * enough room for the decoded form. */
static u32 hex4(const char *p) {
    u32 cp = 0;
    for (i32 k = 0; k < 4; k++) {
        char h = p[k];
        u32 d;
        if (h >= '0' && h <= '9') d = (u32)(h - '0');
        else if (h >= 'a' && h <= 'f') d = (u32)(h - 'a') + 10u;
        else if (h >= 'A' && h <= 'F') d = (u32)(h - 'A') + 10u;
        else return 0xFFFFFFFFu;
        cp = cp * 16u + d;
    }
    return cp;
}

static size_t utf8_put(char *dst, u32 cp) {
    if (cp < 0x80) { dst[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0u | (cp >> 6)); dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    dst[0] = (char)(0xF0u | (cp >> 18));
    dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Copies into the arena, unescaping \uXXXX, \\ and friends. */
static Str unescape(JParser *p, Str raw) {
    char *dst = arena_new(p->a, char, raw.n + 1);
    if (!dst) { p->oom = true; return (Str){0}; }
    size_t w = 0;
    for (size_t i = 0; i < raw.n; i++) {
        if (raw.p[i] != '\\') { dst[w++] = raw.p[i]; continue; }
        i++; if (i >= raw.n) break;
        char e = raw.p[i];
        switch (e) {
            case '"':  dst[w++] = '"'; break;
            case '\\': dst[w++] = '\\'; break;
            case '/':  dst[w++] = '/'; break;
            case 'b':  dst[w++] = '\b'; break;
            case 'f':  dst[w++] = '\f'; break;
            case 'n':  dst[w++] = '\n'; break;
            case 'r':  dst[w++] = '\r'; break;
            case 't':  dst[w++] = '\t'; break;
            case 'u': {
                if (i + 4 >= raw.n) break;
                u32 cp = hex4(raw.p + i + 1);
                if (cp == 0xFFFFFFFFu) goto bad;
                i += 4;
                /* A high surrogate is only half a code point: pair it with the
                 * low one that follows, or the model's emoji come out as
                 * invalid UTF-8 the renderer then has to guess at. */
                if (cp >= 0xD800u && cp <= 0xDBFFu
                    && i + 6 < raw.n && raw.p[i+1] == '\\' && raw.p[i+2] == 'u') {
                    u32 lo = hex4(raw.p + i + 3);
                    if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        i += 6;
                    }
                }
                if (cp >= 0xD800u && cp <= 0xDFFFu) cp = 0xFFFDu;  /* lone half */
                w += utf8_put(dst + w, cp);
                continue;
            }
            default: dst[w++] = e; break;
        }
        continue;
    bad: dst[w++] = e;
    }
    return (Str){ dst, w };
}

static JVal *parse_value(JParser *p);

static Str parse_string_raw(JParser *p) {
    if (getc_(p) != '"') return (Str){0};
    size_t start = p->pos;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == '\\') { p->pos += 2; continue; }
        if (c == '"') {
            Str raw = { p->src + start, p->pos - start };
            p->pos++; /* closing quote */
            return raw;
        }
        p->pos++;
    }
    return (Str){0};
}

static JVal *parse_string(JParser *p) {
    Str raw = parse_string_raw(p);
    if (!raw.p) return NULL;
    JVal *v = jnew(p, J_STR);
    if (!v) return NULL;
    v->u.s = unescape(p, raw);
    return v;
}

static JVal *parse_number(JParser *p) {
    size_t start = p->pos;
    b8 digits = false;
    if (peekc(p) == '-') p->pos++;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c>='0'&&c<='9') digits = true;
        else if (!(c=='.'||c=='e'||c=='E'||c=='+'||c=='-')) break;
        p->pos++;
    }
    if (!digits) return NULL;
    Str raw = { p->src + start, p->pos - start };
    char tmp[64];
    size_t l = raw.n < 63 ? raw.n : 63;
    memcpy(tmp, raw.p, l); tmp[l] = '\0';
    JVal *v = jnew(p, J_NUM);
    if (!v) return NULL;
    v->u.n = strtod(tmp, NULL);
    return v;
}

static JVal *parse_array(JParser *p) {
    getc_(p); /* [ */
    skipws(p);
    size_t count = 0;
    /* Built as a linked list, then compacted into one contiguous block. */
    JVal list_head; list_head.next = NULL; JVal *tail = &list_head;
    while (peekc(p) != ']') {
        JVal *v = parse_value(p);
        if (!v) return NULL;
        tail->next = v; tail = v; count++;
        skipws(p);
        if (peekc(p) == ',') { p->pos++; skipws(p); }
        else break;
    }
    getc_(p); /* ] */
    JVal *arr = jnew(p, J_ARR);
    if (!arr) return NULL;
    arr->u.arr.items = NULL;
    arr->u.arr.n = 0;
    if (count == 0) return arr;
    JVal *items = arena_new(p->a, JVal, count);
    if (!items) { p->oom = true; return NULL; }
    JVal *cur = list_head.next;
    for (size_t i = 0; i < count; i++) {
        items[i] = *cur;
        items[i].next = NULL;   /* the sibling link belongs to objects only */
        cur = cur->next;
    }
    arr->u.arr.items = items;
    arr->u.arr.n = count;
    return arr;
}

static JVal *parse_object(JParser *p) {
    getc_(p); /* { */
    JVal *obj = jnew(p, J_OBJ);
    if (!obj) return NULL;
    obj->u.obj.head = NULL;
    JVal *prev = NULL;
    skipws(p);
    while (peekc(p) != '}') {
        Str kraw = parse_string_raw(p);
        if (!kraw.p) return NULL;
        Str key = unescape(p, kraw);
        skipws(p);
        if (getc_(p) != ':') return NULL;
        skipws(p);
        JVal *v = parse_value(p);
        if (!v) return NULL;
        v->key = key;
        if (prev) prev->next = v; else obj->u.obj.head = v;
        prev = v;
        skipws(p);
        if (peekc(p) == ',') { p->pos++; skipws(p); }
        else break;
    }
    getc_(p); /* } */
    return obj;
}

static JVal *parse_value(JParser *p) {
    skipws(p);
    i32 c = peekc(p);
    if (c == '"') return parse_string(p);
    if (c == '{' || c == '[') {
        /* Nesting is recursion, and the depth comes from whatever the provider
         * sends: without this cap a stream of "[[[[[..." is a stack overflow. */
        if (p->depth >= YOKE_MAX_JSON_DEPTH) return NULL;
        p->depth++;
        JVal *v = c == '{' ? parse_object(p) : parse_array(p);
        p->depth--;
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (p->pos + 4 <= p->len && !memcmp(p->src+p->pos, "true", 4)) { p->pos+=4; JVal *v=jnew(p,J_BOOL); if (v) v->u.b=true; return v; }
    if (p->pos + 5 <= p->len && !memcmp(p->src+p->pos, "false", 5)) { p->pos+=5; JVal *v=jnew(p,J_BOOL); if (v) v->u.b=false; return v; }
    if (p->pos + 4 <= p->len && !memcmp(p->src+p->pos, "null", 4))  { p->pos+=4; return jnew(p,J_NULL); }
    return NULL;
}

JVal *json_parse(Arena *a, Str s) {
    JParser p = { a, s.p, 0, s.n, 0, false };
    if (!s.p || s.n == 0) return NULL;
    JVal *v = parse_value(&p);
    if (p.oom) return NULL;
    return v;
}

void json_write(Buf *b, const JVal *v) {
    if (!v) { buf_puts(b, STR("null")); return; }
    switch (v->type) {
        case J_NULL: buf_puts(b, STR("null")); break;
        case J_BOOL: buf_puts(b, v->u.b ? STR("true") : STR("false")); break;
        case J_NUM:  buf_putf(b, "%g", v->u.n); break;
        case J_STR:  buf_json_str(b, v->u.s); break;
        case J_ARR:
            buf_putc(b, '[');
            for (size_t i = 0; i < v->u.arr.n; i++) {
                if (i) buf_putc(b, ',');
                json_write(b, &v->u.arr.items[i]);
            }
            buf_putc(b, ']');
            break;
        case J_OBJ:
            buf_putc(b, '{');
            for (JVal *m = v->u.obj.head; m; m = m->next) {
                if (m != v->u.obj.head) buf_putc(b, ',');
                buf_json_str(b, m->key);
                buf_putc(b, ':');
                json_write(b, m);
            }
            buf_putc(b, '}');
            break;
    }
}

const JVal *json_get(const JVal *obj, Str key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (JVal *m = obj->u.obj.head; m; m = m->next)
        if (str_eq(m->key, key)) return m;
    return NULL;
}
const JVal *json_at(const JVal *arr, size_t i) {
    if (!arr || arr->type != J_ARR || i >= arr->u.arr.n) return NULL;
    return &arr->u.arr.items[i];
}
