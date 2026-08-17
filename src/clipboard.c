#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Reading the clipboard needs a helper program. A terminal application can
 * write one over OSC 52 but cannot read it back, and no escape sequence
 * carries an image in either direction. Each helper is spawned with an argv
 * of its own, never through a shell, so nothing a clipboard holds and nothing
 * a helper prints can become part of a command line.
 *
 * The poll loop is this module's rather than shared with secrets.c: that one
 * feeds a secret on stdin and is careful about where a key can be seen, and
 * folding two different contracts into one runner would serve neither.
 */

#define CLIP_POLL_MS   20
#define CLIP_LIST_MAX  4096   // a type listing is a few dozen short lines
#define CLIP_ARGV_MAX  8
// The element a helper's argv carries where the media type belongs.
#define CLIP_TYPE_MARK "%t"

typedef struct {
    const char *list[CLIP_ARGV_MAX];    // prints the types on offer, or empty
    const char *fetch[CLIP_ARGV_MAX];   // prints one type's bytes
} ClipHelper;

static const ClipHelper k_clip[] = {
    { { "wl-paste", "--list-types", NULL },
      { "wl-paste", "--no-newline", "--type", CLIP_TYPE_MARK, NULL } },
    { { "xclip", "-selection", "clipboard", "-t", "TARGETS", "-o", NULL },
      { "xclip", "-selection", "clipboard", "-t", CLIP_TYPE_MARK, "-o",
        NULL } },
    /* pngpaste lists nothing: it answers with a PNG or with nothing, which is
     * what makes it the macOS entry. */
    { { NULL }, { "pngpaste", "-", NULL } },
};

/* What to ask a clipboard for, in the order it is asked. The listing says
 * what is on offer; this is the preference over it. */
static const Str k_clip_types[] = {
    { "image/png", 9 }, { "image/jpeg", 10 },
    { "image/gif", 9 }, { "image/webp", 10 },
};

typedef enum {
    CLIP_OK = 0,
    CLIP_ABSENT,   // the helper is not installed
    CLIP_NONE,     // it ran and offered nothing of the kind
    CLIP_BIG,      // more bytes than an image may have
    CLIP_FAILED,   // spawn, pipe or deadline
} ClipStatus;

/* Poll one fd until the deadline. Returns 1 ready, 0 timed out, -1 failed.
 *
 * The UI is deliberately not pumped here, unlike the waits that sit behind a
 * network request: a clipboard read is local and bounded by
 * AGENT_CLIPBOARD_TIMEOUT_MS, and Ctrl-V mid-turn reaches this from inside
 * the input dispatch, where a nested tui_poll_input would deliver the keys
 * after it a second time. */
static i32 clip_wait(i32 fd, f64 deadline) {
    for (;;) {
        f64 left = deadline - agent_now_seconds();
        if (left <= 0) return 0;
        i32 ms = (i32)(left * 1000.0);
        if (ms > CLIP_POLL_MS) ms = CLIP_POLL_MS;
        if (ms < 1) ms = 1;
        struct pollfd pfd = { fd, POLLIN, 0 };
        i32 rc = poll(&pfd, 1, ms);
        if (rc < 0) { if (errno == EINTR) continue; return -1; }
        if (rc > 0) return 1;
    }
}

/* Runs `argv` with the type mark replaced by `type`, collecting at most `cap`
 * bytes of its stdout. The child's stderr goes nowhere: a helper explaining
 * itself must not paint over a raw-mode screen. */
