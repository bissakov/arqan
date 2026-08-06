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
#include "tools.c"
#include "provider.c"
#include "session.c"
#include "tui.c"

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
    g_commands[n++] = (TuiCmd){ STR("/exit"), STR("Quit yoke") };
    return n;
}

/* Streaming sinks append to the TUI's transcript. */
static void on_text(Str delta, void *ud) {
    (void)ud;
    tui_write(delta);
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
        tui_write(STR("\nTool · "));
        tui_write(name);
        tui_write(STR("\n"));
        tui_write(args);
        tui_write(STR("\n"));
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
        tui_write(STR("Result\n"));
        tui_write(str_take(res_dup, 400));
        if (res_dup.n > 400) tui_write(STR("\n... output truncated in transcript"));
        tui_write(STR("\n"));
    }
    return true;
}

/* Repaint a resumed conversation: the transcript is a rendering of the
 * messages, so replaying them is the same code path a live turn takes. */
static void render_conv(const Conv *c) {
    for (size_t i = 0; i < c->n; i++) {
        switch (c->role[i]) {
            case M_SYSTEM: break;
            case M_USER: tui_write_user(c->text[i]); break;
            case M_TOOL:
                tui_write(STR("Result\n"));
                tui_write(str_take(c->text[i], 400));
                if (c->text[i].n > 400)
                    tui_write(STR("\n... output truncated in transcript"));
                tui_write(STR("\n"));
                break;
            case M_ASSISTANT:
                if (conv_is_call(c, i)) {
                    tui_write(STR("\nTool · "));
                    tui_write(c->tool_name[i]);
                    tui_write(STR("\n"));
                    tui_write(c->text[i]);
                    tui_write(STR("\n"));
                } else if (c->text[i].n) {
                    tui_write(c->text[i]);
                    tui_write(STR("\n\n"));
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
    if (!tui_pick(items, n, &pick)) { arena_reset(scratch); return; }

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
    render_conv(conv);
    if (!whole) tui_notice(STR("session truncated: the conversation is full"));
    arena_reset(scratch);
}

i32 main(i32 argc, char **argv) {
    (void)argc; (void)argv;

    Arena persist, scratch;
    arena_init(&persist, g_persist, sizeof g_persist);
    arena_init(&scratch,  g_scratch, sizeof g_scratch);

    Config cfg;
    config_load(&cfg, &persist, &scratch);
    arena_reset(&scratch);

    ToolRegistry tools;
    tools_init(&tools, &persist);

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
    tui_start(cfg.model, cfg.base_url, !cfg.api_key.p, tools.n);
    tui_set_commands(g_commands, commands_init());
    tui_set_history(&hist);
    tui_set_interrupt_flag(&g_got_sigint);
    atexit(tui_stop);

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
        if (!strcmp(line, "/resume")) {
            resume_session(&sess, &conv, &persist, &scratch, session_mark);
            continue;
        }

        Str user_text = str_dup(&persist, (Str){ line, ln });
        if (ln && !user_text.p) {
            tui_write(STR("\n[out of memory: /clear to start a new session]\n\n"));
            continue;
        }
        if (conv_add(&conv, M_USER, user_text) == CONV_NONE) {
            tui_write(STR("\n[conversation is full: /clear to start a new one]\n\n"));
            continue;
        }
        tui_write_user((Str){ line, ln });
        session_save(&sess, &conv);

        /* agent loop: keep running until the model emits no tool calls.
         * The composer stays editable throughout; only submitting waits. */
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
            Provider p = {
                .cfg = &cfg,
                .tools = &tools,
                .conv = &conv,
                .persist = &persist,
                .scratch = &scratch,
                .on_text = on_text,
                .on_tool_call = on_tool_call,
                .ud = NULL,
                .on_idle = on_idle,
                .idle_fd = tui_input_fd(),
                .interrupt_flag = &g_got_sigint,
            };
            char err[256] = {0};
            arena_reset(&scratch);
            /* snapshot conv tail to discover tool calls emitted this turn */
            size_t before = conv.n;
            i32 rc = provider_run(&p, err, sizeof err);
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
                tui_write(STR("\n\n"));
                tui_set_status("ready");
                break; /* no tool calls, turn done */
            }
            /* The turn appended one head slot plus a carrier per call at
             * [before, conv.n); run them straight off the conversation rather
             * than mirroring them into a second, separately capped array. */
            size_t tail = conv.n;
            if (!run_tool_calls(&tools, &conv, &scratch, &persist, before, tail)) {
                tui_set_status("ready");
                break;
            }
            tui_write(STR("\n"));
        }
        tui_set_busy(false);
        session_save(&sess, &conv);
    }

    tui_stop();
    return 0;
}
