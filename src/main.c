/* main.c: yoke entry point. Unity build: includes every module as one TU.
 *
 * Memory plan (no heap at all in our code):
 *   - two big static blocks wrapped in arenas: persistent (messages) + scratch
 *   - libcurl's own allocations happen inside curl and are out of our control.
 */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "yoke.h"

/* Unity includes: single translation unit. */
#include "core.c"
#include "json.c"
#include "http.c"
#include "paths.c"
#include "history.c"
#include "config.c"
#include "cli.c"
#include "tools.c"
#include "prompt.c"
#include "provider.c"
#include "session.c"
#include "tui.c"
#include "render.c"
#include "markdown.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t g_got_sigint = 0;
static void on_sigint(i32 sig) { (void)sig; g_got_sigint = 1; }

/* Static backing storage, so no malloc in our code path. */
static alignas(64) u8 g_persist[YOKE_PERSIST_BYTES];
static alignas(64) u8 g_scratch[YOKE_ARENA_BYTES];

/* Slash commands handled below in the prompt loop; the TUI reads this table
 * to drive the composer's completion popup. */
static TuiCmd g_commands[YOKE_MAX_COMMANDS];

static size_t commands_init(void) {
    size_t n = 0;
    g_commands[n++] = (TuiCmd){ STR("/clear"), STR("Start a fresh conversation") };
    g_commands[n++] = (TuiCmd){ STR("/resume"), STR("Resume a saved session from this directory") };
    g_commands[n++] = (TuiCmd){ STR("/model"), STR("Pick the model") };
    g_commands[n++] = (TuiCmd){ STR("/rewind"), STR("Go back to an earlier message and edit it") };
    g_commands[n++] = (TuiCmd){ STR("/copy"), STR("Copy the last response to the clipboard") };
    g_commands[n++] = (TuiCmd){ STR("/verbose"), STR("Toggle untruncated tool output") };
    g_commands[n++] = (TuiCmd){ STR("/raw"), STR("Toggle raw Markdown") };
    g_commands[n++] = (TuiCmd){ STR("/exit"), STR("Quit yoke") };
    return n;
}

/* Streaming sinks append to the TUI's transcript. */
/* Set while a reply's reasoning is still streaming, so the first word of the
 * answer proper can open a row of its own. */
static b8 g_reasoning;
static void on_reason(Str delta, void *ud) {
    (void)ud;
    if (!g_reasoning) {
        g_reasoning = true;
        tui_set_status("reasoning");
        tui_write(STR("\n"));
    }
    tui_write_reason(delta);
}
static void on_text(Str delta, void *ud) {
    (void)ud;
    if (g_reasoning) {
        g_reasoning = false;
        tui_set_status("thinking");
        tui_write(STR("\n\n"));
    }
    md_write(delta);
}
static void on_tool_call(i32 idx, Str id, Str name, Str args_delta, void *ud) {
    (void)ud; (void)idx; (void)id; (void)name; (void)args_delta;
    tui_set_status("preparing tool call");
}
/* Called from inside the request wait: keeps the composer typeable mid-turn. */
static void on_idle(void *ud) {
    (void)ud;
    tui_poll_input();
}

/* Run the tool calls the turn just appended to `conv`, the carrier slots in
 * [first, last), and append each result. Returns false when a result did not
 * fit in the conversation, which ends the turn. */
static b8 run_tool_calls(ToolRegistry *reg, Conv *conv, Arena *scratch,
                         Arena *persist, size_t first, size_t last) {
    for (size_t i = first; i < last; i++) {
        if (!conv_is_call(conv, i)) continue;
        Str name = conv->tool_name[i];
        Str args = conv->text[i];
        Str id   = conv->tool_call_id[i];
        size_t tool = tools_find(reg, name);
        Buf out; buf_init(&out, scratch, 4096);
        char err[256] = {0};
        render_tool_call(name, args, scratch);
        char status[32];
        snprintf(status, sizeof status, "running %.*s", (i32)name.n, name.p);
        tui_set_status(status);
        b8 ok = tools_run(reg, tool, args, scratch, &out, err, sizeof err);
        if (!ok && !err[0]) snprintf(err, sizeof err, "tool failed");
        if (!ok) { out.n = 0; buf_putf(&out, "ERROR: %s", err); }
        Str result = buf_finish(&out);
        Str res_dup = str_dup(persist, result);
        if (result.n && !res_dup.p) res_dup = STR("ERROR: out of memory");
        if (conv_add_tool(conv, id, res_dup) == CONV_NONE) {
            tui_write(STR("\n[conversation is full: /clear to start a new one]\n"));
            return false;
        }
        render_tool_result(name, res_dup);
    }
    return true;
}

