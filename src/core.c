#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---- arena --------------------------------------------------------------
void arena_init(Arena *a, void *mem, size_t cap) {
    a->base = (u8 *)mem;
    a->cap = cap;
    a->off = 0;
}

void *arena_alloc(Arena *a, size_t n, size_t align) {
    /* INVARIANT: alignment is applied to the offset, so a caller gets an
     * absolutely aligned pointer only when the memory handed to arena_init is
     * at least as aligned. The startup arenas use alignas(64) for this. */
    if (align == 0 || (align & (align - 1))) return NULL;
    size_t mask = align - 1;
    size_t pad = (align - (a->off & mask)) & mask;
    if (pad > a->cap - a->off) {
        agent_log(AGENT_LOG_ERROR, "arena OOM: need %zu, have %zu", n,
                  a->cap - a->off);
        return NULL;
    }
    size_t p = a->off + pad;
    if (n > a->cap - p) {
        agent_log(AGENT_LOG_ERROR, "arena OOM: need %zu, have %zu", n,
                  a->cap - p);
        return NULL;
    }
    a->off = p + n;
    return a->base + p;
}

void *arena_alloc_array(Arena *a, size_t count, size_t size, size_t align) {
    if (size && count > (size_t)-1 / size) {
        agent_log(AGENT_LOG_ERROR, "arena overflow: %zu x %zu", count, size);
        return NULL;
    }
    return arena_alloc(a, count * size, align);
}

void arena_reset(Arena *a) {
    a->off = 0;
}
size_t arena_used(const Arena *a) {
    return a->off;
}


Str str_c(const char *z) {
    return (Str){z, strlen(z)};
}

b8 str_eq(Str a, Str b) {
    return a.n == b.n && (a.n == 0 || !memcmp(a.p, b.p, a.n));
}

static char str_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

b8 str_eq_ci(Str a, Str b) {
    if (a.n != b.n) return false;
    for (size_t i = 0; i < a.n; i++)
        if (str_lower(a.p[i]) != str_lower(b.p[i])) return false;
    return true;
}

Str str_dup(Arena *a, Str s) {
    char *dst = (char *)arena_alloc(a, s.n + 1, 1);
    if (!dst) return (Str){0};
    if (s.n) memcpy(dst, s.p, s.n);
    dst[s.n] = '\0';
    return (Str){dst, s.n};
}

Str str_dup_opt(Arena *a, Str s) {
    return s.n ? str_dup(a, s) : (Str){0};
}

b8 str_starts(Str s, Str prefix) {
    return s.n >= prefix.n
           && (prefix.n == 0 || !memcmp(s.p, prefix.p, prefix.n));
}

Str str_trim(Str s) {
    while (s.n
           && (s.p[0] == ' ' || s.p[0] == '\t' || s.p[0] == '\n'
               || s.p[0] == '\r')) {
        s.p++;
        s.n--;
    }
    while (s.n
           && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t'
               || s.p[s.n - 1] == '\n' || s.p[s.n - 1] == '\r'))
        s.n--;
    return s;
}
Str str_take(Str s, size_t n) {
    return (Str){s.p, n < s.n ? n : s.n};
}

b8 str_line(Str s, size_t *off, Str *line) {
    if (*off >= s.n) return false;
    const char *p = s.p + *off;
    const char *nl = (const char *)memchr(p, '\n', s.n - *off);
    size_t n = nl ? (size_t)(nl - p) : s.n - *off;
    *line = (Str){p, n};
    *off += nl ? n + 1 : n;
    return true;
}

size_t str_lines(Str s) {
    size_t off = 0, n = 0;
    Str line;
    while (str_line(s, &off, &line)) n++;
    return n;
}

Str str_clip_utf8(Str s, size_t max) {
    if (s.n <= max) return s;
    size_t n = max;
    while (n && ((u8)s.p[n] & 0xc0) == 0x80) n--;
    return (Str){s.p, n};
}

