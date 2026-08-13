#include "agent.h"

#include <fcntl.h>
#include <fnmatch.h>
#include <string.h>
#include <unistd.h>

enum { AGIG_NEG = 1, AGIG_DIRONLY = 2, AGIG_PATHNAME = 4 };

static b8 g_show_ignored;

void agent_ignore_set_show(b8 on) { g_show_ignored = on; }
b8 agent_ignore_show(void) { return g_show_ignored; }

static void agent_ig_add(AgentIgnore *ig, Str pat, size_t base_n) {
    pat = str_trim(pat);
    if (!pat.n || pat.p[0] == '#') return;
    u8 flags = 0;
    if (pat.p[0] == '!') { flags |= AGIG_NEG; pat = str_drop(pat, 1); }
    if (pat.n && pat.p[pat.n - 1] == '/') { flags |= AGIG_DIRONLY; pat.n--; }
    if (pat.n && pat.p[0] == '/') {
        flags |= AGIG_PATHNAME;
        pat = str_drop(pat, 1);
    } else {
        for (size_t i = 0; i + 1 < pat.n; i++)
            if (pat.p[i] == '/') { flags |= AGIG_PATHNAME; break; }
    }
    if (!pat.n || base_n > UINT16_MAX || ig->n >= AGENT_IGNORE_PATTERNS)
        return;
    if (ig->buf_n + pat.n + 1 > sizeof ig->buf) return;
    char *slot = ig->buf + ig->buf_n;
    memcpy(slot, pat.p, pat.n);
    slot[pat.n] = '\0';
    ig->buf_n += pat.n + 1;
    ig->pat[ig->n] = slot;
    ig->flag[ig->n] = flags;
    ig->base[ig->n] = (u16)base_n;
    ig->n++;
}

static void agent_ig_load(AgentIgnore *ig, const char *path, size_t base_n) {
    i32 fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[AGENT_IGNORE_BYTES];
    ssize_t got = read(fd, buf, sizeof buf);
    (void)close(fd);
    if (got <= 0) return;
    size_t n = (size_t)got, start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i != n && buf[i] != '\n') continue;
        agent_ig_add(ig, (Str){ buf + start, i - start }, base_n);
        start = i + 1;
    }
}

void agent_ignore_push(AgentIgnore *ig, const char *dir, size_t dir_n,
                       size_t path_n) {
    static const char *const names[] = { ".gitignore", ".ignore" };
    char path[AGENT_MAX_PATH];
    if (dir_n + 12 >= sizeof path) return;
    if (dir_n) memcpy(path, dir, dir_n);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        memcpy(path + dir_n, names[i], strlen(names[i]) + 1);
        agent_ig_load(ig, path, path_n);
    }
}

void agent_ignore_build(AgentIgnore *ig, Str dir) {
    ig->n = 0;
    ig->buf_n = 0;
    if (dir.n && dir.p[0] == '/') return;
    for (size_t base = 0;;) {
        agent_ignore_push(ig, dir.p, base, base);
        if (base >= dir.n) break;
        while (base < dir.n && dir.p[base] != '/') base++;
        if (base < dir.n) base++;
    }
}

b8 agent_ignore_match(const AgentIgnore *ig, const char *rel, size_t rel_n,
                      b8 is_dir) {
    b8 ignored = false;
    for (size_t i = 0; i < ig->n; i++) {
        u8 f = ig->flag[i];
        if ((f & AGIG_DIRONLY) && !is_dir) continue;
        if (ig->base[i] > rel_n) continue;
        const char *sub = rel + ig->base[i];
        if (!(f & AGIG_PATHNAME)) {
            const char *slash = strrchr(sub, '/');
            if (slash) sub = slash + 1;
        }
        i32 flags = f & AGIG_PATHNAME ? FNM_PATHNAME : 0;
        if (fnmatch(ig->pat[i], sub, flags) == 0)
            ignored = !(f & AGIG_NEG);
    }
    return ignored;
}

AgentIgnoreMark agent_ignore_mark(const AgentIgnore *ig) {
    return (AgentIgnoreMark){ ig->n, ig->buf_n };
}

void agent_ignore_restore(AgentIgnore *ig, AgentIgnoreMark mark) {
    ig->n = mark.n;
    ig->buf_n = mark.buf_n;
}
