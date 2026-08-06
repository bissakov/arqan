/* core.c: arena, strings, buffers, logging, time. */
#include "yoke.h"

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

/* Every size below can come from a file or from the provider, so each step is
 * checked against the arena's own capacity instead of being computed first and
 * compared afterwards: `off + n` is exactly the addition that wraps. */
void *arena_alloc(Arena *a, size_t n, size_t align) {
    if (align == 0 || (align & (align - 1))) return NULL;   /* power of two */
    size_t mask = align - 1;
    size_t pad = (align - (a->off & mask)) & mask;
    if (pad > a->cap - a->off) {
        yoke_log(YOKE_LOG_ERROR, "arena OOM: need %zu, have %zu", n, a->cap - a->off);
        return NULL;
    }
    size_t p = a->off + pad;
    if (n > a->cap - p) {
        yoke_log(YOKE_LOG_ERROR, "arena OOM: need %zu, have %zu", n, a->cap - p);
        return NULL;
    }
    a->off = p + n;
    return a->base + p;
}

void *arena_alloc_array(Arena *a, size_t count, size_t size, size_t align) {
    if (size && count > (size_t)-1 / size) {
        yoke_log(YOKE_LOG_ERROR, "arena overflow: %zu x %zu", count, size);
        return NULL;
    }
    return arena_alloc(a, count * size, align);
}

void arena_reset(Arena *a) { a->off = 0; }
size_t arena_used(const Arena *a) { return a->off; }

/* ---- strings ------------------------------------------------------------ */
Str str_c(const char *z) { return (Str){ z, strlen(z) }; }
/* An empty Str legitimately carries a NULL pointer (missing JSON field, failed
 * dup), and memcmp(NULL, NULL, 0) is undefined however harmless it looks. */
b8 str_eq(Str a, Str b) { return a.n == b.n && (a.n == 0 || !memcmp(a.p, b.p, a.n)); }

Str str_dup(Arena *a, Str s) {
    char *dst = (char *)arena_alloc(a, s.n + 1, 1);
    if (!dst) return (Str){0};
    memcpy(dst, s.p, s.n);
    dst[s.n] = '\0';
    return (Str){ dst, s.n };
}

b8 str_starts(Str s, Str prefix) {
    return s.n >= prefix.n && (prefix.n == 0 || !memcmp(s.p, prefix.p, prefix.n));
}

Str str_trim(Str s) {
    while (s.n && (s.p[0]==' '||s.p[0]=='\t'||s.p[0]=='\n'||s.p[0]=='\r')) { s.p++; s.n--; }
    while (s.n && (s.p[s.n-1]==' '||s.p[s.n-1]=='\t'||s.p[s.n-1]=='\n'||s.p[s.n-1]=='\r')) s.n--;
    return s;
}
Str str_take(Str s, size_t n) { return (Str){ s.p, n < s.n ? n : s.n }; }
Str str_drop(Str s, size_t n) { return n >= s.n ? (Str){0} : (Str){ s.p+n, s.n-n }; }

i64 str_int(Str s, b8 *ok) {
    b8 neg = false; size_t i = 0;
    if (s.n && s.p[0]=='-') { neg = true; i = 1; }
    if (i == s.n) { if (ok) *ok = false; return 0; }
    i64 d = 0;
    for (; i < s.n; i++) {
        if (s.p[i]<'0'||s.p[i]>'9') { if(ok)*ok=false; return 0; }
        i64 digit = s.p[i] - '0';
        /* Signed overflow is undefined, so refuse rather than wrap. */
        if (d > (INT64_MAX - digit) / 10) { if (ok) *ok = false; return 0; }
        d = d*10 + digit;
    }
    if (ok) *ok = true;
    return neg ? -d : d;
}

/* ---- buffer ------------------------------------------------------------- */
void buf_init(Buf *b, Arena *a, size_t cap) {
    b->a = a; b->n = 0; b->oom = false;
    b->p = (char *)arena_alloc(a, cap, 1);
    /* A failed reserve is an empty, latched-full buffer, never a live one
     * pointing at NULL with room to spare. */
    b->cap = b->p ? cap : 0;
    if (!b->p) b->oom = true;
}
b8 buf_ok(const Buf *b) { return !b->oom; }

/* Returns whether `need` bytes are now writable. Failure latches: a buffer
 * that could not grow stays short, and every later write is dropped instead
 * of running past the allocation. */
static b8 buf_grow(Buf *b, size_t need) {
    if (need <= b->cap) return true;
    if (b->oom) return false;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) {
        if (nc > (size_t)-1 / 2) { nc = need; break; }
        nc *= 2;
    }
    char *np = (char *)arena_alloc(b->a, nc, 1);
    if (!np) { b->oom = true; yoke_log(YOKE_LOG_ERROR, "buf OOM"); return false; }
    if (b->n) memcpy(np, b->p, b->n);
    b->p = np; b->cap = nc;
    return true;
}
void buf_put(Buf *b, const void *p, size_t n) {
    if (n == 0 || !p) return;
    if (n > b->cap - b->n && !buf_grow(b, b->n + n)) return;
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
void buf_putc(Buf *b, char c) {
    if (b->n == b->cap && !buf_grow(b, b->n + 1)) return;
    b->p[b->n++] = c;
}
void buf_puts(Buf *b, Str s) { buf_put(b, s.p, s.n); }
void buf_putf(Buf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    size_t avail = b->cap - b->n;
    i32 w = vsnprintf(avail ? b->p + b->n : NULL, avail, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w >= avail) {
        if (!buf_grow(b, b->n + (size_t)w + 1)) return;
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
    if (b->n == b->cap && !buf_grow(b, b->n + 1)) {
        /* Nowhere to put the terminator: hand back what is safely readable. */
        if (b->n == 0) return (Str){0};
        b->n--;
    }
    b->p[b->n] = '\0';
    return (Str){ b->p, b->n };
}

/* ---- logging ------------------------------------------------------------ */
static i32 g_level = YOKE_LOG_INFO;
static YokeLogSink g_log_sink;
static void *g_log_ud;
void yoke_log_set_level(i32 level) { g_level = level; }
void yoke_log_set_sink(YokeLogSink sink, void *ud) { g_log_sink = sink; g_log_ud = ud; }
void yoke_log(i32 level, const char *fmt, ...) {
    if (level < g_level) return;
    static const char *tags[] = {"DBG","INF","WRN","ERR"};
    if (level < YOKE_LOG_DEBUG || level > YOKE_LOG_ERROR) level = YOKE_LOG_ERROR;
    char msg[512];
    va_list ap; va_start(ap, fmt);
    i32 w = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    size_t n = w > 0 ? ((size_t)w < sizeof msg ? (size_t)w : sizeof msg - 1) : 0;
    if (g_log_sink) { g_log_sink(level, (Str){ msg, n }, g_log_ud); return; }
    fprintf(stderr, "[yoke %s] %.*s\n", tags[level], (i32)n, msg);
}

/* ---- time --------------------------------------------------------------- */
f64 yoke_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}