size_t utf8_decode(const char *s, size_t n, u32 *cp) {
    if (n == 0) return 0;
    u8 c = (u8)s[0];
    size_t len;
    u32 v;
    if (c < 0x80u) {
        *cp = c;
        return 1;
    } else if ((c & 0xE0u) == 0xC0u) {
        len = 2;
        v = c & 0x1Fu;
    } else if ((c & 0xF0u) == 0xE0u) {
        len = 3;
        v = c & 0x0Fu;
    } else if ((c & 0xF8u) == 0xF0u) {
        len = 4;
        v = c & 0x07u;
    } else
        return 0;
    if (n < len) return 0;
    for (size_t i = 1; i < len; i++) {
        u8 t = (u8)s[i];
        if ((t & 0xC0u) != 0x80u) return 0;
        v = (v << 6) | (t & 0x3Fu);
    }

    static const u32 min[5] = {0, 0, 0x80u, 0x800u, 0x10000u};
    if (v < min[len]) return 0;
    if (v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu)) return 0;
    *cp = v;
    return len;
}

Str str_drop(Str s, size_t n) {
    return n >= s.n ? (Str){0} : (Str){s.p + n, s.n - n};
}

i64 str_int(Str s, b8 *ok) {
    b8 neg = false;
    size_t i = 0;
    if (s.n && s.p[0] == '-') {
        neg = true;
        i = 1;
    }
    if (i == s.n) {
        if (ok) *ok = false;
        return 0;
    }
    i64 d = 0;
    for (; i < s.n; i++) {
        if (s.p[i] < '0' || s.p[i] > '9') {
            if (ok) *ok = false;
            return 0;
        }
        i64 digit = s.p[i] - '0';
        if (d > (INT64_MAX - digit) / 10) {
            if (ok) *ok = false;
            return 0;
        }
        d = d * 10 + digit;
    }
    if (ok) *ok = true;
    return neg ? -d : d;
}

u64 str_hash64(Str s) {
    u64 h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < s.n; i++) {
        h ^= (u8)s.p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}


static const u32 k_sha256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static u32 sha256_rotr(u32 x, u32 n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(u32 h[8], const u8 *p) {
    u32 w[64];
    for (size_t i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16
               | (u32)p[4 * i + 2] << 8 | (u32)p[4 * i + 3];
    for (size_t i = 16; i < 64; i++) {
        u32 s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18)
                 ^ (w[i - 15] >> 3);
        u32 s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19)
                 ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    u32 e = h[4], f = h[5], g = h[6], k = h[7];
    for (size_t i = 0; i < 64; i++) {
        u32 s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = k + s1 + ch + k_sha256[i] + w[i];
        u32 s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

#if defined(__linux__)
int pipe2(int fds[2], int flags);
#endif
#if defined(__GLIBC__) \
    && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
int close_range(unsigned int first, unsigned int last, int flags);
#define AGENT_HAVE_CLOSE_RANGE 1
#endif

b8 pipe_cloexec(i32 fds[2]) {
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) return false;
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0
        && fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0)
        return true;
    close(fds[0]);
    close(fds[1]);
    return false;
#endif
}

void child_close_fds(i32 keep_from) {
    if (keep_from < 0) keep_from = 0;
#ifdef AGENT_HAVE_CLOSE_RANGE
    if (close_range((unsigned int)keep_from, ~0u, 0) == 0) return;
#endif
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 65536) max = 65536;
    for (i32 fd = keep_from; fd < (i32)max; fd++) close(fd);
}

void sha256(const void *p, size_t n, u8 out[SHA256_BYTES]) {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const u8 *in = (const u8 *)p;
    size_t left = n;
    while (left >= 64) {
        sha256_block(h, in);
        in += 64;
        left -= 64;
    }
    u8 tail[128] = {0};
    if (left) memcpy(tail, in, left);
    tail[left] = 0x80;
    size_t tail_n = left < 56 ? 64 : 128;
    u64 bits = (u64)n * 8;
    for (size_t i = 0; i < 8; i++) tail[tail_n - 1 - i] = (u8)(bits >> (8 * i));
    sha256_block(h, tail);
    if (tail_n == 128) sha256_block(h, tail + 64);
    for (size_t i = 0; i < 8; i++) {
        out[4 * i] = (u8)(h[i] >> 24);
        out[4 * i + 1] = (u8)(h[i] >> 16);
        out[4 * i + 2] = (u8)(h[i] >> 8);
        out[4 * i + 3] = (u8)h[i];
    }
}

