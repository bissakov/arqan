/* main.c — yoke entry point. Unity build: includes every module as one TU.
 *
 * Memory plan (no heap at all in our code):
 *   - two big static blocks wrapped in arenas: persistent (messages) + scratch
 *   - libcurl's own allocations happen inside curl and are out of our control.
 */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "yoke.h"

/* Unity includes — single translation unit. */
#include "core.c"
#include "json.c"
#include "http.c"
#include "config.c"
#include "tools.c"
#include "provider.c"
#include "tui.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t g_got_sigint = 0;
static void on_sigint(i32 sig) { (void)sig; g_got_sigint = 1; }

/* static backing storage — no heap malloc in our code path */
static alignas(64) u8 g_persist[YOKE_PERSIST_BYTES];
static alignas(64) u8 g_scratch[YOKE_ARENA_BYTES];

/* Slash commands handled below in the prompt loop; the TUI reads this table
 * to drive the composer's completion popup. */
static TuiCmd g_commands[YOKE_MAX_COMMANDS];

static size_t commands_init(void) {
    size_t n = 0;
    g_commands[n++] = (TuiCmd){ STR("/clear"), STR("Start a fresh conversation") };
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

/* Run the tool calls the turn just appended to `conv` — the carrier slots in
 * [first, last) — and append each result. Returns false when a result did not
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
        if (res_dup.n > 400) tui_write(STR("\n… output truncated in transcript"));
        tui_write(STR("\n"));
    }
    return true;
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

    Conv conv;
    if (!conv_init(&conv, &persist, cfg.max_messages)) {
        fprintf(stderr, "yoke: cannot reserve %zu conversation slots\n",
                cfg.max_messages);
        return 1;
    }

    /* seed system prompt */
    conv_add(&conv, M_SYSTEM, cfg.system_prompt);
    size_t session_mark = persist.off;

    /* SIGINT cancels line editing or the active provider request. */
    struct sigaction sa = {0}; sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    setvbuf(stdout, NULL, _IONBF, 0);
    tui_start(cfg.model, cfg.base_url, !cfg.api_key.p, tools.n);
    tui_set_commands(g_commands, commands_init());
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
            tui_clear();
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
                break; /* no tool calls → turn done */
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
    }

    tui_stop();
    return 0;
}
