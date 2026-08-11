/* prompt.c: the system prompt, as a template with placeholders.
 *
 * A prompt is a document, so it comes from one place at a time rather than
 * from layers: --system or ARQAN_SYSTEM_PROMPT, else the project's
 * .arqan/SYSTEM.md found by walking up from the working directory, else the
 * global $XDG_CONFIG_HOME/arqan/SYSTEM.md, else the built-in template. One
 * past AGENT_MAX_PROMPT_FILE is refused rather than truncated.
 *
 * Whichever wins is expanded before it is sent, so a prompt written months
 * ago still describes the tools that exist today. An unknown "{name}" is left
 * as written, which keeps braces safe in a prompt about JSON or C.
 *
 * AGENTS.md is the project's instructions rather than the operator's, so it
 * does not compete with the prompt: every one from the working directory up
 * to the root is appended to whichever won, since a subdirectory refines its
 * parent instead of replacing it.
 *
 * Plan mode's prompt is resolved the same way from PLAN.md and expanded
 * against the tools plan mode offers.
 */
#include "agent.h"

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
    "- Use grep and find to locate code, and bash for everything else a "
    "shell does; each returns a page, so narrow a search with a path or "
    "glob and page through the rest with offset\n"
    "- Use read to examine files instead of cat or sed, and its offset and "
    "limit to page through a long one rather than reading it whole\n"
    "- In bash, prefer head, tail, sed -n or grep over dumping a command's "
    "full output\n"
    "- Inspect environment variables for current model and session details "
    "when relevant\n"
    "- Change code with patch, giving each hunk enough context to match one "
    "place in the file; put every file of one change in a single call\n"
    "- Use write only for a whole file, and read a file before patching it\n"
    "- Be concise in responses\n"
    "- Show file paths clearly when working with files\n"
    "\n"
    "Current working directory: {cwd}\n";

/* The same project with none of the tools that change it, so what it can
 * promise is a plan rather than an edit. */
static const char PROMPT_PLAN_BUILTIN[] =
    "You are an expert software planner. You are in Plan mode: you "
    "investigate the project and propose a plan, and you change nothing. No "
    "file is written, no edit is applied; a tool that would do either is not "
    "available to you here.\n"
    "\n"
    "Available tools:\n"
    "{tools}\n"
    "Guidelines:\n"
    "- Read the code before planning it: a plan built on a guess about the "
    "codebase is worse than no plan\n"
    "- Call ask_user whenever a choice is the user's to make, offering the "
    "options you see and marking the one you recommend\n"
    "- Ask about one thing at a time, not a form of questions at once\n"
    "- Call submit_plan once the plan is complete: the plan is its argument, "
    "written as Markdown, and the user decides from it whether the work goes "
    "ahead\n"
    "- The plan states what changes, in which files, and in what order; it "
    "carries no code beyond what a step needs to be unambiguous\n"
    "- Assume the plan may be carried out in a session that has none of this "
    "conversation, so it stands on its own\n"
    "\n"
    "Current working directory: {cwd}\n";

/* Empty when `path` does not exist or holds only space. A file past the limit
 * sets `err` and reads nothing, which stops the search rather than falling
 * through to a prompt the user did not ask for. */
static Str prompt_read(Str path, Arena *a, char *err, size_t err_cap) {
    if (!path.n || path.n >= AGENT_MAX_PATH) return (Str){0};
    Str body = {0};
    u64 size = 0;
    if (file_read(a, path.p, AGENT_MAX_PROMPT_FILE, 0, &body, &size)
        == FILE_TOO_LARGE)
        snprintf(err, err_cap,
                 "%.*s is %llu bytes, over the %d byte system prompt limit",
                 (int)path.n, path.p, (unsigned long long)size,
                 (int)AGENT_MAX_PROMPT_FILE);
    return str_trim(body);
}