static ClipStatus clip_exec(const char *const *argv, Str type, char *out,
                            size_t cap, size_t *n) {
    const char *args[CLIP_ARGV_MAX];
    char typez[32];
    size_t argc = 0;
    if (type.n >= sizeof typez) return CLIP_FAILED;
    /* The listing call passes no type at all, and memcpy is not defined for
     * a null source however few bytes it is asked for. */
    if (type.n) memcpy(typez, type.p, type.n);
    typez[type.n] = '\0';
    for (; argc + 1 < CLIP_ARGV_MAX && argv[argc]; argc++)
        args[argc] = strcmp(argv[argc], CLIP_TYPE_MARK) ? argv[argc] : typez;
    if (!argc) return CLIP_FAILED;
    args[argc] = NULL;

    *n = 0;
    i32 fds[2];
    if (pipe(fds) != 0) return CLIP_FAILED;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return CLIP_FAILED; }
    if (pid == 0) {
        i32 null_rd = open("/dev/null", O_RDONLY);
        i32 null_wr = open("/dev/null", O_WRONLY);
        if (null_rd >= 0) dup2(null_rd, STDIN_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        if (null_wr >= 0) dup2(null_wr, STDERR_FILENO);
        if (null_rd > STDERR_FILENO) close(null_rd);
        if (null_wr > STDERR_FILENO) close(null_wr);
        close(fds[0]); close(fds[1]);
        execvp(args[0], (char *const *)(uintptr_t)args);
        _exit(127);
    }
    close(fds[1]);

    f64 deadline = agent_now_seconds()
                 + (f64)AGENT_CLIPBOARD_TIMEOUT_MS / 1000.0;
    b8 ok = true, timed_out = false, over = false;
    while (ok) {
        i32 rc = clip_wait(fds[0], deadline);
        if (rc == 0) { timed_out = true; ok = false; break; }
        if (rc < 0) { ok = false; break; }
        if (*n >= cap) {   // drain to let the helper finish, but it is refused
            char sink[4096];
            ssize_t got = read(fds[0], sink, sizeof sink);
            if (got <= 0) { if (got < 0 && errno == EINTR) continue; break; }
            over = true;
            continue;
        }
        ssize_t got = read(fds[0], out + *n, cap - *n);
        if (got < 0) { if (errno == EINTR) continue; ok = false; break; }
        if (got == 0) break;
        *n += (size_t)got;
    }
    close(fds[0]);

    if (timed_out) kill(pid, SIGKILL);
    i32 status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (timed_out || !ok) return CLIP_FAILED;
    if (over) return CLIP_BIG;
    i32 code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 127) return CLIP_ABSENT;
    // A helper answers nonzero for a clipboard that holds nothing it can give.
    if (code != 0) return CLIP_NONE;
    return *n ? CLIP_OK : CLIP_NONE;
}

// The first type in the preference order that `listing` offers.
static b8 clip_pick_type(Str listing, Str *type) {
    for (size_t t = 0; t < sizeof k_clip_types / sizeof *k_clip_types; t++) {
        Str want = k_clip_types[t];
        for (size_t i = 0; i + want.n <= listing.n; i++) {
            if (memcmp(listing.p + i, want.p, want.n)) continue;
            // A whole line, so "image/png" does not match "ximage/png-ish".
            b8 head = i == 0 || listing.p[i - 1] == '\n'
                   || listing.p[i - 1] == '\r';
            size_t end = i + want.n;
            b8 tail = end == listing.n || listing.p[end] == '\n'
                   || listing.p[end] == '\r';
            if (head && tail) { *type = want; return true; }
        }
    }
    return false;
}

b8 clipboard_image(Arena *scratch, Str *out, char *err, size_t err_cap) {
    *out = (Str){0};
    /* One byte over the limit, so bytes that cannot be sent are recognized
     * as too many rather than read as a truncated image. */
    size_t cap = (size_t)AGENT_MAX_IMAGE_BYTES + 1;
    char *buf = arena_alloc(scratch, cap, 1);
    char *listing = arena_alloc(scratch, CLIP_LIST_MAX, 1);
    if (!buf || !listing) {
        snprintf(err, err_cap, "not enough memory to read the clipboard");
        return false;
    }

    b8 present = false, over = false;
    for (size_t h = 0; h < sizeof k_clip / sizeof *k_clip; h++) {
        Str type = k_clip_types[0];
        if (k_clip[h].list[0]) {
            size_t got = 0;
            ClipStatus st = clip_exec(k_clip[h].list, (Str){0}, listing,
                                      CLIP_LIST_MAX, &got);
            if (st == CLIP_ABSENT) continue;
            present = true;
            if (st != CLIP_OK || !clip_pick_type((Str){ listing, got }, &type))
                continue;
        }
        size_t got = 0;
        ClipStatus st = clip_exec(k_clip[h].fetch, type, buf, cap, &got);
        if (st == CLIP_ABSENT) continue;
        present = true;
        if (st == CLIP_BIG) { over = true; continue; }
        if (st != CLIP_OK) continue;
        *out = (Str){ buf, got };
        return true;
    }

    if (over) {
        char max[32];
        spill_size_text(max, sizeof max, AGENT_MAX_IMAGE_BYTES);
        snprintf(err, err_cap, "the clipboard image is over %s", max);
    } else if (present) {
        snprintf(err, err_cap, "the clipboard holds no image");
    } else {
        snprintf(err, err_cap, "no clipboard reader found: install %s",
                 "wl-paste, xclip or pngpaste");
    }
    return false;
}
