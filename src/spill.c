#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static Str spill_dir(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] != '/') tmp = "/tmp";
    Str s = str_c(tmp);
    while (s.n > 1 && s.p[s.n - 1] == '/') s.n--;
    return s;
}

static void spill_drop(Spill *s) {
    if (s->fd >= 0) close(s->fd);
    if (s->path[0]) unlink(s->path);
    s->fd = -1;
    s->path[0] = '\0';
    s->buf_n = 0;
}

void spill_open(Spill *s, const char *tool, const char *ext, Str key) {
    s->fd = -1;
    s->path[0] = '\0';
    s->written = 0;
    s->buf_n = 0;
    s->full = false;

    Str dir = spill_dir();
    i32 n = snprintf(s->path, sizeof s->path,
                     "%.*s/" AGENT_NAME "-%s-%016llx.%s", (i32)dir.n, dir.p,
                     tool, (unsigned long long)str_hash64(key), ext);

    if (n < 0 || (size_t)n >= AGENT_SPILL_PATH_MAX) {
        s->path[0] = '\0';
        return;
    }

    if (unlink(s->path) != 0 && errno != ENOENT) {
        s->path[0] = '\0';
        return;
    }
    i32 fd = open(s->path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0) {
        s->path[0] = '\0';
        return;
    }
    s->fd = fd;
}

static b8 spill_raw(Spill *s, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(s->fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            spill_drop(s);
            return false;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return true;
}

static b8 spill_flush(Spill *s) {
    if (s->fd < 0 || !s->buf_n) return s->fd >= 0;
    size_t n = s->buf_n;
    s->buf_n = 0;
    return spill_raw(s, s->buf, n);
}

void spill_put(Spill *s, const char *p, size_t n) {
    if (s->fd < 0 || !n) return;
    if (s->written >= AGENT_SPILL_BYTES) {
        s->full = true;
        return;
    }
    if (n > AGENT_SPILL_BYTES - s->written) {
        n = AGENT_SPILL_BYTES - s->written;
        s->full = true;
    }
    s->written += n;
    if (n >= sizeof s->buf) {
        if (spill_flush(s)) spill_raw(s, p, n);
        return;
    }
    if (s->buf_n + n > sizeof s->buf && !spill_flush(s)) return;
    memcpy(s->buf + s->buf_n, p, n);
    s->buf_n += n;
}

void spill_putf(Spill *s, const char *fmt, ...) {
    if (s->fd < 0) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    i32 n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    spill_put(s, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}


void spill_size_text(char *z, size_t cap, size_t n) {
    if (n < 1024)
        snprintf(z, cap, "%zu B", n);
    else if (n < 1024 * 1024)
        snprintf(z, cap, "%.0f KB", (f64)n / 1024.0);
    else
        snprintf(z, cap, "%.1f MB", (f64)n / (1024.0 * 1024.0));
}

i32 spill_release(Spill *s, char *path, size_t path_cap, size_t *written) {
    *written = 0;
    if (s->fd < 0) return -1;
    if (!spill_flush(s) || !s->path[0]) {
        spill_drop(s);
        return -1;
    }
    size_t n = strlen(s->path);
    if (n >= path_cap) {
        spill_drop(s);
        return -1;
    }
    memcpy(path, s->path, n + 1);
    *written = s->written;
    i32 fd = s->fd;
    s->fd = -1;
    s->path[0] = '\0';
    s->written = 0;
    s->buf_n = 0;
    return fd;
}

void spill_finish(Spill *s, Buf *out, b8 keep) {
    if (s->fd < 0) return;
    b8 ok = spill_flush(s);
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    if (!ok || !keep || !s->written) {
        if (s->path[0]) unlink(s->path);
        s->path[0] = '\0';
        return;
    }
    char size[32];
    spill_size_text(size, sizeof size, s->written);

    if (out->n && out->p[out->n - 1] != '\n') buf_puts(out, STR("\n\n"));
    buf_putf(out, "[full output: %s, %s%s; grep or read part of it]\n", s->path,
             size, s->full ? ", truncated there" : "");
}