/* The prompt of the nearest ancestor of `dir` that has one, `suffix` carrying
 * its own leading separator ("/.arqan/SYSTEM.md"). As git does it, the project
 * root is wherever the marker is rather than where arqan started. */
static Str prompt_project(Str dir, const char *suffix, size_t suffix_size,
                          Arena *scratch, Str *path_out, char *err,
                          size_t err_cap) {
    char path[AGENT_MAX_PATH];
    size_t n = dir.n;
    if (!n || dir.p[0] != '/' || n + suffix_size > sizeof path)
        return (Str){0};
    memcpy(path, dir.p, n);
    while (n > 1 && path[n - 1] == '/') n--;
    for (;;) {
        /* At the root the directory is the separator the suffix carries. */
        size_t off = n == 1 ? 0 : n;
        memcpy(path + off, suffix, suffix_size);
        Str body = prompt_read((Str){ path, off + suffix_size - 1 }, scratch,
                               err, err_cap);
        if (body.n || *err) {
            if (body.n && path_out)
                *path_out = str_dup(scratch,
                                    (Str){ path, off + suffix_size - 1 });
            return body;
        }
        if (n == 1) return (Str){0};
        while (n > 1 && path[n - 1] != '/') n--;
        while (n > 1 && path[n - 1] == '/') n--;
    }
}

/* The highest precedence `name` in the config dirs. */
static Str prompt_global(Str name, Arena *scratch, Str *path_out, char *err,
                         size_t err_cap) {
    Str cand[AGENT_MAX_CONFIG_FILES];
    size_t n = paths_config_files(name, scratch, cand,
                                  AGENT_MAX_CONFIG_FILES);
    for (size_t i = n; i > 0; i--) {
        Str body = prompt_read(cand[i - 1], scratch, err, err_cap);
        if (body.n || *err) {
            if (body.n && path_out) *path_out = cand[i - 1];
            return body;
        }
    }
    return (Str){0};
}

/* Every AGENTS.md at or above `dir`, nearest first. Past
 * AGENT_MAX_AGENTS_FILES the outermost are dropped, since the nearest
 * describes the code being worked on. */
