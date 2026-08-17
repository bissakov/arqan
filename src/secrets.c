#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SECRET_POLL_MS   50
#define SECRET_SERVICE_ID AGENT_NAME

typedef enum { SECRET_OP_LOOKUP, SECRET_OP_STORE, SECRET_OP_ERASE } SecretOp;

/* argv plus the storage the built strings point into, so a command lives on
 * one frame and needs no arena. */
typedef struct {
    const char *argv[AGENT_MAX_SECRET_ARGV + 1];
    size_t n;
    char account[AGENT_MAX_ENDPOINT_NAME + 1];
    char path[AGENT_MAX_ENDPOINT_NAME + 8];    
    char label[AGENT_MAX_ENDPOINT_NAME + 16];  
    char words[AGENT_MAX_SECRET_CMD + 1];      
} SecretCmd;


static const char *const g_secret_names[] = {
    "file", "secret-service", "pass", "keychain", "command",
};
#define SECRET_SOURCE_N (sizeof g_secret_names / sizeof g_secret_names[0])

SecretSource secret_source_from_str(Str s, b8 *known) {
    s = str_trim(s);
    if (known) *known = true;
    if (!s.n) return SECRET_STORED;
    for (size_t i = 0; i < SECRET_SOURCE_N; i++)
        if (str_eq(s, str_c(g_secret_names[i]))) return (SecretSource)i;
    if (known) *known = false;
    return SECRET_STORED;
}

Str secret_source_name(SecretSource src) {
    size_t i = (size_t)src;
    return str_c(i < SECRET_SOURCE_N ? g_secret_names[i] : g_secret_names[0]);
}

b8 secret_source_external(SecretSource src) { return src != SECRET_STORED; }

/* keychain and command are lookup-only: security(1) takes the secret on argv
 * rather than stdin, and a user's own command is not arqan's to write to. */
b8 secret_source_can_store(SecretSource src) {
    return src == SECRET_STORED || src == SECRET_SERVICE || src == SECRET_PASS;
}


static b8 secret_account_ok(Str name) {
    if (!name.n || name.n > AGENT_MAX_ENDPOINT_NAME) return false;
    if (name.p[0] == '-' || name.p[0] == '.') return false;
    for (size_t i = 0; i < name.n; i++) {
        u8 c = (u8)name.p[i];
        if (c < 0x20 || c == 0x7f || c == '/') return false;
    }
    return true;
}

static void secret_arg(SecretCmd *c, const char *z) {
    if (c->n < AGENT_MAX_SECRET_ARGV) c->argv[c->n++] = z;
}


static b8 secret_build(SecretCmd *c, SecretSource src, SecretOp op,
                       Str account, char *err, size_t err_cap) {
    memset(c, 0, sizeof *c);
    if (!secret_account_ok(account)) {
        snprintf(err, err_cap, "provider name must be letters, digits, '.', "
                 "'_' or '-' to use an external key store");
        return false;
    }
    memcpy(c->account, account.p, account.n);
    snprintf(c->path, sizeof c->path, "%s/%s", SECRET_SERVICE_ID, c->account);
    snprintf(c->label, sizeof c->label, "%s: %s", SECRET_SERVICE_ID, c->account);

    switch (src) {
        case SECRET_SERVICE:
            secret_arg(c, "secret-tool");
            if (op == SECRET_OP_LOOKUP) secret_arg(c, "lookup");
            else if (op == SECRET_OP_ERASE) secret_arg(c, "clear");
            else { secret_arg(c, "store"); secret_arg(c, "--label"); secret_arg(c, c->label); }
            secret_arg(c, "service"); secret_arg(c, SECRET_SERVICE_ID);
            secret_arg(c, "account"); secret_arg(c, c->account);
            break;
        case SECRET_PASS:
            secret_arg(c, "pass");
            if (op == SECRET_OP_LOOKUP) secret_arg(c, "show");
            else if (op == SECRET_OP_ERASE) { secret_arg(c, "rm"); secret_arg(c, "--force"); }
            else { secret_arg(c, "insert"); secret_arg(c, "--multiline"); }
            secret_arg(c, c->path);
            break;
        case SECRET_KEYCHAIN:
            secret_arg(c, "security");
            secret_arg(c, op == SECRET_OP_ERASE ? "delete-generic-password"
                                                : "find-generic-password");
            secret_arg(c, "-s"); secret_arg(c, SECRET_SERVICE_ID);
            secret_arg(c, "-a"); secret_arg(c, c->account);
            if (op == SECRET_OP_LOOKUP) secret_arg(c, "-w");
            break;
        case SECRET_STORED:
        case SECRET_COMMAND:
            snprintf(err, err_cap, "no built-in helper for this key source");
            return false;
    }
    c->argv[c->n] = NULL;
    return true;
}


