/* paths.c: XDG Base Directory resolution.
 *
 * Two rules the spec is explicit about and this file enforces: a relative
 * value in an XDG_* variable is invalid and falls back to the default as if
 * unset, and directories are created with mode 0700.
 */
#include "yoke.h"

#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* $HOME, or the passwd entry when it is unset or relative. Empty when both
 * fail, which leaves callers without a writable base. */
static Str paths_home(void) {
    const char *h = getenv("HOME");
    if (!h || h[0] != '/') {
        struct passwd *pw = getpwuid(getuid());
        h = pw ? pw->pw_dir : NULL;
    }
    if (!h || h[0] != '/') return (Str){0};
    return str_c(h);
}

static const char *dir_env(YokeDir kind) {
    switch (kind) {
        case YOKE_DIR_CONFIG: return "XDG_CONFIG_HOME";
        case YOKE_DIR_DATA:   return "XDG_DATA_HOME";
        case YOKE_DIR_STATE:  return "XDG_STATE_HOME";
        case YOKE_DIR_CACHE:  return "XDG_CACHE_HOME";
    }
    return "";
}

static Str dir_default(YokeDir kind) {
    switch (kind) {
        case YOKE_DIR_CONFIG: return STR(".config");
        case YOKE_DIR_DATA:   return STR(".local/share");
        case YOKE_DIR_STATE:  return STR(".local/state");
        case YOKE_DIR_CACHE:  return STR(".cache");
    }
    return (Str){0};
}

