/* prompt.c: the system prompt, as a template with placeholders.
 *
 * The prompt is a document, so it comes from one place at a time rather than
 * being assembled from layers: --system or YOKE_SYSTEM_PROMPT if either is
 * set, else the project's .yoke/SYSTEM.md found by walking up from the
 * working directory, else the global $XDG_CONFIG_HOME/yoke/SYSTEM.md, else
 * the built-in template below.
 *
 * Whichever it is, it is expanded before it is sent: what only startup knows
 * (the registered tools, the working directory) is written as a placeholder
 * and substituted here, so a prompt written months ago still describes the
 * tools that exist today. An unknown "{name}" is left exactly as written,
 * which keeps braces safe in a prompt that talks about JSON or C.
 *
 * A SYSTEM.md past YOKE_MAX_PROMPT_FILE is refused, not truncated: half a
 * prompt is a different prompt, and one that silently drops its last
 * paragraph is worse than one that never ran.
 *
 * AGENTS.md is the other half of this file and the opposite kind of thing:
 * not the operator's instructions to yoke but the project's, so it does not
 * compete with the prompt, it is appended to whichever prompt won. Every one
 * from the working directory up to the root applies rather than just the
 * nearest, since a subdirectory refines its parent instead of replacing it.
 */
#include "yoke.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char PROMPT_BUILTIN[] =
    "You are an expert coding assistant. You help users by advising, reading "
    "files, executing commands, editing code, and writing new files.\n"
    "\n"
    "Available tools:\n"
    "{tools}\n"
    "In addition to the tools above, you may have access to other custom "
    "tools depending on the project.\n"
    "\n"
    "Guidelines:\n"
    "- Use bash for file operations like ls, rg, find\n"
    "- Use read to examine files instead of cat or sed\n"
    "- Inspect environment variables for current model and session details "
    "when relevant\n"
    "- Make precise edits using exact text replacement; keep each edit target "
    "as small as possible while remaining unique in the file\n"
    "- When changing multiple separate locations in one file, batch them into "
    "a single edit call with multiple entries instead of multiple calls\n"
    "- Use write only for new files or complete rewrites\n"
    "- Be concise in responses\n"
    "- Show file paths clearly when working with files\n"
    "\n"
    "Current working directory: {cwd}\n";

/* Reads `path` into `a`, empty when it does not exist or holds only space.
 * A file past the limit sets `err` and reads nothing, which stops the search
 * rather than falling through to a prompt the user did not ask for. */
static Str prompt_read(Str path, Arena *a, char *err, size_t err_cap) {
    if (!path.n || path.n >= YOKE_MAX_PATH) return (Str){0};
    FILE *f = fopen(path.p, "rb");
    if (!f) return (Str){0};
    fseek(f, 0, SEEK_END);
    i64 sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return (Str){0}; }
    if ((u64)sz > YOKE_MAX_PROMPT_FILE) {
        fclose(f);
        snprintf(err, err_cap,
                 "%.*s is %lld bytes, over the %d byte system prompt limit",
                 (int)path.n, path.p, (long long)sz, (int)YOKE_MAX_PROMPT_FILE);
        return (Str){0};
    }
    char *buf = arena_new(a, char, (size_t)sz);
    if (!buf) { fclose(f); return (Str){0}; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return str_trim((Str){ buf, n });
}

/* The SYSTEM.md of the nearest ancestor of `dir` that has one. Git's rule:
 * the project root is wherever the marker is, not where yoke started. */
static Str prompt_project(Str dir, Arena *scratch, char *err, size_t err_cap) {
    static const char suffix[] = "/.yoke/SYSTEM.md";
    char path[YOKE_MAX_PATH];
    size_t n = dir.n;
    if (!n || dir.p[0] != '/' || n + sizeof suffix > sizeof path)
        return (Str){0};
    memcpy(path, dir.p, n);
    while (n > 1 && path[n - 1] == '/') n--;
    for (;;) {
        /* At the root the directory is the separator the suffix carries. */
        size_t off = n == 1 ? 0 : n;
        memcpy(path + off, suffix, sizeof suffix);
        Str body = prompt_read((Str){ path, off + sizeof suffix - 1 }, scratch,
                               err, err_cap);
        if (body.n || *err) return body;
        if (n == 1) return (Str){0};
        while (n > 1 && path[n - 1] != '/') n--;
        while (n > 1 && path[n - 1] == '/') n--;
    }
}

/* The highest precedence SYSTEM.md that exists: the user's config dir first,
 * then the XDG_CONFIG_DIRS entries below it. */
