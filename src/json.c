/* json.c — tiny arena JSON parser + serializer.
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

static Str unescape(JParser *p, Str raw) {
    /* copy into arena, unescaping \uXXXX, \\, etc. */
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
                u32 cp = 0;
                for (i32 k = 0; k < 4; k++) {
                    char h = raw.p[i+1+k]; u32 d;
                    if (h>='0'&&h<='9') d = h-'0';
                    else if (h>='a'&&h<='f') d = h-'a'+10;
                    else if (h>='A'&&h<='F') d = h-'A'+10;
                    else goto bad;
                    cp = cp*16 + d;
                }
                i += 4;
                /* encode as UTF-8 (BMP, no surrogate handling) */
                if (cp < 0x80) dst[w++] = (char)cp;
                else if (cp < 0x800) { dst[w++] = (char)(0xC0|(cp>>6)); dst[w++] = (char)(0x80|(cp&0x3F)); }
                else { dst[w++] = (char)(0xE0|(cp>>12)); dst[w++] = (char)(0x80|((cp>>6)&0x3F)); dst[w++] = (char)(0x80|(cp&0x3F)); }
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
    if (peekc(p) == '-') p->pos++;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if ((c>='0'&&c<='9')||c=='.'||c=='e'||c=='E'||c=='+'||c=='-') p->pos++;
        else break;
    }
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
    JVal *first = NULL, *prev = NULL;
    skipws(p);
    size_t count = 0;
    /* count first to allocate contiguous */
    /* We'll build a linked list then compact. */
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
    if (count == 0) {
        JVal *v = jnew(p, J_ARR); v->u.arr.items = NULL; v->u.arr.n = 0; return v;
    }
    JVal *arr = jnew(p, J_ARR);
    arr->u.arr.items = arena_new(p->a, JVal, count);
    arr->u.arr.n = count;
    JVal *cur = list_head.next;
    for (size_t i = 0; i < count; i++) {
        arr->u.arr.items[i] = *cur;
        cur = cur->next;
    }
    (void)first; (void)prev;
    return arr;
}

static JVal *parse_object(JParser *p) {
    getc_(p); /* { */
    JVal *obj = jnew(p, J_OBJ);
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
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (p->pos + 4 <= p->len && !memcmp(p->src+p->pos, "true", 4)) { p->pos+=4; JVal *v=jnew(p,J_BOOL); v->u.b=true; return v; }
    if (p->pos + 5 <= p->len && !memcmp(p->src+p->pos, "false", 5)) { p->pos+=5; JVal *v=jnew(p,J_BOOL); v->u.b=false; return v; }
    if (p->pos + 4 <= p->len && !memcmp(p->src+p->pos, "null", 4))  { p->pos+=4; return jnew(p,J_NULL); }
    return NULL;
}

JVal *json_parse(Arena *a, Str s) {
    JParser p = { a, s.p, 0, s.n, false };
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
