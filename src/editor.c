/* editor.c: the composer draft, edited in vi, vim or nvim.
 *
 * The draft goes to a 0600 file in a private temporary directory, the
 * terminal is handed to the editor, and the file is read back after it exits.
 * Only the vi family is supported, so a cursor position and a non-zero exit
 * (`:cq`) mean the same thing in every editor this runs.
 */
#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


#define EDITOR_WORDS_MAX 16
#define EDITOR_NO_EXEC   127
#define EDITOR_FILE      "draft.md"

static const char *const k_vims[] = {"nvim", "vim", "vi"};

typedef struct {
    char text[AGENT_MAX_PATH];
    char *word[EDITOR_WORDS_MAX];
    size_t n;
} EditorCmd;

typedef struct {
    char dir[AGENT_MAX_PATH];
    char path[AGENT_MAX_PATH];
} EditorFile;


static const char *editor_base(const char *word) {
    const char *slash = strrchr(word, '/');
    return slash ? slash + 1 : word;
}

static b8 editor_is_vim(const char *word) {
    const char *base = editor_base(word);
    for (size_t i = 0; i < sizeof k_vims / sizeof *k_vims; i++)
        if (!strcmp(base, k_vims[i])) return true;
    return false;
}

static b8 editor_split(const char *value, EditorCmd *cmd) {
    cmd->n = 0;
    if (!value) return false;
    size_t len = strlen(value);
    if (len >= sizeof cmd->text) return false;
    memcpy(cmd->text, value, len + 1);
    for (char *p = cmd->text; *p;) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (!*p) break;
        if (cmd->n + 1 == EDITOR_WORDS_MAX) return false;
        cmd->word[cmd->n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return cmd->n && editor_is_vim(cmd->word[0]);
}

static b8 editor_from_env(EditorCmd *cmd) {
    return editor_split(getenv("VISUAL"), cmd)
           || editor_split(getenv("EDITOR"), cmd);
}


static void editor_cursor(Str text, size_t cursor, size_t *line, size_t *col) {
    if (cursor > text.n) cursor = text.n;
    size_t start = 0;
    *line = 1;
    for (size_t i = 0; i < cursor; i++)
        if (text.p[i] == '\n') {
            (*line)++;
            start = i + 1;
        }
    *col = cursor - start + 1;
}

static void editor_exec(const char *const *lead, size_t lead_n,
                        const char *vi_arg, const char *vim_arg,
                        const char *path) {
    const char *argv[EDITOR_WORDS_MAX + 3];
    size_t n = 0;
    for (size_t i = 0; i < lead_n; i++) argv[n++] = lead[i];
    argv[n++] = strcmp(editor_base(lead[0]), "vi") ? vim_arg : vi_arg;
    argv[n++] = path;
    argv[n] = NULL;
    execvp(argv[0], (char *const *)(uintptr_t)argv);
}

static i32 editor_run(const EditorCmd *cmd, Str text, size_t cursor,
                      const char *path, char *err, size_t err_cap) {
    size_t line, col;
    editor_cursor(text, cursor, &line, &col);
    char vi_arg[32], vim_arg[64];
    snprintf(vi_arg, sizeof vi_arg, "+%zu", line);
    snprintf(vim_arg, sizeof vim_arg, "+call cursor(%zu,%zu)", line, col);

    char **envp = child_env(CHILD_ENV_PLAIN);
    tui_suspend();
    pid_t pid = fork();
    if (pid < 0) {
        tui_resume();
        snprintf(err, err_cap, "could not start the editor: %s",
                 strerror(errno));
        return -1;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        child_close_fds(3);
        environ = envp;
        if (cmd->n)
            editor_exec((const char *const *)cmd->word, cmd->n, vi_arg, vim_arg,
                        path);
        for (size_t i = 0; i < sizeof k_vims / sizeof *k_vims; i++)
            editor_exec(&k_vims[i], 1, vi_arg, vim_arg, path);
        _exit(EDITOR_NO_EXEC);
    }
    i32 status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    tui_resume();

    if (WIFSIGNALED(status)) {
        snprintf(err, err_cap,
                 "the editor stopped on signal %d; the draft is unchanged",
                 WTERMSIG(status));
        return -1;
    }
    i32 code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == EDITOR_NO_EXEC) {
        snprintf(err, err_cap,
                 "no supported editor found on PATH; the draft is unchanged");
        return -1;
    }
    if (code != 0) {
        snprintf(err, err_cap,
                 "the editor exited with status %d; the draft is unchanged",
                 code);
        return -1;
    }
    return 0;
}


static b8 editor_write_all(i32 fd, Str text) {
    size_t off = 0;
    while (off < text.n) {
        ssize_t w = write(fd, text.p + off, text.n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

static b8 editor_file_open(EditorFile *f, Str text, char *err, size_t err_cap) {
    Str dir = spill_dir();
    i32 n = snprintf(f->dir, sizeof f->dir, "%.*s/" AGENT_NAME "-draft-XXXXXX",
                     (i32)dir.n, dir.p);
    f->path[0] = '\0';
    if (n <= 0 || (size_t)n >= sizeof f->dir || !mkdtemp(f->dir)) {
        f->dir[0] = '\0';
        snprintf(err, err_cap, "could not make a directory for the draft");
        return false;
    }
    n = snprintf(f->path, sizeof f->path, "%s/" EDITOR_FILE, f->dir);
    i32 fd =
        n > 0 && (size_t)n < sizeof f->path
            ? open(f->path,
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)
            : -1;
    if (fd < 0) {
        f->path[0] = '\0';
        snprintf(err, err_cap, "could not write the draft to a file");
        return false;
    }
    b8 wrote = editor_write_all(fd, text);
    if (close(fd) != 0) wrote = false;
    if (!wrote) snprintf(err, err_cap, "could not write the draft to a file");
    return wrote;
}

static void editor_file_drop(EditorFile *f) {
    if (f->path[0]) unlink(f->path);
    if (f->dir[0]) rmdir(f->dir);
}


EditorResult editor_edit(Str text, size_t cursor, Arena *scratch, Str *out,
                         char *err, size_t err_cap) {
    *out = (Str){0};
    EditorCmd cmd;
    if (!editor_from_env(&cmd)) cmd.n = 0;

    EditorFile file;
    if (!editor_file_open(&file, text, err, err_cap)) {
        editor_file_drop(&file);
        return EDITOR_FAILED;
    }
    if (editor_run(&cmd, text, cursor, file.path, err, err_cap) != 0) {
        editor_file_drop(&file);
        return EDITOR_FAILED;
    }

    Str saved;
    FileStatus st =
        file_read(scratch, file.path, AGENT_LINE_BUF, 0, &saved, NULL);
    editor_file_drop(&file);
    if (st == FILE_TOO_LARGE) return EDITOR_TOO_BIG;
    if (st != FILE_OK) {
        snprintf(err, err_cap,
                 "could not read the saved draft back; the draft is unchanged");
        return EDITOR_FAILED;
    }
    if (saved.n && saved.p[saved.n - 1] == '\n' && !str_eq(saved, text))
        saved.n--;
    if (str_eq(saved, text)) return EDITOR_UNCHANGED;
    *out = saved;
    return EDITOR_SAVED;
}
