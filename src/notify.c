#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    NOTIFY_OFF = 0,
    NOTIFY_BEL,
    NOTIFY_OSC9,
    NOTIFY_BOTH
} NotifyMode;

static struct {
    NotifyMode mode;
    Str command; // in the persist arena, or empty
    f64 min_ms;
} g_notify;

void notify_init(const Conf *c, Arena *persist) {
    memset(&g_notify, 0, sizeof g_notify);
    Str m = conf_str(c, CONF_NOTIFY);
    if (str_eq(m, STR("bel")))
        g_notify.mode = NOTIFY_BEL;
    else if (str_eq(m, STR("osc9")))
        g_notify.mode = NOTIFY_OSC9;
    else if (str_eq(m, STR("both")))
        g_notify.mode = NOTIFY_BOTH;
    g_notify.min_ms = (f64)conf_num(c, CONF_NOTIFY_MIN_MS);

    Str cmd = str_trim(conf_str(c, CONF_NOTIFY_COMMAND));
    if (cmd.n && cmd.n <= AGENT_MAX_NOTIFY_CMD && !memchr(cmd.p, '\0', cmd.n))
        g_notify.command = str_dup(persist, cmd);
}

static const char *kind_name(NotifyKind k) {
    switch (k) {
        case NOTIFY_TURN_DONE: return "turn-done";
        case NOTIFY_TURN_FAILED: return "turn-failed";
        case NOTIFY_INPUT_NEEDED: return "input-needed";
        case NOTIFY_INTERRUPTED: return "interrupted";
    }
    return "unknown";
}

static const char *kind_label(NotifyKind k) {
    switch (k) {
        case NOTIFY_TURN_DONE: return "done";
        case NOTIFY_TURN_FAILED: return "error";
        case NOTIFY_INPUT_NEEDED: return "waiting for you";
        case NOTIFY_INTERRUPTED: return "interrupted";
    }
    return "";
}

/* `in` is model or provider text, so it may hold escapes of its own: one
 * left in an OSC string could close it and let the rest run as commands.
 * Every control byte goes, runs of blanks collapse, and the tail is cut at
 * a UTF-8 boundary rather than mid-sequence. */
static Str sanitize(Str in, char *out, size_t cap) {
    size_t n = 0;
    b8 blank = true;
    for (size_t i = 0; i < in.n && n < cap; i++) {
        u8 ch = (u8)in.p[i];
        if (ch < 0x20 || ch == 0x7f) ch = ' ';
        if (ch == ' ') {
            if (blank) continue;
            blank = true;
        } else {
            blank = false;
        }
        out[n++] = (char)ch;
    }
    while (n && out[n - 1] == ' ') n--;
    return str_clip_utf8((Str){out, n}, cap);
}


static size_t json_put(char *dst, size_t cap, Str s) {
    size_t n = 0;
    for (size_t i = 0; i < s.n && n + 2 < cap; i++) {
        if (s.p[i] == '"' || s.p[i] == '\\') dst[n++] = '\\';
        dst[n++] = s.p[i];
    }
    return n;
}

/* Appends what fits and reports the new length; a truncated field loses its
 * tail rather than the object's shape, since the caller closes it after. */
static size_t lit_put(char *dst, size_t cap, size_t n, const char *z) {
    size_t len = strlen(z);
    if (n + len >= cap) return n;
    memcpy(dst + n, z, len);
    return n + len;
}


static void run_command(NotifyKind kind, Str text) {
    if (!g_notify.command.n) return;

    char words[AGENT_MAX_NOTIFY_CMD + 1];
    const char *argv[AGENT_MAX_NOTIFY_ARGV + 1];
    size_t argc = 0;
    memcpy(words, g_notify.command.p, g_notify.command.n);
    words[g_notify.command.n] = '\0';
    /* No quoting and no escapes, as key_command does it: a line that needs
     * either wants a shell, and a wrapper script is the answer instead. */
    for (size_t i = 0; i < g_notify.command.n;) {
        while (i < g_notify.command.n && (words[i] == ' ' || words[i] == '\t'))
            words[i++] = '\0';
        if (i >= g_notify.command.n) break;
        if (argc >= AGENT_MAX_NOTIFY_ARGV) return;
        argv[argc++] = words + i;
        while (i < g_notify.command.n && words[i] != ' ' && words[i] != '\t')
            i++;
    }
    if (!argc) return;
    argv[argc] = NULL;


    char payload[1024];
    char cwd[512];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
    Str dir = {cwd, strlen(cwd)};
    size_t cap = sizeof payload - 8;
    size_t n = lit_put(payload, cap, 0, "{\"kind\":\"");
    n = lit_put(payload, cap, n, kind_name(kind));
    n = lit_put(payload, cap, n, "\",\"text\":\"");
    n += json_put(payload + n, cap - n, text);
    n = lit_put(payload, cap, n, "\",\"cwd\":\"");
    n += json_put(payload + n, cap - n, dir);
    n = lit_put(payload, sizeof payload, n, "\"}\n");

    i32 fds[2];
    if (pipe(fds) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        if (fork() == 0) {
            i32 null_wr = open("/dev/null", O_WRONLY);
            dup2(fds[0], STDIN_FILENO);

            if (null_wr >= 0) {
                dup2(null_wr, STDOUT_FILENO);
                dup2(null_wr, STDERR_FILENO);
                if (null_wr > STDERR_FILENO) close(null_wr);
            }
            close(fds[0]);
            close(fds[1]);
            execvp(argv[0], (char *const *)(uintptr_t)argv);
            _exit(127);
        }
        _exit(0);
    }
    close(fds[0]);

    struct sigaction oldpipe, ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ignore, &oldpipe);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fds[1], payload + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        break;
    }
    sigaction(SIGPIPE, &oldpipe, NULL);
    close(fds[1]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

void notify_event(NotifyKind kind, Str detail, f64 elapsed_ms) {
    if (kind == NOTIFY_TURN_DONE && elapsed_ms < g_notify.min_ms) return;

    char body[AGENT_MAX_NOTIFY_TEXT];
    Str said = sanitize(detail, body, sizeof body);

    char line[AGENT_MAX_NOTIFY_TEXT];

    i32 len = said.n ? snprintf(line, sizeof line, AGENT_NAME ": %.*s",
                                (i32)said.n, said.p)
                     : snprintf(line, sizeof line, AGENT_NAME ": %s",
                                kind_label(kind));
    if (len < 0) return;
    Str msg = str_clip_utf8(
        (Str){line, (size_t)len < sizeof line ? (size_t)len : sizeof line - 1},
        sizeof line - 1);

    if (g_notify.mode == NOTIFY_OSC9 || g_notify.mode == NOTIFY_BOTH)
        tui_desktop_notify(msg);
    if (g_notify.mode == NOTIFY_BEL || g_notify.mode == NOTIFY_BOTH) tui_bell();
    run_command(kind, msg);
}
