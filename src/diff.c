/* The /diff command: what git says changed in the files this session wrote.
 *
 * git runs as a child with no shell, under one deadline for every run, and
 * its output is bounded by what the view window can hold. Nothing here
 * reaches the model: the model keeps using `git diff` through `bash`.
 */
#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIFF_POLL_MS   50
#define DIFF_ERR_BYTES 4096
#define DIFF_ARGV_MAX  (AGENT_MAX_TOUCHED + 8)

typedef struct {
    void (*idle)(void *ud);
    void *idle_ud;
    b8 running;
} DiffState;

static DiffState g_diff;

void diff_set_idle(void (*fn)(void *ud), void *ud) {
    g_diff.idle = fn;
    g_diff.idle_ud = ud;
}

typedef enum {
    DIFF_RUN_OK = 0,
    DIFF_RUN_ABSENT,
    DIFF_RUN_FAILED,
    DIFF_RUN_TIMEOUT,
    DIFF_RUN_BIG,
    DIFF_RUN_NOMEM
} DiffRunStatus;

typedef struct {
    i32 fd;
    Buf *buf;
    size_t cap;
    b8 over;
    b8 open;
} DiffPipe;

static b8 diff_pipe_pump(DiffPipe *p) {
    char chunk[4096];
    ssize_t got = read(p->fd, chunk, sizeof chunk);
    if (got < 0) return errno == EINTR || errno == EAGAIN;
    if (got == 0) {
        close(p->fd);
        p->open = false;
        return true;
    }
    if (p->buf->n + (size_t)got > p->cap) {
        p->over = true;
        return false;
    }
    buf_put(p->buf, chunk, (size_t)got);
    return buf_ok(p->buf);
}

static void diff_pipe_close(DiffPipe *p) {
    if (p->open) close(p->fd);
    p->open = false;
}

static DiffRunStatus diff_run(const char *const *argv, Buf *out, Buf *err,
                              f64 deadline, i32 *code) {
    *code = -1;
    i32 out_fds[2], err_fds[2];
    if (!pipe_cloexec(out_fds)) return DIFF_RUN_FAILED;
    if (!pipe_cloexec(err_fds)) {
        close(out_fds[0]);
        close(out_fds[1]);
        return DIFF_RUN_FAILED;
    }
    char **envp = child_env(CHILD_ENV_PLAIN);
    pid_t pid = fork();
    if (pid < 0) {
        close(out_fds[0]);
        close(out_fds[1]);
        close(err_fds[0]);
        close(err_fds[1]);
        return DIFF_RUN_FAILED;
    }
    if (pid == 0) {
        i32 null_rd = open("/dev/null", O_RDONLY);
        if (null_rd >= 0) dup2(null_rd, STDIN_FILENO);
        dup2(out_fds[1], STDOUT_FILENO);
        dup2(err_fds[1], STDERR_FILENO);
        if (null_rd > STDERR_FILENO) close(null_rd);
        child_close_fds(3);
        environ = envp;
        execvp(argv[0], (char *const *)(uintptr_t)argv);
        _exit(127);
    }
    close(out_fds[1]);
    close(err_fds[1]);

    DiffPipe pipes[2] = {
        {out_fds[0], out, AGENT_RESP_BUF, false, true},
        {err_fds[0], err, DIFF_ERR_BYTES, false, true},
    };
    DiffRunStatus status = DIFF_RUN_OK;
    while (pipes[0].open || pipes[1].open) {
        f64 left = deadline - agent_now_seconds();
        if (left <= 0) {
            status = DIFF_RUN_TIMEOUT;
            break;
        }
        i32 ms = (i32)(left * 1000.0);
        if (ms > DIFF_POLL_MS) ms = DIFF_POLL_MS;
        if (ms < 1) ms = 1;
        struct pollfd pfd[2];
        size_t n = 0;
        for (size_t i = 0; i < 2; i++)
            if (pipes[i].open)
                pfd[n++] = (struct pollfd){pipes[i].fd, POLLIN, 0};
        i32 rc = poll(pfd, (nfds_t)n, ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            status = DIFF_RUN_FAILED;
            break;
        }
        if (rc == 0) {
            if (g_diff.idle) g_diff.idle(g_diff.idle_ud);
            continue;
        }
        b8 ok = true;
        for (size_t k = 0; k < n && ok; k++) {
            if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            for (size_t i = 0; i < 2; i++)
                if (pipes[i].open && pipes[i].fd == pfd[k].fd)
                    ok = diff_pipe_pump(&pipes[i]);
        }
        if (!ok) {
            if (pipes[1].over) {
                diff_pipe_close(&pipes[1]);
                pipes[1].over = false;
                continue;
            }
            status = pipes[0].over                  ? DIFF_RUN_BIG
                     : !buf_ok(out) || !buf_ok(err) ? DIFF_RUN_NOMEM
                                                    : DIFF_RUN_FAILED;
            break;
        }
    }
    diff_pipe_close(&pipes[0]);
    diff_pipe_close(&pipes[1]);
    if (status != DIFF_RUN_OK) kill(pid, SIGKILL);
    i32 wait_status = 0;
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {}
    if (status != DIFF_RUN_OK) return status;
    *code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
    if (*code == 127) return DIFF_RUN_ABSENT;
    return DIFF_RUN_OK;
}

static Str diff_fatal_line(Str s) {
    Str line = {0}, first = {0};
    size_t off = 0;
    while (str_line(s, &off, &line)) {
        line = str_trim(line);
        if (!line.n) continue;
        if (!first.n) first = line;
        if (str_starts(line, STR("fatal: "))) return str_drop(line, 7);
    }
    return first;
}