void hex_encode(const u8 *p, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 0xf];
    }
    out[2 * n] = '\0';
}

FileStatus file_read(Arena *a, const char *path, size_t max, size_t head,
                     Str *out, u64 *size_out) {
    *out = (Str){0};
    if (size_out) *size_out = 0;
    if (!path || !*path) return FILE_MISSING;
    FILE *f = fopen(path, "rb");
    if (!f) return FILE_MISSING;
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return FILE_UNREADABLE;
    }
    if (!S_ISREG(st.st_mode)) {
        fclose(f);
        return FILE_NOT_REGULAR;
    }
    u64 size = (u64)st.st_size;
    if (size_out) *size_out = size;
    if (size > max) {
        fclose(f);
        return FILE_TOO_LARGE;
    }
    size_t want = (size_t)size;
    if (head && want > head) want = head;
    char *buf = arena_new(a, char, want + 1);
    if (!buf) {
        fclose(f);
        return FILE_NO_MEMORY;
    }
    size_t rd = fread(buf, 1, want, f);
    b8 failed = ferror(f) != 0;
    fclose(f);
    if (failed) return FILE_UNREADABLE;
    buf[rd] = '\0';
    *out = (Str){buf, rd};
    return FILE_OK;
}


static b8 file_sync_parent(const char *path) {
    char parent[AGENT_MAX_PATH];
    const char *slash = strrchr(path, '/');
    if (!slash) {
        memcpy(parent, ".", 2);
    } else {
        size_t n = slash == path ? 1 : (size_t)(slash - path);
        if (n >= sizeof parent) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(parent, path, n);
        parent[n] = '\0';
    }
    i32 fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    i32 rc;
    do rc = fsync(fd);
    while (rc != 0 && errno == EINTR);
    i32 saved = rc == 0 ? 0 : errno;
    if (close(fd) != 0 && !saved) saved = errno;
    if (saved) {
        errno = saved;
        return false;
    }
    return true;
}

static mode_t file_atomic_mode(const char *path, u32 create_mode, b8 *ok) {
    struct stat st;
    i32 rc = lstat(path, &st);
    if (rc == 0 && !S_ISLNK(st.st_mode)) {
        *ok = true;
        return st.st_mode & 0777;
    }
    if (rc != 0 && errno != ENOENT) {
        *ok = false;
        return 0;
    }
    mode_t mask = umask(0);
    (void)umask(mask);
    *ok = true;
    return (mode_t)create_mode & (mode_t)0777 & ~mask;
}

b8 file_write_atomic(const char *path, u32 mode, b8 sync_parent,
                     FileWriteFn write_fn, void *ud) {
    if (!path || !*path || !write_fn) {
        errno = EINVAL;
        return false;
    }
    b8 mode_ok;
    mode_t final_mode = file_atomic_mode(path, mode, &mode_ok);
    if (!mode_ok) return false;

    char tmp[AGENT_MAX_PATH];
    i32 n = snprintf(tmp, sizeof tmp, "%s." AGENT_NAME "-tmp-XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return false;
    }
    i32 fd = mkstemp(tmp);
    if (fd < 0) return false;

    i32 saved = 0;
    FILE *f = fdopen(fd, "wb");
    if (!f) saved = errno;
    if (!f) {
        if (close(fd) != 0 && !saved) saved = errno;
    } else {
        errno = 0;
        b8 wrote = write_fn(f, ud) && ferror(f) == 0;
        if (!wrote) saved = errno ? errno : EIO;
        if (!saved && fchmod(fileno(f), final_mode) != 0) saved = errno;
#ifdef AGENT_TESTING
        const char *fail = getenv(AGENT_ENV_PREFIX "TEST_ATOMIC_FAIL");
        if (!saved && fail && strcmp(fail, "flush") == 0) saved = ENOSPC;
#endif
        if (!saved && fflush(f) != 0) saved = errno;
        if (!saved) {
            i32 rc;
            do rc = fsync(fileno(f));
            while (rc != 0 && errno == EINTR);
            if (rc != 0) saved = errno;
        }
        if (fclose(f) != 0 && !saved) saved = errno;
    }
    if (saved) {
        (void)unlink(tmp);
        errno = saved;
        return false;
    }
    if (rename(tmp, path) != 0) {
        saved = errno;
        (void)unlink(tmp);
        errno = saved;
        return false;
    }
    if (sync_parent && !file_sync_parent(path)) return false;
    return true;
}

