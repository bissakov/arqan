/* highlight.c: bounded client for the optional arqan-highlight companion. */
#include "agent.h"
#include "highlight_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define YHL_TIMEOUT_MS 500
#define YHL_POLL_MS 25

typedef struct {
    char path[AGENT_MAX_PATH];
    b8 path_only;
    b8 disabled;
    pid_t pid;
    i32 in_fd;
    i32 out_fd;
    u32 next_id;
} HighlightClient;

static HighlightClient g_hl = { .pid = -1, .in_fd = -1, .out_fd = -1 };

static void put_u32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 get_u32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static b8 copy_path(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (!n || n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

void highlight_init(const char *argv0) {
    g_hl = (HighlightClient){ .pid = -1, .in_fd = -1, .out_fd = -1,
                              .next_id = 1 };
    const char *override = getenv(AGENT_ENV_PREFIX "HIGHLIGHTER");
    if (override && *override) {
        if (!copy_path(g_hl.path, sizeof g_hl.path, override)) g_hl.disabled = true;
        else g_hl.path_only = true;
        return;
    }

    char resolved[AGENT_MAX_PATH];
    const char *exe = NULL;
    if (argv0 && strchr(argv0, '/')) {
        exe = realpath(argv0, resolved);
    } else if (argv0 && *argv0) {
        const char *path = getenv("PATH");
        if (!path) path = "";
        for (size_t off = 0; !exe;) {
            size_t end = off;
            while (path[end] && path[end] != ':') end++;
            Str dir = { path + off, end - off };
            i32 n = dir.n
                  ? snprintf(resolved, sizeof resolved, "%.*s/%s",
                             (i32)dir.n, dir.p, argv0)
                  : snprintf(resolved, sizeof resolved, "./%s", argv0);
            if (n > 0 && (size_t)n < sizeof resolved
                && access(resolved, X_OK) == 0) {
                char found[AGENT_MAX_PATH];
                memcpy(found, resolved, (size_t)n + 1);
                exe = realpath(found, resolved);
            }
            if (!path[end]) break;
            off = end + 1;
        }
    }
    if (exe) {
        char *slash = strrchr(resolved, '/');
        if (slash) {
            size_t dir_n = (size_t)(slash - resolved) + 1;
            static const char helper[] = AGENT_NAME "-highlight";
            if (dir_n + sizeof helper <= sizeof g_hl.path) {
                memcpy(g_hl.path, resolved, dir_n);
                memcpy(g_hl.path + dir_n, helper, sizeof helper);
                if (access(g_hl.path, X_OK) == 0) {
                    g_hl.path_only = true;
                    return;
                }
            }
        }
    }
    copy_path(g_hl.path, sizeof g_hl.path, AGENT_NAME "-highlight");
}

static void close_fd(i32 *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void highlight_disable(void) {
    close_fd(&g_hl.in_fd);
    close_fd(&g_hl.out_fd);
    if (g_hl.pid > 0) {
        kill(g_hl.pid, SIGKILL);
        while (waitpid(g_hl.pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    g_hl.pid = -1;
    g_hl.disabled = true;
}

static b8 set_nonblock(i32 fd) {
    i32 flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static b8 highlight_start(void) {
    if (g_hl.disabled) return false;
    if (g_hl.pid > 0) return true;
    i32 to_child[2], from_child[2];
    if (pipe(to_child) != 0) { g_hl.disabled = true; return false; }
    if (pipe(from_child) != 0) {
        close(to_child[0]); close(to_child[1]);
        g_hl.disabled = true;
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        g_hl.disabled = true;
        return false;
    }
    if (pid == 0) {
        i32 devnull = open("/dev/null", O_WRONLY);
        if (dup2(to_child[0], STDIN_FILENO) < 0
            || dup2(from_child[1], STDOUT_FILENO) < 0)
            _exit(126);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        if (devnull > STDERR_FILENO) close(devnull);
        if (g_hl.path_only) execl(g_hl.path, g_hl.path, (char *)NULL);
        else execlp(g_hl.path, g_hl.path, (char *)NULL);
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    g_hl.pid = pid;
    g_hl.in_fd = to_child[1];
    g_hl.out_fd = from_child[0];
    if (!set_nonblock(g_hl.in_fd) || !set_nonblock(g_hl.out_fd)) {
        highlight_disable();
        return false;
    }
    return true;
}

static b8 io_all(i32 fd, i16 events, u8 *p, size_t n, f64 deadline) {
    size_t off = 0;
    while (off < n) {
        f64 left = deadline - agent_now_seconds();
        if (left <= 0) return false;
        i32 ms = (i32)(left * 1000.0);
        if (ms > YHL_POLL_MS) ms = YHL_POLL_MS;
        if (ms < 1) ms = 1;
        struct pollfd pfd = { fd, events, 0 };
        i32 rc = poll(&pfd, 1, ms);
        tui_poll_input();
        if (rc < 0) { if (errno == EINTR) continue; return false; }
        if (rc == 0) continue;
        if (pfd.revents & (POLLERR | POLLNVAL)) return false;
        if (!(pfd.revents & (events | POLLHUP))) continue;
        ssize_t got = events == POLLOUT
                    ? write(fd, p + off, n - off)
                    : read(fd, p + off, n - off);
        if (got > 0) { off += (size_t)got; continue; }
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return false;
    }
    return true;
}

b8 highlight_request(YhlHintKind kind, Str hint, Str source,
                     YhlResult *result) {
    result->n = 0;
    if (md_raw() || !tui_highlight_enabled() || g_hl.disabled) return false;
    size_t hint_max = kind == YHL_HINT_MARKDOWN_ALIAS
                    ? YHL_ALIAS_MAX : YHL_FILENAME_MAX;
    if (!hint.n || hint.n > hint_max || source.n > YHL_SOURCE_MAX) return false;
    if (!highlight_start()) return false;

    u8 header[YHL_REQUEST_HEADER] = {0};
    memcpy(header, YHL_MAGIC, 4);
    u32 id = g_hl.next_id++;
    if (!g_hl.next_id) g_hl.next_id = 1;
    put_u32(header + 4, id);
    header[8] = (u8)kind;
    put_u32(header + 12, (u32)hint.n);
    put_u32(header + 16, (u32)source.n);
    f64 deadline = agent_now_seconds() + (f64)YHL_TIMEOUT_MS / 1000.0;

    struct sigaction oldpipe, ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    b8 have_old = sigaction(SIGPIPE, &ignore, &oldpipe) == 0;
    b8 ok = io_all(g_hl.in_fd, POLLOUT, header, sizeof header, deadline)
         && io_all(g_hl.in_fd, POLLOUT, (u8 *)(uintptr_t)hint.p, hint.n, deadline)
         && io_all(g_hl.in_fd, POLLOUT, (u8 *)(uintptr_t)source.p, source.n,
                   deadline);
    if (have_old) sigaction(SIGPIPE, &oldpipe, NULL);
    if (!ok) { highlight_disable(); return false; }

    u8 response[YHL_RESPONSE_HEADER];
    if (!io_all(g_hl.out_fd, POLLIN, response, sizeof response, deadline)
        || memcmp(response, YHL_MAGIC, 4) != 0
        || get_u32(response + 4) != id) {
        highlight_disable();
        return false;
    }
    u8 status = response[8];
    u32 count = get_u32(response + 12);
    if (count > YHL_RUN_MAX || (status != YHL_STATUS_OK && count != 0)) {
        highlight_disable();
        return false;
    }
    if (status == YHL_STATUS_UNKNOWN || status == YHL_STATUS_TOO_LARGE
        || status == YHL_STATUS_TOO_COMPLEX)
        return false;
    if (status != YHL_STATUS_OK) { highlight_disable(); return false; }

    u32 previous = 0;
    for (u32 i = 0; i < count; i++) {
        u8 wire[YHL_RUN_BYTES];
        if (!io_all(g_hl.out_fd, POLLIN, wire, sizeof wire, deadline)) {
            highlight_disable(); result->n = 0; return false;
        }
        u32 a = get_u32(wire);
        u32 b = get_u32(wire + 4);
        u8 semantic = wire[8];
        if (a < previous || a >= b || b > source.n
            || semantic < YHL_SEM_COMMENT || semantic > YHL_SEM_BUILTIN) {
            highlight_disable(); result->n = 0; return false;
        }
        result->run[i] = (YhlRun){ a, b, semantic };
        previous = b;
    }
    result->n = count;
    return true;
}

void highlight_close(void) {
    close_fd(&g_hl.in_fd);
    close_fd(&g_hl.out_fd);
    if (g_hl.pid > 0) {
        for (i32 i = 0; i < 5; i++) {
            pid_t rc = waitpid(g_hl.pid, NULL, WNOHANG);
            if (rc == g_hl.pid) { g_hl.pid = -1; return; }
            if (rc < 0 && errno != EINTR) break;
            poll(NULL, 0, 10);
        }
        kill(g_hl.pid, SIGKILL);
        while (waitpid(g_hl.pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    g_hl.pid = -1;
}
