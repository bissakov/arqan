/* core.c — arena, strings, buffers, logging, time. */
#include "ah.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ---- arena -------------------------------------------------------------- */
void arena_init(Arena *a, void *mem, size_t cap) {
    a->base = (u8 *)mem;
    a->cap  = cap;
    a->off  = 0;
}

static size_t align_up(size_t n, size_t align) {
    return (n + (align - 1)) & ~(align - 1);
}

void *arena_alloc(Arena *a, size_t n, size_t align) {
    size_t p = align_up(a->off, align);
    size_t end = p + n;
    if (end > a->cap) {
        ah_log(AH_LOG_ERROR, "arena OOM: need %zu, have %zu", end, a->cap);
        return NULL;
    }
    a->off = end;
    return a->base + p;
}

void arena_reset(Arena *a) { a->off = 0; }
size_t arena_used(const Arena *a) { return a->off; }

/* ---- strings ------------------------------------------------------------ */
Str str_c(const char *z) { return (Str){ z, strlen(z) }; }
b8 str_eq(Str a, Str b) { return a.n == b.n && !memcmp(a.p, b.p, a.n); }

Str str_dup(Arena *a, Str s) {
    char *dst = (char *)arena_alloc(a, s.n + 1, 1);
    if (!dst) return (Str){0};
    memcpy(dst, s.p, s.n);
    dst[s.n] = '\0';
    return (Str){ dst, s.n };
}

Str str_trim(Str s) {
    while (s.n && (s.p[0]==' '||s.p[0]=='\t'||s.p[0]=='\n'||s.p[0]=='\r')) { s.p++; s.n--; }
    while (s.n && (s.p[s.n-1]==' '||s.p[s.n-1]=='\t'||s.p[s.n-1]=='\n'||s.p[s.n-1]=='\r')) s.n--;
    return s;
}
Str str_take(Str s, size_t n) { return (Str){ s.p, n < s.n ? n : s.n }; }
Str str_drop(Str s, size_t n) { return n >= s.n ? (Str){0} : (Str){ s.p+n, s.n-n }; }

i64 str_int(Str s, b8 *ok) {
    i64 v = 0; b8 neg = false; size_t i = 0;
    if (s.n && s.p[0]=='-') { neg = true; i = 1; }
    i64 d = 0;
    for (; i < s.n; i++) { if (s.p[i]<'0'||s.p[i]>'9') { if(ok)*ok=false; return 0; } d = d*10 + (s.p[i]-'0'); }
    v = neg ? -d : d; if (ok) *ok = true; return v;
}

/* ---- buffer ------------------------------------------------------------- */
void buf_init(Buf *b, Arena *a, size_t cap) {
    b->a = a; b->n = 0; b->cap = cap;
    b->p = (char *)arena_alloc(a, cap, 1);
}
void buf_grow(Buf *b, size_t need) {
    if (need <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) nc *= 2;
    char *np = (char *)arena_alloc(b->a, nc, 1);
    if (!np) { ah_log(AH_LOG_ERROR, "buf OOM"); return; }
    memcpy(np, b->p, b->n);
    b->p = np; b->cap = nc;
}
void buf_put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) buf_grow(b, b->n + n);
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
void buf_putc(Buf *b, char c) {
    if (b->n + 1 > b->cap) buf_grow(b, b->n + 1);
    b->p[b->n++] = c;
}
void buf_puts(Buf *b, Str s) { buf_put(b, s.p, s.n); }
void buf_putf(Buf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    size_t avail = b->cap - b->n;
    i32 w = vsnprintf(b->p + b->n, avail, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w >= avail) {
        buf_grow(b, b->n + (size_t)w + 1);
        avail = b->cap - b->n;
        va_start(ap, fmt);
        vsnprintf(b->p + b->n, avail, fmt, ap);
        va_end(ap);
    }
    b->n += (size_t)w;
}
void buf_json_str(Buf *b, Str s) {
    buf_putc(b, '"');
    for (size_t i = 0; i < s.n; i++) {
        u8 c = (u8)s.p[i];
        switch (c) {
            case '"':  buf_put(b, "\\\"", 2); break;
            case '\\': buf_put(b, "\\\\", 2); break;
            case '\b': buf_put(b, "\\b", 2); break;
            case '\f': buf_put(b, "\\f", 2); break;
            case '\n': buf_put(b, "\\n", 2); break;
            case '\r': buf_put(b, "\\r", 2); break;
            case '\t': buf_put(b, "\\t", 2); break;
            default:
                if (c < 0x20) buf_putf(b, "\\u%04x", c);
                else buf_putc(b, (char)c);
        }
    }
    buf_putc(b, '"');
}
Str buf_finish(Buf *b) {
    if (b->n + 1 > b->cap) buf_grow(b, b->n + 1);
    b->p[b->n] = '\0';
    return (Str){ b->p, b->n };
}

/* ---- logging ------------------------------------------------------------ */
static i32 g_level = AH_LOG_INFO;
void ah_log_set_level(i32 level) { g_level = level; }
void ah_log(i32 level, const char *fmt, ...) {
    if (level < g_level) return;
    static const char *tags[] = {"DBG","INF","WRN","ERR"};
    fprintf(stderr, "[ah %s] ", tags[level]);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- time --------------------------------------------------------------- */
f64 ah_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}
