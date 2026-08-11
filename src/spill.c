/* spill.c: the whole of a tool's output, kept on disk when the result is a page.
 *
 * A tool result is replayed to the provider on every later turn, so each tool
 * returns a bounded page. The bytes that did not fit are still worth having:
 * a spill writes the complete output to a file and the tool names that path,
 * so the next call narrows it with bash instead of paging it all back into
 * the context.
 *
 * The file name hashes the call, so a repeated call overwrites its own file
 * rather than filling the directory, and a spill nothing was dropped from is
 * unlinked before the tool returns.
 *
 * Spilling is best effort: a spill that cannot be opened or written closes
 * itself and the tool answers exactly as it did before.
 */
#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* $TMPDIR, or /tmp when it is unset or relative. */
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
    /* The note naming the path is charged to the same result budget the page
     * is, so a path that would not leave room for it is no spill at all. */
    if (n < 0 || (size_t)n >= AGENT_SPILL_PATH_MAX) { s->path[0] = '\0'; return; }

    /* The name is predictable and the directory is shared, so the previous
     * file is removed and the new one created exclusively: an existing entry
     * this cannot unlink belongs to another user (the sticky bit keeps it
     * theirs) and is left alone rather than written through. O_NOFOLLOW
     * refuses a symlink planted between the two calls. */
    if (unlink(s->path) != 0 && errno != ENOENT) { s->path[0] = '\0'; return; }
    i32 fd = open(s->path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0) { s->path[0] = '\0'; return; }
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
    if (s->written >= AGENT_SPILL_BYTES) { s->full = true; return; }
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

/* A size the model reads at a glance: what it decides is whether to grep the
 * file or read a range of it, and no such choice turns on the last byte. */
static void spill_size(char *z, size_t cap, size_t n) {
    if (n < 1024) snprintf(z, cap, "%zu B", n);
    else if (n < 1024 * 1024) snprintf(z, cap, "%.0f KB", (f64)n / 1024.0);
    else snprintf(z, cap, "%.1f MB", (f64)n / (1024.0 * 1024.0));
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
    spill_size(size, sizeof size, s->written);
    /* The note is its own line wherever it lands, including after a result
     * that ends mid-line. */
    if (out->n && out->p[out->n - 1] != '\n') buf_puts(out, STR("\n\n"));
    buf_putf(out, "[full output: %s, %s%s; grep or read part of it]\n",
             s->path, size, s->full ? ", truncated there" : "");
}