static b8 file_write_str(FILE *f, void *ud) {
    const Str *data = ud;
    return !data->n || fwrite(data->p, 1, data->n, f) == data->n;
}

b8 file_write_atomic_str(const char *path, Str data, u32 mode, b8 sync_parent) {
    return file_write_atomic(path, mode, sync_parent, file_write_str, &data);
}


void buf_init(Buf *b, Arena *a, size_t cap) {
    b->a = a;
    b->n = 0;
    b->oom = false;
    b->p = (char *)arena_alloc(a, cap, 1);

    b->cap = b->p ? cap : 0;
    if (!b->p) b->oom = true;
}
b8 buf_ok(const Buf *b) {
    return !b->oom;
}

static b8 buf_grow(Buf *b, size_t need) {
    if (need <= b->cap) return true;
    if (b->oom) return false;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) {
        if (nc > (size_t)-1 / 2) {
            nc = need;
            break;
        }
        nc *= 2;
    }
    char *np = (char *)arena_alloc(b->a, nc, 1);
    if (!np) {
        b->oom = true;
        agent_log(AGENT_LOG_ERROR, "buf OOM");
        return false;
    }
    if (b->n) memcpy(np, b->p, b->n);
    b->p = np;
    b->cap = nc;
    return true;
}
void buf_adopt(Buf *b, Arena *a, Str s) {
    b->a = a;
    b->n = s.n;
    b->cap = s.n;
    b->oom = false;

    b->p = (char *)s.p;
}
b8 buf_reserve(Buf *b, size_t need) {
    return buf_grow(b, need);
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
void buf_puts(Buf *b, Str s) {
    buf_put(b, s.p, s.n);
}
void buf_putf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
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
    buf_json_chars(b, s);
    buf_putc(b, '"');
}


static size_t utf8_seq(Str s, size_t i) {
    u8 c = (u8)s.p[i];
    size_t need;
    u32 cp;
    u32 lo;
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0) {
        need = 2;
        cp = c & 0x1fu;
        lo = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        need = 3;
        cp = c & 0x0fu;
        lo = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        need = 4;
        cp = c & 0x07u;
        lo = 0x10000;
    } else
        return 0;
    if (s.n - i < need) return 0;
    for (size_t k = 1; k < need; k++) {
        u8 t = (u8)s.p[i + k];
        if ((t & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (t & 0x3fu);
    }
    if (cp < lo || cp > 0x10ffff) return 0;
    if (cp >= 0xd800 && cp <= 0xdfff) return 0;
    return need;
}

void buf_json_chars(Buf *b, Str s) {
    for (size_t i = 0; i < s.n; i++) {
        u8 c = (u8)s.p[i];
        switch (c) {
            case '"': buf_put(b, "\\\"", 2); break;
            case '\\': buf_put(b, "\\\\", 2); break;
            case '\b': buf_put(b, "\\b", 2); break;
            case '\f': buf_put(b, "\\f", 2); break;
            case '\n': buf_put(b, "\\n", 2); break;
            case '\r': buf_put(b, "\\r", 2); break;
            case '\t': buf_put(b, "\\t", 2); break;
            default: {
                if (c < 0x20) {
                    buf_putf(b, "\\u%04x", c);
                    break;
                }
                if (c < 0x80) {
                    buf_putc(b, (char)c);
                    break;
                }
                size_t seq = utf8_seq(s, i);
                if (!seq) {
                    buf_put(b, "\xef\xbf\xbd", 3);
                    break;
                }
                buf_put(b, s.p + i, seq);
                i += seq - 1;
            }
        }
    }
}

void buf_base64(Buf *b, const void *p, size_t n) {
    static const char k_b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!n || !p) return;
    if (n > ((size_t)-1 - b->n) / 4 * 3) {
        b->oom = true;
        return;
    }
    if (!buf_reserve(b, b->n + (n + 2) / 3 * 4)) return;
    const u8 *s = (const u8 *)p;
    size_t i = 0;
    for (; n - i >= 3; i += 3) {
        u32 v = (u32)s[i] << 16 | (u32)s[i + 1] << 8 | s[i + 2];
        b->p[b->n++] = k_b64[v >> 18];
        b->p[b->n++] = k_b64[(v >> 12) & 0x3f];
        b->p[b->n++] = k_b64[(v >> 6) & 0x3f];
        b->p[b->n++] = k_b64[v & 0x3f];
    }
    if (i == n) return;
    u32 v = (u32)s[i] << 16;
    if (n - i == 2) v |= (u32)s[i + 1] << 8;
    b->p[b->n++] = k_b64[v >> 18];
    b->p[b->n++] = k_b64[(v >> 12) & 0x3f];
    b->p[b->n++] = n - i == 2 ? k_b64[(v >> 6) & 0x3f] : '=';
    b->p[b->n++] = '=';
}