static b8 secret_split(SecretCmd *c, Str command, char *err, size_t err_cap) {
    memset(c, 0, sizeof *c);
    Str s = str_trim(command);
    if (!s.n) { snprintf(err, err_cap, "key_command is empty"); return false; }
    if (s.n > AGENT_MAX_SECRET_CMD) {
        snprintf(err, err_cap, "key_command is longer than %d bytes",
                 AGENT_MAX_SECRET_CMD);
        return false;
    }
    if (memchr(s.p, '\0', s.n)) {
        snprintf(err, err_cap, "key_command contains a nul byte");
        return false;
    }
    memcpy(c->words, s.p, s.n);
    size_t i = 0;
    while (i < s.n) {
        while (i < s.n && (c->words[i] == ' ' || c->words[i] == '\t'))
            c->words[i++] = '\0';
        if (i >= s.n) break;
        if (c->n >= AGENT_MAX_SECRET_ARGV) {
            snprintf(err, err_cap, "key_command has more than %d words",
                     AGENT_MAX_SECRET_ARGV);
            return false;
        }
        c->argv[c->n++] = c->words + i;
        while (i < s.n && c->words[i] != ' ' && c->words[i] != '\t') i++;
    }
    c->argv[c->n] = NULL;
    return c->n > 0;
}


static i32 secret_wait(i32 fd, i16 events, f64 deadline) {
    for (;;) {
        f64 left = deadline - agent_now_seconds();
        if (left <= 0) return 0;
        i32 ms = (i32)(left * 1000.0);
        if (ms > SECRET_POLL_MS) ms = SECRET_POLL_MS;
        if (ms < 1) ms = 1;
        struct pollfd pfd = { fd, events, 0 };
        i32 rc = poll(&pfd, 1, ms);
        tui_poll_input();
        if (rc < 0) { if (errno == EINTR) continue; return -1; }
        if (rc > 0) return 1;
    }
}

static void secret_reap(pid_t pid, i32 *status) {
    while (waitpid(pid, status, 0) < 0 && errno == EINTR) {}
}


static b8 secret_exec(const SecretCmd *c, Str input, char *out, size_t out_cap,
                      size_t *out_n, char *err, size_t err_cap) {
    *out_n = 0;
    i32 in_fds[2] = { -1, -1 }, out_fds[2] = { -1, -1 };
    if (pipe(out_fds) != 0) {
        snprintf(err, err_cap, "pipe failed");
        return false;
    }
    if (input.p && pipe(in_fds) != 0) {
        close(out_fds[0]); close(out_fds[1]);
        snprintf(err, err_cap, "pipe failed");
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(out_fds[0]); close(out_fds[1]);
        if (in_fds[0] >= 0) { close(in_fds[0]); close(in_fds[1]); }
        snprintf(err, err_cap, "fork failed");
        return false;
    }
    if (pid == 0) {
        i32 null_rd = input.p ? -1 : open("/dev/null", O_RDONLY);
        i32 null_wr = open("/dev/null", O_WRONLY);
        if (input.p) dup2(in_fds[0], STDIN_FILENO);
        else if (null_rd >= 0) dup2(null_rd, STDIN_FILENO);
        dup2(out_fds[1], STDOUT_FILENO);
        
        if (null_wr >= 0) dup2(null_wr, STDERR_FILENO);
        if (null_rd > STDERR_FILENO) close(null_rd);
        if (null_wr > STDERR_FILENO) close(null_wr);
        if (in_fds[0] >= 0) { close(in_fds[0]); close(in_fds[1]); }
        close(out_fds[0]); close(out_fds[1]);
        execvp(c->argv[0], (char *const *)(uintptr_t)c->argv);
        _exit(127);
    }
    close(out_fds[1]);
    if (in_fds[0] >= 0) close(in_fds[0]);

    f64 deadline = agent_now_seconds() + (f64)AGENT_SECRET_TIMEOUT_MS / 1000.0;
    b8 ok = true, timed_out = false;

    
    if (in_fds[1] >= 0) {
        struct sigaction oldpipe, ignore = {0};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        b8 have_old = sigaction(SIGPIPE, &ignore, &oldpipe) == 0;
        size_t off = 0;
        while (ok && off < input.n) {
            i32 rc = secret_wait(in_fds[1], POLLOUT, deadline);
            if (rc == 0) { timed_out = true; ok = false; break; }
            if (rc < 0) { ok = false; break; }
            ssize_t w = write(in_fds[1], input.p + off, input.n - off);
            if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
            off += (size_t)w;
        }
        if (ok && input.n && input.p[input.n - 1] != '\n') {
            ssize_t w = write(in_fds[1], "\n", 1);
            if (w < 0) ok = false;
        }
        close(in_fds[1]);
        if (have_old) sigaction(SIGPIPE, &oldpipe, NULL);
    }

    b8 overflow = false;
    while (ok) {
        i32 rc = secret_wait(out_fds[0], POLLIN, deadline);
        if (rc == 0) { timed_out = true; ok = false; break; }
        if (rc < 0) { ok = false; break; }
        if (*out_n >= out_cap) {   
            char sink[256];
            ssize_t n = read(out_fds[0], sink, sizeof sink);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
            overflow = true;
            continue;
        }
        ssize_t n = read(out_fds[0], out + *out_n, out_cap - *out_n);
        if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
        if (n == 0) break;
        *out_n += (size_t)n;
    }
    close(out_fds[0]);

    if (timed_out) kill(pid, SIGKILL);
    i32 status = 0;
    secret_reap(pid, &status);

    if (timed_out) {
        snprintf(err, err_cap, "%s did not answer within %d seconds",
                 c->argv[0], AGENT_SECRET_TIMEOUT_MS / 1000);
        return false;
    }
    if (!ok) { snprintf(err, err_cap, "%s could not be read", c->argv[0]); return false; }
    if (overflow) {
        snprintf(err, err_cap, "%s returned more than %d bytes",
                 c->argv[0], AGENT_MAX_API_KEY);
        return false;
    }
    if (WIFSIGNALED(status)) {
        snprintf(err, err_cap, "%s was killed by signal %d",
                 c->argv[0], WTERMSIG(status));
        return false;
    }
    i32 code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 127) {
        snprintf(err, err_cap, "%s is not installed", c->argv[0]);
        return false;
    }
    if (code != 0) {
        snprintf(err, err_cap, "%s exited %d", c->argv[0], code);
        return false;
    }
    return true;
}