/* The tool a result answers: a result slot carries only the id of the call it
 * belongs to, and how it reads depends on which tool produced it. */
static Str call_name(const Conv *c, size_t result) {
    for (size_t i = result; i-- > 0;)
        if (conv_is_call(c, i) && str_eq(c->tool_call_id[i],
                                         c->tool_call_id[result]))
            return c->tool_name[i];
    return (Str){0};
}

/* Repaint a resumed conversation: the transcript is a rendering of the
 * messages, so replaying them is the same code path a live turn takes. */
static void render_conv(const Conv *c, Arena *scratch) {
    for (size_t i = 0; i < c->n; i++) {
        switch (c->role[i]) {
            case M_SYSTEM: break;
            case M_USER: tui_write_user(c->text[i]); break;
            case M_TOOL:
                render_tool_result(call_name(c, i), c->text[i]);
                break;
            case M_ASSISTANT:
                if (conv_is_call(c, i)) {
                    render_tool_call(c->tool_name[i], c->text[i], scratch);
                } else if (c->text[i].n) {
                    md_write(c->text[i]);
                    md_end();
                    tui_write(STR("\n"));
                }
                break;
        }
    }
}

/* Offer the saved sessions for this directory and resume the chosen one.
 * Nothing to open leaves the view exactly as it was, welcome screen included,
 * and answers in the popup's own slot: a session that did not open is not part
 * of the conversation, so it has no business in the transcript. */
static void resume_session(Session *sess, Conv *conv, Arena *persist,
                           Arena *scratch, size_t session_mark) {
    arena_reset(scratch);
    SessionList list;
    size_t n = session_list(sess, scratch, &list, YOKE_MAX_SESSIONS);
    if (!n) {
        tui_notice(STR("no saved sessions in this directory"));
        arena_reset(scratch);
        return;
    }
    TuiCmd *items = arena_new(scratch, TuiCmd, n);
    if (!items) {
        tui_notice(STR("out of memory listing sessions"));
        arena_reset(scratch);
        return;
    }
    for (size_t i = 0; i < n; i++)
        items[i] = (TuiCmd){ list.name[i], list.preview[i] };

    size_t pick = 0;
    if (!tui_pick(STR("pick a session"), items, n, TUI_PICK_FIRST, &pick)) {
        arena_reset(scratch);
        return;
    }

    /* Read first: replaying rewinds the conversation and overwrites its
     * storage, so a session that cannot be read must not cost the one that is
     * running, so the view stays exactly as it was. */
    Str src = session_read(list.path[pick], scratch);
    if (!src.n) {
        tui_notice(STR("could not read that session"));
        arena_reset(scratch);
        return;
    }
    conv->n = 1;
    persist->off = session_mark;
    b8 whole = session_apply(sess, src, list.path[pick], list.name[pick], conv,
                             persist, scratch);
    tui_clear();
    render_conv(conv, scratch);
    if (!whole) tui_notice(STR("session truncated: the conversation is full"));
    arena_reset(scratch);
}

/* One popup row's worth of a message: control bytes become spaces and the cut
 * lands on a UTF-8 boundary, so neither a newline nor half a glyph reaches the
 * frame. */
