/* main.c — ah entry point. Unity build: includes every module as one TU.
 *
 * Memory plan (all startup-time, no later heap):
 *   - two big blocks from aligned_alloc: persistent (messages) + scratch
 *   - libcurl's own allocations happen inside curl and are out of our control,
 *     but our code touches no heap after init.
 */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "ah.h"

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
static alignas(64) u8 g_persist[AH_PERSIST_BYTES];
static alignas(64) u8 g_scratch[AH_ARENA_BYTES];

/* Streaming sinks append to the TUI's transcript. */
static void on_text(Str delta, void *ud) {
    (void)ud;
    tui_write(delta);
}
static void on_tool_call(i32 idx, Str id, Str name, Str args_delta, void *ud) {
    (void)ud; (void)idx; (void)id; (void)name; (void)args_delta;
    tui_set_status("preparing tool call");
}

/* run one tool call against the registry; append a tool result message */
static void run_tool_calls(ToolRegistry *reg, Conv *conv, Arena *scratch,
                           Arena *persist, i32 count, Str *ids, Str *names, Str *args) {
    (void)count;
    /* iterate the conv's just-added assistant toolcall slots: we instead use
     * the arrays passed in from main, which mirror them. */
    for (i32 i = 0; i < count; i++) {
        const ToolDef *t = tools_find(reg, names[i]);
        Buf out; buf_init(&out, scratch, 4096);
        char err[256] = {0};
        tui_write(STR("\nTool · "));
        tui_write(names[i]);
        tui_write(STR("\n"));
        tui_write(args[i]);
        tui_write(STR("\n"));
        tui_set_status("running tool");
        b8 ok = t && t->run(args[i], scratch, &out, err, sizeof err);
        Str result = buf_finish(&out);
        if (!ok) {
            buf_putf(&out, "ERROR: %s", err);
            result = buf_finish(&out);
        }
        Str res_dup = str_dup(persist, result);
        conv_add_tool(conv, ids[i], res_dup);
        tui_write(STR("Result\n"));
        tui_write(str_take(res_dup, 400));
        if (res_dup.n > 400) tui_write(STR("\n… output truncated in transcript"));
        tui_write(STR("\n"));
    }
}

i32 main(i32 argc, char **argv) {
    (void)argc; (void)argv;

    Arena persist, scratch;
    arena_init(&persist, g_persist, sizeof g_persist);
    arena_init(&scratch,  g_scratch, sizeof g_scratch);

    Config cfg;
    config_load(&cfg, &persist);

    ToolRegistry tools;
    tools_init(&tools, &persist);

    Conv conv;
    conv_init(&conv, &persist, AH_MAX_MESSAGES);

    /* seed system prompt */
    conv_add(&conv, M_SYSTEM, cfg.system_prompt);
    size_t session_mark = persist.off;

    /* SIGINT cancels line editing or the active provider request. */
    struct sigaction sa = {0}; sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    setvbuf(stdout, NULL, _IONBF, 0);
    tui_start(cfg.model, cfg.base_url, !cfg.api_key.p, tools.n);
    atexit(tui_stop);

    char line[AH_LINE_BUF];
    for (;;) {
        size_t ln = 0;
        if (!tui_readline("> ", line, sizeof line, &ln)) break;
        if (ln == 0) { g_got_sigint = 0; continue; }
        if (!strcmp(line, "/exit") || !strcmp(line, "/q")) break;
        if (!strcmp(line, "/new")) {
            /* Keep the configured system prompt, discard the visible and
             * conversational state. Session persistence will replace this. */
            conv.n = 1;
            persist.off = session_mark;
            arena_reset(&scratch);
            tui_clear();
            continue;
        }

        conv_add(&conv, M_USER, str_dup(&persist, str_c(line)));
        tui_write(STR("You\n"));
        tui_write((Str){ line, ln });
        tui_write(STR("\n\nAssistant\n"));

        /* agent loop: keep running until the model emits no tool calls */
        g_got_sigint = 0;
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
            /* collect tool calls from conv tail (pairs of assistant slots) */
            /* The assistant message occupies slots [before .. n). Tool-call
             * carrier slots are the ones after the first with has_tool_call
             * and a tool_name. Gather ids/names/args. */
            Str ids[16], names[16], argss[16];
            i32 count = 0;
            for (size_t i = before; i < conv.n && count < 16; i++) {
                if (conv.role[i] == M_ASSISTANT && conv.has_tool_call[i]
                    && conv.tool_name[i].p && i != before) {
                    ids[count]    = conv.tool_call_id[i];
                    names[count]  = conv.tool_name[i];
                    argss[count]  = conv.text[i];
                    count++;
                }
            }
            if (count == 0) { break; }
            run_tool_calls(&tools, &conv, &scratch, &persist, count, ids, names, argss);
            tui_write(STR("\nAssistant\n"));
        }
    }

    tui_stop();
    return 0;
}