/* The first line of stdout, with no control byte left in it: a key is one
 * line, and anything else is a helper reporting rather than answering. */
static b8 secret_first_line(char *buf, size_t n, Str *out, char *err,
                            size_t err_cap) {
    size_t end = 0;
    while (end < n && buf[end] != '\n') end++;
    Str line = str_trim((Str){ buf, end });
    if (!line.n) { snprintf(err, err_cap, "the key store returned nothing"); return false; }
    if (line.n > AGENT_MAX_API_KEY) {
        snprintf(err, err_cap, "the stored key is longer than %d bytes",
                 AGENT_MAX_API_KEY);
        return false;
    }
    for (size_t i = 0; i < line.n; i++) {
        u8 c = (u8)line.p[i];
        if (c < 0x20 || c == 0x7f) {
            snprintf(err, err_cap, "the stored key contains a control byte");
            return false;
        }
    }
    *out = line;
    return true;
}

Str secret_lookup(SecretSource src, Str account, Str command, Arena *out,
                  char *err, size_t err_cap) {
    if (err_cap) err[0] = '\0';
    SecretCmd c;
    if (src == SECRET_COMMAND) {
        if (!secret_split(&c, command, err, err_cap)) return (Str){0};
    } else if (!secret_build(&c, src, SECRET_OP_LOOKUP, account, err, err_cap)) {
        return (Str){0};
    }
    char buf[AGENT_MAX_API_KEY + 1];
    size_t n = 0;
    if (!secret_exec(&c, (Str){0}, buf, sizeof buf, &n, err, err_cap))
        return (Str){0};
    Str line;
    if (!secret_first_line(buf, n, &line, err, err_cap)) return (Str){0};
    Str key = str_dup(out, line);
    // The plaintext must not outlive this frame in a buffer nobody owns.
    memset(buf, 0, sizeof buf);
    if (!key.p) snprintf(err, err_cap, "out of memory reading the key");
    return key;
}

b8 secret_store(SecretSource src, Str account, Str key, char *err,
                size_t err_cap) {
    if (err_cap) err[0] = '\0';
    if (!secret_source_can_store(src)) {
        snprintf(err, err_cap, "%.*s keys are stored with its own tool, "
                 "not by arqan", (i32)secret_source_name(src).n,
                 secret_source_name(src).p);
        return false;
    }
    if (key.n > AGENT_MAX_API_KEY) return false;
    SecretCmd c;
    if (!secret_build(&c, src, SECRET_OP_STORE, account, err, err_cap))
        return false;
    char buf[64];
    size_t n = 0;
    return secret_exec(&c, key, buf, sizeof buf, &n, err, err_cap);
}

b8 secret_erase(SecretSource src, Str account, char *err, size_t err_cap) {
    if (err_cap) err[0] = '\0';
    if (!secret_source_external(src) || src == SECRET_COMMAND) return true;
    SecretCmd c;
    if (!secret_build(&c, src, SECRET_OP_ERASE, account, err, err_cap))
        return false;
    char buf[64];
    size_t n = 0;
    return secret_exec(&c, (Str){0}, buf, sizeof buf, &n, err, err_cap);
}