static b8 diff_report(DiffRunStatus status, i32 code, Str stderr_text,
                      char *err, size_t err_cap) {
    switch (status) {
        case DIFF_RUN_OK: break;
        case DIFF_RUN_ABSENT:
            snprintf(err, err_cap, "git is not installed");
            return false;
        case DIFF_RUN_TIMEOUT:
            snprintf(err, err_cap, "git diff did not finish in %ds",
                     AGENT_DIFF_TIMEOUT_MS / 1000);
            return false;
        case DIFF_RUN_BIG:
            snprintf(err, err_cap,
                     "diff is larger than %u bytes; run git diff yourself",
                     AGENT_RESP_BUF);
            return false;
        case DIFF_RUN_NOMEM:
            snprintf(err, err_cap, "out of memory capturing the diff");
            return false;
        case DIFF_RUN_FAILED:
            snprintf(err, err_cap, "could not run git");
            return false;
    }
    if (code == 0 || code == 1) return true;
    Str line = diff_fatal_line(stderr_text);
    if (line.n)
        snprintf(err, err_cap, "git diff failed: %.*s", (i32)line.n, line.p);
    else
        snprintf(err, err_cap, "git diff failed with exit code %d", code);
    return false;
}

static const char *diff_arg(Arena *scratch, Str path) {
    if (!path.n || path.n >= AGENT_MAX_PATH || memchr(path.p, '\0', path.n))
        return NULL;
    char *z = arena_alloc(scratch, path.n + 1, 1);
    if (!z) return NULL;
    memcpy(z, path.p, path.n);
    z[path.n] = '\0';
    return z;
}

static size_t diff_argv_paths(const Conv *c, Arena *scratch, const char **argv,
                              size_t argc) {
    for (size_t i = 0; i < c->touched_n && argc + 1 < DIFF_ARGV_MAX; i++) {
        const char *z = diff_arg(scratch, c->touched[i]);
        if (z) argv[argc++] = z;
    }
    argv[argc] = NULL;
    return argc;
}

static Str diff_text(const Buf *b) {
    return (Str){b->p, b->n};
}

static b8 diff_untracked(Str listing, Arena *scratch, Buf *out, Buf *err,
                         f64 deadline, char *msg, size_t msg_cap) {
    size_t off = 0, runs = 0;
    Str line;
    while (runs < AGENT_MAX_TOUCHED && str_line(listing, &off, &line)) {
        line = str_trim(line);
        if (!line.n) continue;
        const char *z = diff_arg(scratch, line);
        if (!z) continue;
        const char *argv[] = {
            "git",        "diff", "--no-color", "--no-ext-diff",
            "--no-index", "--",   "/dev/null",  z,
            NULL};
        err->n = 0;
        i32 code;
        DiffRunStatus st = diff_run(argv, out, err, deadline, &code);
        if (!diff_report(st, code, diff_text(err), msg, msg_cap)) return false;
        runs++;
    }
    return true;
}

static b8 diff_capture_runs(const Conv *c, Arena *scratch, Str *out, char *err,
                            size_t err_cap) {
    const char **argv = arena_new(scratch, const char *, DIFF_ARGV_MAX);
    Buf body, stderr_buf, listing;
    buf_init(&body, scratch, 1 << 16);
    buf_init(&stderr_buf, scratch, 1024);
    buf_init(&listing, scratch, 4096);
    if (!argv || !buf_ok(&body) || !buf_ok(&stderr_buf) || !buf_ok(&listing)) {
        snprintf(err, err_cap, "out of memory capturing the diff");
        return false;
    }
    f64 deadline = agent_now_seconds() + (f64)AGENT_DIFF_TIMEOUT_MS / 1000.0;
    i32 code;

    size_t argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "diff";
    argv[argc++] = "--no-color";
    argv[argc++] = "--no-ext-diff";
    argv[argc++] = "--";
    diff_argv_paths(c, scratch, argv, argc);
    DiffRunStatus st = diff_run(argv, &body, &stderr_buf, deadline, &code);
    if (!diff_report(st, code, diff_text(&stderr_buf), err, err_cap))
        return false;

    argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "ls-files";
    argv[argc++] = "--others";
    argv[argc++] = "--exclude-standard";
    argv[argc++] = "--";
    diff_argv_paths(c, scratch, argv, argc);
    stderr_buf.n = 0;
    st = diff_run(argv, &listing, &stderr_buf, deadline, &code);
    if (!diff_report(st, code, diff_text(&stderr_buf), err, err_cap))
        return false;

    if (!diff_untracked(diff_text(&listing), scratch, &body, &stderr_buf,
                        deadline, err, err_cap))
        return false;
    if (!buf_ok(&body)) {
        snprintf(err, err_cap, "out of memory capturing the diff");
        return false;
    }
    *out = buf_finish(&body);
    return true;
}

b8 diff_capture(const Conv *c, Arena *scratch, Str *out, char *err,
                size_t err_cap) {
    *out = (Str){0};
    if (err_cap) err[0] = '\0';
    if (!c->touched_n) {
        snprintf(err, err_cap, "no files were written in this session");
        return false;
    }
    if (g_diff.running) {
        snprintf(err, err_cap, "a diff is already being captured");
        return false;
    }
    g_diff.running = true;
    b8 ok = diff_capture_runs(c, scratch, out, err, err_cap);
    g_diff.running = false;
    return ok;
}