static size_t prompt_agents(Str dir, Arena *scratch, Str *body, Str *path_out,
                            size_t cap, char *err, size_t err_cap) {
    static const char suffix[] = "/AGENTS.md";
    char path[AGENT_MAX_PATH];
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

static void prompt_tools(Buf *b, const ToolRegistry *tools, AgentMode mode) {
    if (!tools) return;
    for (size_t i = 0; i < tools->n; i++) {
        if (!tools_available(tools, i, mode)) continue;
        buf_putf(b, "- %.*s: %.*s\n",
                 (int)tools->name[i].n, tools->name[i].p,
                 (int)tools->desc[i].n, tools->desc[i].p);
    }
}

/* Expands the placeholders of `tmpl` into `b`. */
static void prompt_expand(Buf *b, Str tmpl, const ToolRegistry *tools,
                          AgentMode mode, Str cwd) {
    for (size_t i = 0; i < tmpl.n; i++) {
        if (tmpl.p[i] != '{') { buf_putc(b, tmpl.p[i]); continue; }
        size_t end = i + 1;
        while (end < tmpl.n && tmpl.p[end] != '}' && tmpl.p[end] != '\n') end++;
        Str name = { tmpl.p + i + 1, end - i - 1 };
        if (end == tmpl.n || tmpl.p[end] != '}') { buf_putc(b, '{'); continue; }
        if (str_eq(name, STR("tools")))    prompt_tools(b, tools, mode);
        else if (str_eq(name, STR("cwd"))) buf_puts(b, cwd);
        else { buf_putc(b, '{'); continue; }
        i = end;
    }
}

/* One prompt, whichever mode it describes: `configured` wins, else the
 * project's file, else the global one, else `builtin`. */
static Str prompt_for(const ToolRegistry *tools, AgentMode mode, Str configured,
                      const char *project, size_t project_size, Str global,
                      const char *builtin, Arena *persist, Arena *scratch,
                      PromptSources *sources, char *err, size_t err_cap) {
    char cwd_buf[AGENT_MAX_PATH];
    Str cwd = getcwd(cwd_buf, sizeof cwd_buf) ? str_c(cwd_buf) : (Str){0};

    if (err_cap) err[0] = '\0';
    Str tmpl = configured, primary_path = {0};
    Str primary_label = configured.p ? STR("Configured prompt") : (Str){0};
    if (!tmpl.p) {
        tmpl = prompt_project(cwd, project, project_size, scratch,
                              &primary_path, err, err_cap);
        if (tmpl.n) primary_label = STR("Project prompt");
    }
    if (!tmpl.n && !*err) {
        tmpl = prompt_global(global, scratch, &primary_path, err, err_cap);
        if (tmpl.n) primary_label = STR("Global prompt");
    }
    if (*err) return (Str){0};
    if (!tmpl.n) {
        tmpl = str_c(builtin);
        primary_label = STR("Built-in prompt");
    }

    Str agents[AGENT_MAX_AGENTS_FILES], agent_paths[AGENT_MAX_AGENTS_FILES];
    size_t n_agents = prompt_agents(cwd, scratch, agents, agent_paths,
                                    AGENT_MAX_AGENTS_FILES, err, err_cap);
    if (*err) return (Str){0};

    size_t extra = 1024;
    for (size_t i = 0; i < n_agents; i++)
        extra += agents[i].n + agent_paths[i].n + 64;

    if (sources) memset(sources, 0, sizeof *sources);
    Buf primary;
    buf_init(&primary, persist, tmpl.n + extra);
    prompt_expand(&primary, tmpl, tools, mode, cwd);
    if (!buf_ok(&primary)) return (Str){0};
    Str expanded = buf_finish(&primary);

    if (sources) {
        sources->primary = expanded;
        sources->primary_label = primary_label;
        sources->primary_path = primary_path.n ? str_dup(persist, primary_path)
                                               : (Str){0};
        for (size_t i = 0; i < n_agents; i++) {
            sources->agents[i] = str_dup(persist, agents[i]);
            sources->agent_paths[i] = str_dup(persist, agent_paths[i]);
            if (!sources->agents[i].p || !sources->agent_paths[i].p)
                return (Str){0};
        }
        sources->n_agents = n_agents;
    }

    Buf b;
    buf_init(&b, persist, expanded.n + extra);
    buf_puts(&b, expanded);
    if (n_agents) {
        buf_puts(&b, STR("\n\nProject-specific instructions and "
                         "guidelines:\n"));
        for (size_t i = n_agents; i > 0; i--)
            buf_putf(&b, "\n<project_instructions path=\"%.*s\">\n%.*s\n"
                         "</project_instructions>\n",
                     (int)agent_paths[i - 1].n, agent_paths[i - 1].p,
                     (int)agents[i - 1].n, agents[i - 1].p);
    }
    if (!buf_ok(&b)) return (Str){0};
    return buf_finish(&b);
}

Str prompt_build(const ToolRegistry *tools, Str configured, Arena *persist,
                 Arena *scratch, PromptSources *sources, char *err,
                 size_t err_cap) {
    static const char project[] = "/." AGENT_NAME "/SYSTEM.md";
    return prompt_for(tools, MODE_BUILD, configured, project, sizeof project,
                      STR("SYSTEM.md"), PROMPT_BUILTIN, persist, scratch,
                      sources, err, err_cap);
}

Str prompt_build_plan(const ToolRegistry *tools, Arena *persist,
                      Arena *scratch, PromptSources *sources, char *err,
                      size_t err_cap) {
    static const char project[] = "/." AGENT_NAME "/PLAN.md";
    return prompt_for(tools, MODE_PLAN, (Str){0}, project, sizeof project,
                      STR("PLAN.md"), PROMPT_PLAN_BUILTIN, persist, scratch,
                      sources, err, err_cap);
}