static i32 base64_digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

B64Status base64_decode(Arena *a, Str text, size_t max, Str *out) {
    *out = (Str){0};
    size_t n = text.n;
    if (n % 4 == 0 && n && text.p[n - 1] == '=') n--;
    if (n % 4 == 3 && n && text.p[n - 1] == '=') n--;
    if (n % 4 == 1) return B64_MALFORMED;
    if (n < text.n && (text.n % 4 || n % 4 == 0)) return B64_MALFORMED;
    size_t bytes = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
    if (bytes > max) return B64_TOO_LARGE;
    if (!bytes) return n ? B64_MALFORMED : B64_OK;
    u8 *p = arena_alloc(a, bytes, 1);
    if (!p) return B64_NO_MEMORY;
    size_t at = 0;
    u32 acc = 0;
    for (size_t i = 0; i < n; i++) {
        i32 d = base64_digit(text.p[i]);
        if (d < 0) return B64_MALFORMED;
        acc = acc << 6 | (u32)d;
        if (i % 4 == 3) {
            p[at++] = (u8)(acc >> 16);
            p[at++] = (u8)(acc >> 8);
            p[at++] = (u8)acc;
            acc = 0;
        }
    }
    if (n % 4 == 2) p[at++] = (u8)(acc >> 4);
    if (n % 4 == 3) {
        p[at++] = (u8)(acc >> 10);
        p[at++] = (u8)(acc >> 2);
    }
    *out = (Str){(const char *)p, at};
    return B64_OK;
}

Str buf_finish(Buf *b) {
    if (b->n == b->cap && !buf_grow(b, b->n + 1)) {
        if (b->n == 0) return (Str){0};
        b->n--;
    }
    b->p[b->n] = '\0';
    return (Str){b->p, b->n};
}


static struct {
    i32 level;
    AgentLogSink sink;
    void *ud;
} g_log = {.level = AGENT_LOG_INFO};

void agent_log_set_level(i32 level) {
    g_log.level = level;
}
void agent_log_set_sink(AgentLogSink sink, void *ud) {
    g_log.sink = sink;
    g_log.ud = ud;
}
static void log_emit(i32 level, b8 recorded, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));

static void log_emit(i32 level, b8 recorded, const char *fmt, va_list ap) {
    if (level < g_log.level) return;
    static const char *tags[] = {"DBG", "INF", "WRN", "ERR"};
    if (level < AGENT_LOG_DEBUG || level > AGENT_LOG_ERROR)
        level = AGENT_LOG_ERROR;
    char msg[512];
    i32 w = vsnprintf(msg, sizeof msg, fmt, ap);
    size_t n =
        w > 0 ? ((size_t)w < sizeof msg ? (size_t)w : sizeof msg - 1) : 0;
    if (recorded) telemetry_log(level, (Str){msg, n});
    if (g_log.sink) {
        g_log.sink(level, (Str){msg, n}, g_log.ud);
        return;
    }
    fprintf(stderr, "[" AGENT_NAME " %s] %.*s\n", tags[level], (i32)n, msg);
}

void agent_log(i32 level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_emit(level, true, fmt, ap);
    va_end(ap);
}

void agent_log_local(i32 level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_emit(level, false, fmt, ap);
    va_end(ap);
}


f64 agent_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}