/* Base directory for `kind`, without the "yoke" component. */
static Str paths_base(YokeDir kind, Arena *a) {
    const char *env = getenv(dir_env(kind));
    if (env && env[0] == '/') {   /* a relative value is ignored */
        Str s = str_c(env);
        while (s.n > 1 && s.p[s.n - 1] == '/') s.n--;
        return str_dup(a, s);
    }
    Str home = paths_home();
    if (!home.n) return (Str){0};
    Str def = dir_default(kind);
    Buf b; buf_init(&b, a, home.n + def.n + 2);
    buf_puts(&b, home);
    buf_putc(&b, '/');
    buf_puts(&b, def);
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

Str paths_dir(YokeDir kind, Arena *a) {
    Str base = paths_base(kind, a);
    if (!base.n) return (Str){0};
    Buf b; buf_init(&b, a, base.n + 6);
    buf_puts(&b, base);
    buf_puts(&b, STR("/yoke"));
    if (!buf_ok(&b)) return (Str){0};
    Str out = buf_finish(&b);
    return out.n < YOKE_MAX_PATH ? out : (Str){0};
}

Str paths_file(YokeDir kind, Str name, Arena *a) {
    Str dir = paths_dir(kind, a);
    if (!dir.n || !name.n) return (Str){0};
    Buf b; buf_init(&b, a, dir.n + name.n + 2);
    buf_puts(&b, dir);
    buf_putc(&b, '/');
    buf_puts(&b, name);
    if (!buf_ok(&b)) return (Str){0};
    Str out = buf_finish(&b);
    return out.n < YOKE_MAX_PATH ? out : (Str){0};
}

/* mkdir -p with 0700, as the spec requires for created XDG directories.
 * mkdir reports EEXIST for a regular file too, so an existing component is
 * confirmed to be a directory: the caller's next open would otherwise fail
 * with ENOTDIR long after this said the path was ready. */
b8 paths_ensure_dir(Str dir) {
    if (!dir.n || dir.p[0] != '/' || dir.n >= YOKE_MAX_PATH) return false;
    char path[YOKE_MAX_PATH];
    memcpy(path, dir.p, dir.n);
    path[dir.n] = '\0';
    for (size_t i = 1; i <= dir.n; i++) {
        if (i != dir.n && path[i] != '/') continue;
        char saved = path[i];
        path[i] = '\0';
        if (mkdir(path, 0700) != 0) {
            struct stat st;
            if (errno != EEXIST) return false;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
        }
        path[i] = saved;
    }
    return true;
}

/* "<dir>/.yoke/<name>", empty when it would not fit a path buffer. */
static Str project_file(const char *dir, size_t n, Str name, Arena *a) {
    Buf b;
    buf_init(&b, a, n + name.n + YOKE_PROJECT_DIR.n + 3);
    buf_put(&b, dir, n);
    buf_putc(&b, '/');
    buf_puts(&b, YOKE_PROJECT_DIR);
    buf_putc(&b, '/');
    buf_puts(&b, name);
    if (!buf_ok(&b)) return (Str){0};
    Str out = buf_finish(&b);
    return out.n < YOKE_MAX_PATH ? out : (Str){0};
}

Str paths_project_dir(Arena *a) {
    char cwd[YOKE_MAX_PATH];
    if (!getcwd(cwd, sizeof cwd) || cwd[0] != '/') return (Str){0};
    size_t n = strlen(cwd);
    while (n > 1 && cwd[n - 1] == '/') n--;
    Buf b;
    buf_init(&b, a, n + YOKE_PROJECT_DIR.n + 2);
    buf_put(&b, cwd, n == 1 ? 0 : n);
    buf_putc(&b, '/');
    buf_puts(&b, YOKE_PROJECT_DIR);
    if (!buf_ok(&b)) return (Str){0};
    Str out = buf_finish(&b);
    return out.n < YOKE_MAX_PATH ? out : (Str){0};
}

/* The project chain, outermost first: the nearest file is applied last and
 * therefore wins, the way the nearest AGENTS.md does. Only files that exist
 * are returned, so a caller reads what it is given. */
size_t paths_project_files(Str name, Arena *a, Str *out, size_t max) {
    char cwd[YOKE_MAX_PATH];
    if (!out || max == 0 || !name.n) return 0;
    if (!getcwd(cwd, sizeof cwd) || cwd[0] != '/') return 0;
    if (max > YOKE_MAX_PROJECT_FILES) max = YOKE_MAX_PROJECT_FILES;

    Str near[YOKE_MAX_PROJECT_FILES];
    size_t found = 0, n = strlen(cwd);
    while (n > 1 && cwd[n - 1] == '/') n--;
    for (;;) {
        /* At the root the directory is the separator the suffix carries. */
        Str p = project_file(cwd, n == 1 ? 0 : n, name, a);
        struct stat st;
        if (p.n && stat(p.p, &st) == 0 && S_ISREG(st.st_mode)) {
            near[found++] = p;
            if (found == max) break;
        }
        if (n == 1) break;
        while (n > 1 && cwd[n - 1] != '/') n--;
        while (n > 1 && cwd[n - 1] == '/') n--;
    }
    for (size_t i = 0; i < found; i++) out[i] = near[found - 1 - i];
    return found;
}

/* Candidates for `name` in increasing precedence: the XDG_CONFIG_DIRS
 * entries first, XDG_CONFIG_HOME last. */
size_t paths_config_files(Str name, Arena *a, Str *out, size_t max) {
    if (!out || max == 0 || !name.n) return 0;
    const char *dirs = getenv("XDG_CONFIG_DIRS");
    if (!dirs || !*dirs) dirs = "/etc/xdg";

    /* XDG_CONFIG_DIRS runs highest first, so collect it and then reverse. */
    Str sys[8];
    size_t sys_n = 0;
    Str all = str_c(dirs);
    size_t start = 0;
    for (size_t i = 0; i <= all.n && sys_n < 8; i++) {
        if (i != all.n && all.p[i] != ':') continue;
        Str d = { all.p + start, i - start };
        start = i + 1;
        while (d.n > 1 && d.p[d.n - 1] == '/') d.n--;
        if (!d.n || d.p[0] != '/') continue;   /* relative entries: ignored */
        Buf b; buf_init(&b, a, d.n + name.n + 8);
        buf_puts(&b, d);
        buf_puts(&b, STR("/yoke/"));
        buf_puts(&b, name);
        if (!buf_ok(&b)) continue;
        Str p = buf_finish(&b);
        if (p.n < YOKE_MAX_PATH) sys[sys_n++] = p;
    }

    size_t n = 0;
    for (size_t i = sys_n; i > 0 && n < max; i--) out[n++] = sys[i - 1];
    Str user = paths_file(YOKE_DIR_CONFIG, name, a);
    if (user.n && n < max) out[n++] = user;
    return n;
}
