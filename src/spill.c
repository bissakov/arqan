#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>


Str spill_dir(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] != '/') tmp = "/tmp";
    Str s = str_c(tmp);
    while (s.n > 1 && s.p[s.n - 1] == '/') s.n--;
    return s;
}

#define SPILL_SECRET_BYTES 16
#define SPILL_NAME_TRIES   (SHA256_BYTES / sizeof(u64))

static struct {
    pid_t pid;
    u8 secret[SPILL_SECRET_BYTES];
} g_spill_secret;

static b8 spill_read_random(u8 *out, size_t n) {
    i32 fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n;
}

static void spill_secret_refresh(void) {
    pid_t pid = getpid();
    if (g_spill_secret.pid == pid) return;
    g_spill_secret.pid = pid;
    if (spill_read_random(g_spill_secret.secret, SPILL_SECRET_BYTES)) return;
    u64 seed[2] = {(u64)pid, (u64)(agent_now_seconds() * 1e9)};
    memcpy(g_spill_secret.secret, seed, sizeof seed);
}

static void spill_names(Str key, u8 names[SHA256_BYTES]) {
    spill_secret_refresh();
    u8 mix[SPILL_SECRET_BYTES + SHA256_BYTES];
    memcpy(mix, g_spill_secret.secret, SPILL_SECRET_BYTES);
    sha256(key.p, key.n, mix + SPILL_SECRET_BYTES);
    sha256(mix, sizeof mix, names);
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
    u8 names[SHA256_BYTES];
    spill_names(key, names);
    for (size_t i = 0; i < SPILL_NAME_TRIES; i++) {
        u64 name;
        memcpy(&name, names + i * sizeof name, sizeof name);
        i32 n = snprintf(s->path, sizeof s->path,
                         "%.*s/" AGENT_NAME "-%s-%016llx.%s", (i32)dir.n, dir.p,
                         tool, (unsigned long long)name, ext);
        if (n < 0 || (size_t)n >= AGENT_SPILL_PATH_MAX) break;
        if (unlink(s->path) != 0 && errno != ENOENT) continue;
        i32 fd =
            open(s->path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                 0600);
        if (fd >= 0) {
            s->fd = fd;
            return;
        }
        if (errno != EEXIST) break;
    }
    s->path[0] = '\0';
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


b8 spill_path_ours(const char *resolved) {
    char dir[PATH_MAX];
    Str tmp = spill_dir();
    if (tmp.n >= sizeof dir) return false;
    memcpy(dir, tmp.p, tmp.n);
    dir[tmp.n] = '\0';
    char real_dir[PATH_MAX];
    if (!realpath(dir, real_dir)) return false;
    size_t n = strlen(real_dir);
    if (n == 1) n = 0;
    if (strncmp(resolved, real_dir, n) != 0 || resolved[n] != '/') return false;
    const char *name = resolved + n + 1;
    Str prefix = STR(AGENT_NAME "-");
    if (strchr(name, '/') || strncmp(name, prefix.p, prefix.n) != 0)
        return false;
    struct stat st;
    return stat(resolved, &st) == 0 && S_ISREG(st.st_mode)
           && st.st_uid == getuid();
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