static Str prompt_global(Arena *scratch, char *err, size_t err_cap) {
    Str cand[YOKE_MAX_CONFIG_FILES];
    size_t n = paths_config_files(STR("SYSTEM.md"), scratch, cand,
                                  YOKE_MAX_CONFIG_FILES);
    for (size_t i = n; i > 0; i--) {
        Str body = prompt_read(cand[i - 1], scratch, err, err_cap);
        if (body.n || *err) return body;
    }
    return (Str){0};
}

/* Every AGENTS.md at or above `dir`, nearest first, as body/path pairs.
 * Past YOKE_MAX_AGENTS_FILES the outermost are the ones dropped: the nearest
 * statement is the one that describes the code being worked on. */
static size_t prompt_agents(Str dir, Arena *scratch, Str *body, Str *path_out,
                            size_t cap, char *err, size_t err_cap) {
    static const char suffix[] = "/AGENTS.md";
    char path[YOKE_MAX_PATH];
    size_t n = dir.n, found = 0;
    if (!n || dir.p[0] != '/' || n + sizeof suffix > sizeof path) return 0;
    memcpy(path, dir.p, n);
    while (n > 1 && path[n - 1] == '/') n--;
    for (;;) {
        size_t off = n == 1 ? 0 : n;
        memcpy(path + off, suffix, sizeof suffix);
        Str full = { path, off + sizeof suffix - 1 };
        Str text = prompt_read(full, scratch, err, err_cap);
        if (*err) return found;
        if (text.n && found < cap) {
            Str p = str_dup(scratch, full);
            if (!p.p) return found;
            body[found] = text;
            path_out[found] = p;
            found++;
        }
        if (n == 1) return found;
        while (n > 1 && path[n - 1] != '/') n--;
        while (n > 1 && path[n - 1] == '/') n--;
    }
}

static void prompt_tools(Buf *b, const ToolRegistry *tools) {
    if (!tools) return;
    for (size_t i = 0; i < tools->n; i++)
        buf_putf(b, "- %.*s: %.*s\n",
                 (int)tools->name[i].n, tools->name[i].p,
                 (int)tools->desc[i].n, tools->desc[i].p);
}

/* Substitutes the placeholders of `tmpl` into `b`. */
static void prompt_expand(Buf *b, Str tmpl, const ToolRegistry *tools,
                          Str cwd) {
    for (size_t i = 0; i < tmpl.n; i++) {
        if (tmpl.p[i] != '{') { buf_putc(b, tmpl.p[i]); continue; }
        size_t end = i + 1;
        while (end < tmpl.n && tmpl.p[end] != '}' && tmpl.p[end] != '\n') end++;
        Str name = { tmpl.p + i + 1, end - i - 1 };
        if (end == tmpl.n || tmpl.p[end] != '}') { buf_putc(b, '{'); continue; }
        if (str_eq(name, STR("tools")))    prompt_tools(b, tools);
        else if (str_eq(name, STR("cwd"))) buf_puts(b, cwd);
        else { buf_putc(b, '{'); continue; }
        i = end;
    }
}

Str prompt_build(const ToolRegistry *tools, Str configured, Arena *persist,
                 Arena *scratch, char *err, size_t err_cap) {
    char cwd_buf[YOKE_MAX_PATH];
    Str cwd = getcwd(cwd_buf, sizeof cwd_buf) ? str_c(cwd_buf) : (Str){0};

    if (err_cap) err[0] = '\0';
    Str tmpl = configured;
    if (!tmpl.p) tmpl = prompt_project(cwd, scratch, err, err_cap);
    if (!tmpl.n && !*err) tmpl = prompt_global(scratch, err, err_cap);
    if (*err) return (Str){0};
    if (!tmpl.n) tmpl = str_c(PROMPT_BUILTIN);

    Str agents[YOKE_MAX_AGENTS_FILES], agent_paths[YOKE_MAX_AGENTS_FILES];
    size_t n_agents = prompt_agents(cwd, scratch, agents, agent_paths,
                                    YOKE_MAX_AGENTS_FILES, err, err_cap);
    if (*err) return (Str){0};

    size_t extra = 1024;
    for (size_t i = 0; i < n_agents; i++)
        extra += agents[i].n + agent_paths[i].n + 64;

    Buf b;
    buf_init(&b, persist, tmpl.n + extra);
    prompt_expand(&b, tmpl, tools, cwd);
    if (n_agents) {
        buf_puts(&b, STR("\n\nProject-specific instructions and "
                         "guidelines:\n"));
        for (size_t i = n_agents; i > 0; i--)
            buf_putf(&b, "\n<project_instructions path=\"%.*s\">\n%.*s\n"
                         "</project_instructions>\n",
                     (int)agent_paths[i - 1].n, agent_paths[i - 1].p,
                     (int)agents[i - 1].n, agents[i - 1].p);
    }
    if (!buf_ok(&b)) return tmpl;
    return buf_finish(&b);
}