#define REWIND_PREVIEW_BYTES 72
static Str preview_line(Arena *a, Str s) {
    char tmp[REWIND_PREVIEW_BYTES];
    size_t n = 0;
    for (size_t i = 0; i < s.n && n < sizeof tmp; i++) {
        u8 c = (u8)s.p[i];
        tmp[n++] = c < 0x20 ? ' ' : (char)c;
    }
    while (n && ((u8)tmp[n - 1] & 0xc0u) == 0x80u) n--;
    Buf b; buf_init(&b, a, n + 8);
    buf_put(&b, tmp, n);
    if (s.n > n) buf_puts(&b, STR("..."));
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

/* Go back to an earlier user turn: the chosen message returns to the composer
 * and everything from it onward leaves the conversation, so the next turn
 * continues from where that one did. The persistent arena is not rewound: its
 * messages are bump-allocated in conversation order, and the composer is
 * loaded from the text that is about to be dropped. */
static void rewind_conversation(Conv *conv, Session *sess, Arena *scratch) {
    arena_reset(scratch);
    size_t count = 0;
    for (size_t i = 0; i < conv->n; i++)
        if (conv->role[i] == M_USER) count++;
    if (!count) {
        tui_notice(STR("no message to go back to"));
        return;
    }
    /* A conversation can hold more turns than the popup does. Which end is
     * dropped is the caller's to decide, since the picker knows nothing about
     * the order: going back a hundred turns is a session to resume, not a
     * message to edit, so the oldest go. */
    size_t skip = count > YOKE_MAX_POPUP ? count - YOKE_MAX_POPUP : 0;
    size_t cap = count - skip;
    size_t *at = arena_new(scratch, size_t, cap);
    TuiCmd *items = arena_new(scratch, TuiCmd, cap);
    if (!at || !items) {
        tui_notice(STR("out of memory listing messages"));
        arena_reset(scratch);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < conv->n; i++) {
        if (conv->role[i] != M_USER) continue;
        if (skip) { skip--; continue; }
        at[n] = i;
        items[n] = (TuiCmd){ preview_line(scratch, conv->text[i]), (Str){0} };
        n++;
    }

    /* The list points into the transcript above it, so it is ordered like it:
     * oldest at the top, the newest turn on the row nearest the composer,
     * which is where the selection opens. Going back further is Up, as it is
     * in the composer's own history and in the scrollback. */
    size_t pick = 0;
    if (!tui_pick(STR("rewind to a message"), items, n, TUI_PICK_LAST, &pick)) {
        arena_reset(scratch);
        return;
    }
    size_t slot = at[pick];
    tui_set_input(conv->text[slot]);
    conv->n = slot;
    tui_clear();
    render_conv(conv, scratch);
    /* A session file is append-only and this one no longer describes the
     * conversation, so what is left of it continues in a new file, which the
     * next save writes whole. */
    session_begin(sess);
    session_save(sess, conv);
    arena_reset(scratch);
}

/* Copy the model's last reply as the Markdown it wrote: the transcript is a
 * rendering of that text, wrapped and interleaved with tool output, while the
 * conversation still holds the source. Slots carrying a tool call hold JSON
 * arguments, not prose, so they are not a reply. */
static void copy_last_reply(const Conv *conv) {
    for (size_t i = conv->n; i-- > 0;) {
        if (conv->role[i] != M_ASSISTANT || conv_is_call(conv, i)) continue;
        if (!conv->text[i].n) continue;
        tui_notice(tui_copy(conv->text[i])
                   ? STR("copied the last response")
                   : STR("that response is too large to copy"));
        return;
    }
    tui_notice(STR("no response to copy"));
}

/* Offer what the provider's /models endpoint lists and switch to the chosen
 * one for this session, remembering it for the next. The conversation is
 * untouched: a model change is not part of it. */
static void choose_model(Config *cfg, Arena *persist, Arena *scratch) {
    arena_reset(scratch);
    tui_set_status("loading models");
    Str names[YOKE_MAX_MODELS];
    char err[128] = {0};
    size_t n = provider_models(cfg, scratch, names, YOKE_MAX_MODELS,
                               err, sizeof err);
    tui_set_status("ready");
    if (!n) {
        tui_notice(str_c(err[0] ? err : "no models to pick from"));
        arena_reset(scratch);
        return;
    }
    TuiCmd *items = arena_new(scratch, TuiCmd, n);
    if (!items) {
        tui_notice(STR("out of memory listing models"));
        arena_reset(scratch);
        return;
    }
    for (size_t i = 0; i < n; i++)
        items[i] = (TuiCmd){ names[i],
                             str_eq(names[i], cfg->model) ? STR("current")
                                                          : (Str){0} };

    size_t pick = 0;
    if (!tui_pick(STR("pick a model"), items, n, TUI_PICK_FIRST, &pick)) {
        arena_reset(scratch);
        return;
    }
    Str chosen = str_dup(persist, names[pick]);
    if (!chosen.p) {
        tui_notice(STR("out of memory storing the model"));
        arena_reset(scratch);
        return;
    }
    cfg->model = chosen;
    tui_set_model(chosen);
    b8 saved = config_remember_model(chosen, scratch);
    char msg[256];
    i32 len = snprintf(msg, sizeof msg, "model: %.*s%s", (i32)chosen.n, chosen.p,
                       saved ? "" : " (not remembered: no state directory)");
    if (len > 0) tui_notice((Str){ msg, (size_t)len < sizeof msg
                                        ? (size_t)len : sizeof msg - 1 });
    arena_reset(scratch);
}

/* Everything one user turn touches, so the interactive loop and the one-shot
 * -p run drive the same code. */
typedef struct {
    Config       *cfg;
    ToolRegistry *tools;
    Conv         *conv;
    Arena        *persist;
    Arena        *scratch;
    Session      *sess;
    b8            echo;   /* write the prompt into the transcript */
} Agent;

/* Appends `text` as a user message and streams completions until the model
 * asks for no more tools. False when the turn ended on an error, an interrupt
 * or a full conversation, which is the exit status of a one-shot run. */
static b8 agent_turn(Agent *ag, Str text) {
    Conv *conv = ag->conv;

    Str user_text = str_dup(ag->persist, text);
    if (text.n && !user_text.p) {
        tui_write(STR("\n[out of memory: /clear to start a new session]\n\n"));
        return false;
    }
    if (conv_add(conv, M_USER, user_text) == CONV_NONE) {
        tui_write(STR("\n[conversation is full: /clear to start a new one]\n\n"));
        return false;
    }
    if (ag->echo) tui_write_user(text);
    session_save(ag->sess, conv);

    /* The composer stays editable throughout; only submitting waits. */
    b8 ok = false;
    g_got_sigint = 0;
    tui_set_busy(true);
    for (i32 turn = 0; turn < 16; turn++) {
        if (g_got_sigint) {
            tui_write(STR("\n[interrupted]\n\n"));
            tui_set_status("ready");
            g_got_sigint = 0;
            break;
        }
        tui_set_status("thinking");
        g_reasoning = false;
        Provider p = {
            .cfg = ag->cfg,
            .tools = ag->tools,
            .conv = conv,
            .persist = ag->persist,
            .scratch = ag->scratch,
            .on_text = on_text,
            .on_reason = on_reason,
            .on_tool_call = on_tool_call,
            .ud = NULL,
            .on_idle = on_idle,
            .idle_fd = tui_input_fd(),
            .interrupt_flag = &g_got_sigint,
        };
        char err[256] = {0};
        arena_reset(ag->scratch);
        /* snapshot conv tail to discover tool calls emitted this turn */
        size_t before = conv->n;
        i32 rc = provider_run(&p, err, sizeof err);
        /* The completion is whole: whatever line the renderer still held back
         * has no more bytes coming. */
        md_end();
        if (p.usage_valid) tui_set_context_tokens(p.total_tokens);
        if (g_got_sigint) {
            tui_write(STR("\n[interrupted]\n\n"));
            tui_set_status("ready");
            g_got_sigint = 0;
            break;
        }
        if (rc < 0) {
            tui_printf("\n[provider error: %s]\n\n", err);
            tui_set_status("ready");
            break;
        }
        if (rc == 0) {
            /* Close the reply's last row only: the air the next user box
             * writes above itself is the whole margin. */
            tui_write(STR("\n"));
            tui_set_status("ready");
            ok = true;
            break; /* no tool calls, turn done */
        }
        /* The turn appended one head slot plus a carrier per call at
         * [before, conv->n); run them straight off the conversation rather
         * than mirroring them into a second, separately capped array. */
        size_t tail = conv->n;
        if (!run_tool_calls(ag->tools, conv, ag->scratch, ag->persist,
                            before, tail)) {
            tui_set_status("ready");
            break;
        }
        tui_write(STR("\n"));
    }
    tui_set_busy(false);
    session_save(ag->sess, conv);
    return ok;
}

i32 main(i32 argc, char **argv) {
    CliOpts opts;
    switch (cli_parse(argc, argv, &opts)) {
        case CLI_DONE:  return 0;
        case CLI_ERROR: return 2;
        case CLI_RUN:   break;
    }
    if (opts.have_prompt && !opts.prompt.n) {
        fprintf(stderr, "yoke: the prompt is empty\n");
        return 2;
    }

    Arena persist, scratch;
    arena_init(&persist, g_persist, sizeof g_persist);
    arena_init(&scratch,  g_scratch, sizeof g_scratch);

    Config cfg;
    config_load(&cfg, &persist, &scratch);
    cli_apply(&opts, &cfg);
    arena_reset(&scratch);

    ToolRegistry tools;
    tools_init(&tools, &persist);
    char prompt_err[YOKE_MAX_PATH + 128] = {0};
    cfg.system_prompt = prompt_build(&tools, cfg.system_prompt, &persist,
                                     &scratch, prompt_err,
                                     sizeof prompt_err);
    if (!cfg.system_prompt.n) {
        fprintf(stderr, "yoke: %s\n", prompt_err);
        return 2;
    }
    arena_reset(&scratch);

    /* Prompt history lives in the XDG state dir. Without a resolvable one,
     * recall still works for this session and only the on-disk copy is lost. */
    History hist = {0};
    Arena hist_arena = {0};
    void *hist_mem = arena_alloc(&persist, YOKE_HISTORY_BYTES, 64);
    if (hist_mem) {
        arena_init(&hist_arena, hist_mem, YOKE_HISTORY_BYTES);
        if (history_init(&hist, &hist_arena, YOKE_MAX_HISTORY))
            history_load(&hist,
                         paths_file(YOKE_DIR_STATE, STR("history"), &persist),
                         &scratch);
    }
    arena_reset(&scratch);

    Conv conv;
    if (!conv_init(&conv, &persist, cfg.max_messages)) {
        fprintf(stderr, "yoke: cannot reserve %zu conversation slots\n",
                cfg.max_messages);
        return 1;
    }

    conv_add(&conv, M_SYSTEM, cfg.system_prompt);
    size_t session_mark = persist.off;

    /* Sessions are per working directory; without a resolvable data dir the
     * conversation simply is not persisted. */
    static Session sess;
    session_init(&sess, &scratch);
    arena_reset(&scratch);
    session_begin(&sess);

    /* SIGINT cancels line editing or the active provider request. */
    struct sigaction sa = {0}; sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    setvbuf(stdout, NULL, _IONBF, 0);
    tui_start(cfg.model, cfg.base_url, !cfg.api_key.p, tools.n,
              opts.have_prompt);
    tui_set_commands(g_commands, commands_init());
    tui_set_history(&hist);
    tui_set_interrupt_flag(&g_got_sigint);
    atexit(tui_stop);

    Agent agent = {
        .cfg = &cfg, .tools = &tools, .conv = &conv,
        .persist = &persist, .scratch = &scratch, .sess = &sess,
        .echo = !opts.have_prompt,
    };

    /* One-shot: the reply is the output, so nothing else goes to stdout and
     * the exit status reports whether the turn completed. */
    if (opts.have_prompt) {
        b8 ok = agent_turn(&agent, opts.prompt);
        tui_stop();
        return ok ? 0 : 1;
    }

    /* Static, not automatic: a megabyte of stack for a line the composer
     * already holds is the kind of frame that turns a deep call into a
     * crash. */
    static char line[YOKE_LINE_BUF];
    for (;;) {
        size_t ln = 0;
        if (!tui_readline("> ", line, sizeof line, &ln)) break;
        if (ln == 0) { g_got_sigint = 0; continue; }
        if (!strcmp(line, "/exit")) break;
        if (!strcmp(line, "/clear")) {
            /* Keep the configured system prompt, discard the visible and
             * conversational state. Session persistence will replace this. */
            conv.n = 1;
            persist.off = session_mark;
            arena_reset(&scratch);
            session_begin(&sess);   /* the next message starts a new file */
            tui_clear();
            continue;
        }
        if (!strcmp(line, "/rewind")) {
            rewind_conversation(&conv, &sess, &scratch);
            continue;
        }
        if (!strcmp(line, "/copy")) {
            copy_last_reply(&conv);
            continue;
        }
        if (!strcmp(line, "/raw")) {
            md_set_raw(!md_raw());
            tui_notice(md_raw()
                       ? STR("raw: replies are shown as the model wrote them")
                       : STR("raw: off, Markdown is formatted"));
            continue;
        }
        if (!strcmp(line, "/verbose")) {
            render_set_verbose(!render_verbose());
            tui_notice(render_verbose()
                       ? STR("verbose: tool output is shown in full")
                       : STR("verbose: tool output is truncated"));
            continue;
        }
        if (!strcmp(line, "/model")) {
            choose_model(&cfg, &persist, &scratch);
            continue;
        }
        if (!strcmp(line, "/resume")) {
            resume_session(&sess, &conv, &persist, &scratch, session_mark);
            continue;
        }
        agent_turn(&agent, (Str){ line, ln });
    }

    tui_stop();
    return 0;
}
