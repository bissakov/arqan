/* main.c: yoke entry point. Unity build: includes every module as one TU. */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "yoke.h"

#include "core.c"
#include "json.c"
#include "http.c"
#include "paths.c"
#include "telemetry.c"
#include "history.c"
#include "endpoints.c"
#include "config.c"
#include "cli.c"
#include "tools.c"
#include "prompt.c"
#include "provider.c"
#include "session.c"
#include "tui.c"
#include "render.c"
#include "markdown.c"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t g_got_sigint = 0;
static void on_sigint(i32 sig) { (void)sig; g_got_sigint = 1; }

/* Static backing storage: no malloc anywhere in our code. */
static alignas(64) u8 g_persist[YOKE_PERSIST_BYTES];
static alignas(64) u8 g_scratch[YOKE_ARENA_BYTES];

/* Handled below in the prompt loop; the TUI reads this to drive the
 * composer's completion popup. */
static TuiCmd g_commands[YOKE_MAX_COMMANDS];

static size_t g_command_n;

static size_t commands_init(void) {
    size_t n = 0;
    g_commands[n++] = (TuiCmd){ STR("/clear"), STR("Start a fresh conversation") };
    g_commands[n++] = (TuiCmd){ STR("/resume"), STR("Resume a saved session from this directory") };
    g_commands[n++] = (TuiCmd){ STR("/fork"), STR("Continue in a copy, leaving this session as it is") };
    g_commands[n++] = (TuiCmd){ STR("/model"), STR("Pick the model") };
    g_commands[n++] = (TuiCmd){ STR("/provider"), STR("Switch provider, or add one") };
    g_commands[n++] = (TuiCmd){ STR("/mode"), STR("Switch between Build and Plan mode (Shift+Tab)") };
    g_commands[n++] = (TuiCmd){ STR("/rewind"), STR("Go back to an earlier message and edit it") };
    g_commands[n++] = (TuiCmd){ STR("/copy"), STR("Copy the last response to the clipboard") };
    g_commands[n++] = (TuiCmd){ STR("/settings"), STR("Change how yoke behaves") };
    g_commands[n++] = (TuiCmd){ STR("/exit"), STR("Quit yoke") };
    g_command_n = n;
    return n;
}

/* Whether the round's thinking trace and its reply have opened their block:
 * each is one, so the deltas after the first do not open a second. */
static b8 g_reasoning;
static b8 g_replying;
static void on_reason(Str delta, void *ud) {
    (void)ud;
    if (!g_reasoning) {
        g_reasoning = true;
        tui_set_status("reasoning");
        tui_block();
    }
    tui_write_reason(delta);
}
static void on_text(Str delta, void *ud) {
    (void)ud;
    if (!g_replying) {
        g_replying = true;
        g_reasoning = false;
        tui_set_status("thinking");
        tui_block();
    }
    md_write(delta);
}
static void on_tool_call(i32 idx, Str id, Str name, Str args_delta, void *ud) {
    (void)ud; (void)idx; (void)id; (void)name; (void)args_delta;
    tui_set_status("preparing tool call");
}
/* Said in the transcript rather than in a notice: it belongs to the turn
 * being read. It never reaches Conv, so a replay does not repeat it. */
static void on_retry(i32 attempt, i32 attempts, i32 delay_ms, Str reason,
                     void *ud) {
    (void)ud;
    tui_set_status("retrying");
    tui_block();
    char wait[32];
    if (delay_ms < 1000) snprintf(wait, sizeof wait, "%dms", delay_ms);
    else snprintf(wait, sizeof wait, "%.1fs", (f64)delay_ms / 1000.0);
    char row[256];
    i32 n = snprintf(row, sizeof row,
                     "[%.*s; retrying in %s (attempt %d of %d)]\n",
                     (i32)reason.n, reason.p, wait, attempt + 1, attempts);
    if (n > 0)
        tui_write_error((Str){ row, (size_t)n < sizeof row ? (size_t)n
                                                           : sizeof row - 1 });
    g_replying = false;
    g_reasoning = false;
}
/* Called from inside the request wait, so the composer stays typeable. */
static void on_idle(void *ud) {
    (void)ud;
    tui_poll_input();
}

/* Everything one user turn touches, so the interactive loop and -p share it. */
typedef struct {
    Config       *cfg;
    ToolRegistry *tools;
    Conv         *conv;
    Arena        *persist;
    Arena        *scratch;
    Session      *sess;
    size_t        mark;   /* persist offset a conversation starts at */
    /* An approved plan on its way to a session of its own. It lives in the
     * scratch arena until the turn carrying it re-anchors it in persist,
     * which is what lets the conversation it came from be dropped whole. */
    Str           handoff;
    b8            echo;   /* write the prompt into the transcript */
} Agent;

static Str mode_name(AgentMode m) {
    return m == MODE_PLAN ? STR("plan") : STR("build");
}

/* The settings a report needs and the working directory as a hash. Written
 * when recording starts, at startup or at the /telemetry that turned it on. */
static void telemetry_session(const Config *cfg, const ToolRegistry *tools) {
    TelEvent e;
    tel_open(&e, "session");
    tel_str(&e, "version", STR(YOKE_VERSION));
    tel_str(&e, "model", cfg->model);
    tel_str(&e, "provider", cfg->provider);
    tel_str(&e, "mode", mode_name(cfg->mode));
    tel_int(&e, "tools", (i64)tools->n);
    tel_int(&e, "max_tokens", cfg->max_tokens);
    tel_int(&e, "max_messages", (i64)cfg->max_messages);
    tel_bool(&e, "has_key", cfg->api_key.p != NULL);
    tel_int(&e, "cols", (i64)tui_body_cols());
    tel_bool(&e, "fullscreen", tui_is_fullscreen());
    char cwd[YOKE_MAX_PATH];
    if (getcwd(cwd, sizeof cwd)) tel_hash_field(&e, "cwd", str_c(cwd));
    tel_send(&e);
}

/* What the tool calls of one round asked the turn to do next. */
typedef enum { TURN_CONTINUE, TURN_DONE, TURN_HANDOFF, TURN_FULL } TurnAction;

/* Prompt and registry move together, so the model never sees one without the
 * other. */
static void agent_set_mode(Agent *ag, AgentMode mode) {
    TelEvent e;
    tel_open(&e, "mode");
    tel_str(&e, "from", mode_name(ag->cfg->mode));
    tel_str(&e, "to", mode_name(mode));
    tel_send(&e);
    ag->cfg->mode = mode;
    tools_set_mode(mode);
    if (ag->conv->n && ag->conv->role[0] == M_SYSTEM)
        ag->conv->text[0] = mode == MODE_PLAN ? ag->cfg->plan_prompt
                                              : ag->cfg->system_prompt;
    tui_set_mode(mode);
}

/* False when the conversation had no room left, which ends the turn. */
static b8 add_result(Agent *ag, size_t call, Str name, Str result) {
    Conv *conv = ag->conv;
    size_t slot = conv_add_tool(conv, conv->tool_call_id[call], result);
    if (slot == CONV_NONE) {
        tui_block();
        tui_write(STR("[conversation is full: /clear to start a new one]\n"));
        return false;
    }
    render_tool_result(name, result, (u32)(slot + 1), conv->expanded[slot]);
    return true;
}

static Str json_field(const JVal *j, Str key) {
    const JVal *v = j ? json_get(j, key) : NULL;
    return v && v->type == J_STR ? v->u.s : (Str){0};
}

/* The rows are the options the model offered, the list opens on the one it
 * recommends, and a last row hands the composer over for an answer it did
 * not think of. Empty when the question was dismissed. */
static Str ask_user_answer(Agent *ag, Str args) {
    JVal *j = json_parse(ag->scratch, args);
    Str question = json_field(j, STR("question"));
    const JVal *opts = j ? json_get(j, STR("options")) : NULL;
    size_t n = opts && opts->type == J_ARR ? opts->u.arr.n : 0;
    if (n > YOKE_MAX_POPUP - 1) n = YOKE_MAX_POPUP - 1;

    render_question(question);

    TuiCmd *items = arena_new(ag->scratch, TuiCmd, n + 1);
    if (!items) return (Str){0};
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        const JVal *o = json_at(opts, i);
        Str label = json_field(o, STR("label"));
        Str detail = json_field(o, STR("detail"));
        const JVal *rec = o ? json_get(o, STR("recommended")) : NULL;
        if (rec && rec->type == J_BOOL && rec->u.b) {
            start = i;
            Buf b; buf_init(&b, ag->scratch, detail.n + 24);
            buf_puts(&b, STR("recommended"));
            if (detail.n) { buf_puts(&b, STR(" \u00b7 ")); buf_puts(&b, detail); }
            if (buf_ok(&b)) detail = buf_finish(&b);
        }
        items[i] = (TuiCmd){ label, detail };
    }
    items[n] = (TuiCmd){ STR("+ something else"),
                         STR("Answer in your own words") };

    size_t pick = 0;
    if (!tui_pick_from(STR("pick an answer"), items, n + 1, start, &pick))
        return (Str){0};
    if (pick < n) return str_dup(ag->persist, items[pick].name);

    char typed[512];
    if (!tui_ask(STR("your answer"), false, typed, sizeof typed))
        return (Str){0};
    return str_dup(ag->persist, str_c(typed));
}

/* The three answers to a submitted plan are the three ways a turn goes on. */
static TurnAction submit_plan_answer(Agent *ag, Str args, Str *result) {
    Str plan = json_field(json_parse(ag->scratch, args), STR("plan"));
    render_plan(plan);

    const TuiCmd items[] = {
        { STR("Yes"), STR("Switch to Build mode and carry the plan out") },
        { STR("Yes, but from a new session"),
          STR("Start over with the plan as the only context") },
        { STR("No"), STR("Keep planning; say what to change") },
    };
    size_t pick = 2;
    /* A dismissed question is not an approval, so cancelling is "No". */
    if (!tui_pick(STR("continue?"), items, 3, TUI_PICK_FIRST, &pick)) pick = 2;
    if (pick == 2) {
        *result = STR("The user rejected the plan. Stop and wait for what "
                      "they want changed.");
        return TURN_DONE;
    }
    if (pick == 0) {
        agent_set_mode(ag, MODE_BUILD);
        *result = STR("The user approved the plan. You are in Build mode "
                      "now: carry it out.");
        return TURN_CONTINUE;
    }
    ag->handoff = plan;
    *result = STR("The user approved the plan and moved it to a new "
                  "session, which carries it out from the plan alone.");
    return TURN_HANDOFF;
}

/* Run the carrier slots in [first, last) and append each result. The two plan
 * mode tools are questions put to the user rather than work, so they are
 * answered here instead of through tools_run, which cannot reach the screen. */
static TurnAction run_tool_calls(Agent *ag, size_t first, size_t last) {
    Conv *conv = ag->conv;
    /* Every call is answered even once the user has ended the turn: one left
     * without its result is a conversation the provider refuses. */
    TurnAction pending = TURN_CONTINUE;
    for (size_t i = first; i < last; i++) {
        if (!conv_is_call(conv, i)) continue;
        Str name = conv->tool_name[i];
        Str args = conv->text[i];
        if (str_eq(name, STR("submit_plan"))) {
            Str result = {0};
            TurnAction act = submit_plan_answer(ag, args, &result);
            if (!add_result(ag, i, STR("plan"), result)) return TURN_FULL;
            if (act != TURN_CONTINUE) pending = act;
            continue;
        }
        if (str_eq(name, STR("ask_user"))) {
            Str answer = ask_user_answer(ag, args);
            b8 dismissed = !answer.n;
            if (dismissed)
                answer = STR("The user dismissed the question without "
                             "answering. Stop and wait for their next "
                             "message.");
            if (!add_result(ag, i, STR("ask"), answer)) return TURN_FULL;
            if (dismissed) pending = TURN_DONE;
            continue;
        }
        size_t tool = tools_find(ag->tools, name);
        Buf out; buf_init(&out, ag->scratch, 4096);
        char err[256] = {0};
        f64 started = yoke_now_seconds();
        render_tool_call(name, args, ag->scratch, (u32)(i + 1),
                         conv->expanded[i]);
        char status[32];
        snprintf(status, sizeof status, "running %.*s", (i32)name.n, name.p);
        tui_set_status(status);
        b8 ok = tools_run(ag->tools, tool, args, ag->scratch, &out, err,
                          sizeof err);
        if (!ok && !err[0]) snprintf(err, sizeof err, "tool failed");
        if (!ok) { out.n = 0; buf_putf(&out, "ERROR: %s", err); }
        Str result = buf_finish(&out);
        /* Which tool, which argument keys, how long, how much. What the
         * arguments said and what it answered stay out. */
        TelEvent e;
        tel_open(&e, "tool");
        tel_str(&e, "name", name);
        tel_bool(&e, "known", tool != TOOL_NONE);
        tel_int(&e, "args_bytes", (i64)args.n);
        tel_arg_keys(&e, "args", args, ag->scratch);
        tel_int(&e, "ms", (i64)((yoke_now_seconds() - started) * 1000.0));
        tel_bool(&e, "ok", ok);
        tel_shape(&e, "result", result);
        tel_send(&e);
        Str res_dup = str_dup(ag->persist, result);
        if (result.n && !res_dup.p) res_dup = STR("ERROR: out of memory");
        if (!add_result(ag, i, name, res_dup)) return TURN_FULL;
    }
    return pending;
}

/* A result slot carries only the id of the call it belongs to, and how it
 * reads depends on which tool produced it. */
static Str call_name(const Conv *c, size_t result) {
    for (size_t i = result; i-- > 0;)
        if (conv_is_call(c, i) && str_eq(c->tool_call_id[i],
                                         c->tool_call_id[result]))
            return c->tool_name[i];
    return (Str){0};
}

/* The transcript is a rendering of the messages, so replaying them takes the
 * same path a live turn does. */
static void render_conv(const Conv *c, Arena *scratch) {
    for (size_t i = 0; i < c->n; i++) {
        switch (c->role[i]) {
            case M_SYSTEM: break;
            case M_USER:
                if (conv_is_shell(c, i)) {
                    render_shell_call(c->text[i], (u32)(i + 1), c->expanded[i]);
                    render_tool_result(STR("shell"), c->shell_out[i],
                                       (u32)(i + 1), c->expanded[i]);
                } else {
                    tui_write_user(c->text[i]);
                }
                break;
            case M_TOOL:
                render_tool_result(call_name(c, i), c->text[i],
                                   (u32)(i + 1), c->expanded[i]);
                break;
            case M_ASSISTANT:
                if (conv_is_call(c, i)) {
                    render_tool_call(c->tool_name[i], c->text[i], scratch,
                                     (u32)(i + 1), c->expanded[i]);
                } else if (c->text[i].n) {
                    tui_block();
                    md_write(c->text[i]);
                    md_end();
                }
                break;
        }
    }
}

/* `zone` is the block the reader acted on, which keeps its place on screen
 * while everything above it is rebuilt; 0 when no one block was involved. */
static void rerender_conv(const Conv *conv, Arena *scratch, u32 zone) {
    arena_reset(scratch);
    tui_anchor_zone(zone);
    tui_clear_transcript();
    render_conv(conv, scratch);
    tui_restore_anchor();
    arena_reset(scratch);
}

/* Nothing to open leaves the view exactly as it was and answers in the
 * popup's own slot: a session that did not open is not part of the
 * conversation, so it has no business in the transcript. */
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

    /* Read first: replaying overwrites the live conversation's storage, so a
     * session that cannot be read must not cost the one that is running. */
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

/* One popup row of a message: control bytes become spaces and the cut lands
 * on a UTF-8 boundary. */
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

/* The chosen message returns to the composer and everything from it onward
 * leaves the conversation. The persistent arena is not rewound: the composer
 * is loaded from the text that is about to be dropped. */
static void rewind_conversation(Conv *conv, Session *sess, Arena *scratch) {
    arena_reset(scratch);
    size_t count = 0;
    for (size_t i = 0; i < conv->n; i++)
        if (conv->role[i] == M_USER && !conv_is_shell(conv, i)) count++;
    if (!count) {
        tui_notice(STR("no message to go back to"));
        return;
    }
    /* A conversation can hold more turns than the popup does, and going back
     * a hundred of them is a session to resume rather than a message to
     * edit, so the oldest go. */
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
        if (conv->role[i] != M_USER || conv_is_shell(conv, i)) continue;
        if (skip) { skip--; continue; }
        at[n] = i;
        items[n] = (TuiCmd){ preview_line(scratch, conv->text[i]), (Str){0} };
        n++;
    }

    /* The list points into the transcript, so it is ordered like it: the
     * newest turn sits nearest the composer and going back further is Up, as
     * it is in the composer's own history. */
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
     * conversation, so what is left of it continues in a new file. */
    session_fork(sess, conv);
    arena_reset(scratch);
}

/* Continue in a copy: only the file underneath changes, so the session this
 * was forked from keeps what it had, where /resume can still find it. */
static void fork_session(Session *sess, const Conv *conv) {
    if (conv->n <= 1) {
        tui_notice(STR("nothing to fork yet"));
        return;
    }
    if (!sess->dir.n) {
        tui_notice(STR("sessions are not saved here: nothing to fork"));
        return;
    }
    if (!session_fork(sess, conv)) {
        tui_notice(STR("could not start a forked session"));
        return;
    }
    tui_notice(STR("forked: this copy continues, the original is unchanged"));
}

/* The last reply as the Markdown it was written in, which the conversation
 * still holds while the transcript has only a wrapped rendering of it. A slot
 * carrying a tool call holds JSON arguments rather than prose. */
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

/* tui_notice copies, so the stack buffer only has to outlive the call. */
static void notice_fmt(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static void notice_fmt(const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    i32 len = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (len <= 0) return;
    size_t n = (size_t)len < sizeof msg ? (size_t)len : sizeof msg - 1;
    tui_notice((Str){ msg, n });
}

/* Only the commands yoke offers are named: a line it does not know is the
 * user's text, not ours. */
static void telemetry_command(Str line) {
    Str word = line;
    for (size_t i = 0; i < line.n; i++)
        if (line.p[i] == ' ') { word = str_take(line, i); break; }
    b8 offered = false;
    for (size_t i = 0; i < g_command_n && !offered; i++)
        offered = str_eq(g_commands[i].name, word);
    TelEvent e;
    tel_open(&e, "command");
    tel_str(&e, "name", offered ? word : STR("(unknown)"));
    tel_send(&e);
}

/* The pick points into `scratch`, so a caller that keeps it copies it out
 * before resetting. False when nothing was listed or chosen, having said so. */
static b8 pick_model(const Config *cfg, Arena *scratch, Str *out) {
    tui_set_status("loading models");
    Str names[YOKE_MAX_MODELS];
    char err[128] = {0};
    size_t n = provider_models(cfg, scratch, names, YOKE_MAX_MODELS,
                               err, sizeof err);
    tui_set_status("ready");
    if (!n) {
        tui_notice(str_c(err[0] ? err : "no models to pick from"));
        return false;
    }
    TuiCmd *items = arena_new(scratch, TuiCmd, n);
    if (!items) {
        tui_notice(STR("out of memory listing models"));
        return false;
    }
    for (size_t i = 0; i < n; i++)
        items[i] = (TuiCmd){ names[i],
                             str_eq(names[i], cfg->model) ? STR("current")
                                                          : (Str){0} };
    size_t pick = 0;
    if (!tui_pick(STR("pick a model"), items, n, TUI_PICK_FIRST, &pick))
        return false;
    *out = names[pick];
    return true;
}

/* Switch model for this session and remember it for the next. The
 * conversation is untouched: a model change is not part of it. */
static void choose_model(Config *cfg, Arena *persist, Arena *scratch) {
    arena_reset(scratch);
    Str picked = {0};
    if (!pick_model(cfg, scratch, &picked)) {
        arena_reset(scratch);
        return;
    }
    Str chosen = str_dup(persist, picked);
    if (!chosen.p) {
        tui_notice(STR("out of memory storing the model"));
        arena_reset(scratch);
        return;
    }
    cfg->model = chosen;
    tui_set_model(chosen);
    TelEvent e;
    tel_open(&e, "model");
    tel_str(&e, "name", chosen);
    tel_send(&e);
    /* A model id only means something against the endpoint that served it. */
    b8 saved = cfg->provider.n
             ? endpoints_remember_model(cfg->provider, chosen, scratch)
             : config_remember_model(chosen, scratch);
    notice_fmt("model: %.*s%s", (i32)chosen.n, chosen.p,
               saved ? "" : " (not remembered: no state directory)");
    arena_reset(scratch);
}

/* No key and no endpoint from a flag, the environment, a config file or the
 * store. The default base URL is a placeholder here, not a destination. */
static b8 no_provider(const Config *cfg) {
    return !cfg->api_key.p && !cfg->base_url_set;
}

/* Copies into `persist` because the endpoint store they came from lives in
 * the scratch arena, which the next turn rewinds. */
static b8 use_endpoint(Config *cfg, Str name, Str base_url, Str model,
                       Str key, Arena *persist) {
    Str n = str_dup(persist, name);
    Str u = str_dup(persist, base_url);
    Str m = model.n ? str_dup(persist, model) : (Str){0};
    if (!n.p || !u.p || (model.n && !m.p)) return false;
    cfg->provider = n;
    cfg->base_url = u;
    cfg->base_url_set = true;
    cfg->api_key  = key;
    if (m.n) cfg->model = m;
    tui_set_provider(n);
    tui_set_model(cfg->model);
    tui_needs_provider(false);
    TelEvent e;
    tel_open(&e, "provider");
    tel_str(&e, "name", n);
    tel_bool(&e, "has_key", key.p != NULL);
    tel_send(&e);
    return true;
}

/* Ask for a provider and store it: a name, an OpenAI-compatible base URL and
 * a key, then the model, which is picked from what that endpoint actually
 * lists. Listing is also the check that the URL and the key work, so a typo
 * is answered here rather than on the first turn, and nothing is written
 * until it succeeds. Returns false when the form was cancelled or refused. */
static b8 add_endpoint(Config *cfg, Arena *persist, Arena *scratch) {
    arena_reset(scratch);
    char name[YOKE_MAX_ENDPOINT_NAME + 1];
    char url[YOKE_MAX_URL + 1];
    char key[YOKE_MAX_API_KEY + 1];
    if (!tui_ask(STR("a name for this provider"), false, name, sizeof name))
        return false;

    Endpoints eps;
    endpoints_load(&eps, scratch);
    if (endpoints_find(&eps, str_c(name)) != ENDPOINT_NONE) {
        notice_fmt("a provider named %s already exists", name);
        return false;
    }
    if (eps.n >= YOKE_MAX_ENDPOINTS) {
        notice_fmt("no room for another provider (%d)", YOKE_MAX_ENDPOINTS);
        return false;
    }
    if (!tui_ask(STR("its base URL, ending in /v1"), false, url, sizeof url))
        return false;
    if (!str_starts(str_c(url), STR("http://"))
        && !str_starts(str_c(url), STR("https://"))) {
        tui_notice(STR("a base URL starts with http:// or https://"));
        return false;
    }
    /* A local server needs no key, so an empty answer is a valid one. */
    if (!tui_ask(STR("its API key (empty if it needs none)"), true, key,
                 sizeof key))
        key[0] = '\0';

    Config probe = *cfg;
    probe.base_url = str_c(url);
    probe.api_key = key[0] ? str_c(key) : (Str){0};
    probe.model = (Str){0};
    Str model = {0};
    if (!pick_model(&probe, scratch, &model)) return false;

    char err[YOKE_MAX_PATH + 64] = {0};
    if (!endpoints_put(&eps, str_c(name), str_c(url), model, scratch)
        || !endpoints_save(&eps, scratch)) {
        tui_notice(STR("could not write the provider store"));
        return false;
    }
    if (key[0] && !endpoints_set_key(str_c(name), str_c(key), scratch,
                                     err, sizeof err)) {
        tui_notice(str_c(err[0] ? err : "could not store the API key"));
        return false;
    }
    Str stored_key = key[0] ? str_dup(persist, str_c(key)) : (Str){0};
    if (key[0] && !stored_key.p) {
        tui_notice(STR("out of memory storing the provider"));
        return false;
    }
    if (!use_endpoint(cfg, str_c(name), str_c(url), model, stored_key,
                      persist)) {
        tui_notice(STR("out of memory storing the provider"));
        return false;
    }
    endpoints_remember_active(cfg->provider, scratch);
    notice_fmt("provider: %.*s", (i32)cfg->provider.n, cfg->provider.p);
    arena_reset(scratch);
    return true;
}

/* The providers already stored, plus the entry that creates one. With none
 * stored there is nothing to pick from, so the form opens straight away. */
static void choose_provider(Config *cfg, Arena *persist, Arena *scratch) {
    arena_reset(scratch);
    Endpoints eps;
    size_t n = endpoints_load(&eps, scratch);
    if (!n) {
        add_endpoint(cfg, persist, scratch);
        arena_reset(scratch);
        return;
    }
    TuiCmd *items = arena_new(scratch, TuiCmd, n + 1);
    if (!items) {
        tui_notice(STR("out of memory listing providers"));
        arena_reset(scratch);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        Str desc = eps.base_url[i];
        if (str_eq(eps.name[i], cfg->provider)) {
            Buf b; buf_init(&b, scratch, desc.n + 16);
            buf_puts(&b, STR("current · "));
            buf_puts(&b, eps.base_url[i]);
            if (buf_ok(&b)) desc = buf_finish(&b);
        }
        items[i] = (TuiCmd){ eps.name[i], desc };
    }
    items[n] = (TuiCmd){ STR("+ add a provider"),
                         STR("An OpenAI-compatible URL and key") };

    size_t pick = 0;
    if (!tui_pick(STR("pick a provider"), items, n + 1, TUI_PICK_FIRST,
                  &pick)) {
        arena_reset(scratch);
        return;
    }
    if (pick == n) {
        add_endpoint(cfg, persist, scratch);
        arena_reset(scratch);
        return;
    }
    char err[YOKE_MAX_PATH + 64] = {0};
    Str key = endpoints_key(eps.name[pick], persist, scratch, err, sizeof err);
    if (err[0]) {
        tui_notice(str_c(err));
        arena_reset(scratch);
        return;
    }
    if (!use_endpoint(cfg, eps.name[pick], eps.base_url[pick], eps.model[pick],
                      key, persist)) {
        tui_notice(STR("out of memory switching provider"));
        arena_reset(scratch);
        return;
    }
    endpoints_remember_active(cfg->provider, scratch);
    notice_fmt("provider: %.*s", (i32)cfg->provider.n, cfg->provider.p);
    arena_reset(scratch);
}

/* ---- /settings -----------------------------------------------------------
 * The screen is its own answer, so nothing here writes a notice: a toggle
 * that refused to change is a box that stayed empty. Nothing is persisted
 * here either, since a setting that outlives the session is already
 * remembered by whoever owns it.
 */
enum {
    SET_VERBOSE, SET_RAW, SET_STREAM, SET_TELEMETRY,
    SET_MODE, SET_MODEL, SET_PROVIDER, SET_MAX_TOKENS, SET_N
};

/* "[x] label" for a toggle and the same column for a value row, so the two
 * kinds read as one list. A row that lost its checkbox to a full arena is
 * still the row it was. */
static Str setting_label(Arena *a, Str label, const char *box) {
    Buf b; buf_init(&b, a, label.n + 8);
    buf_puts(&b, str_c(box));
    buf_puts(&b, label);
    return buf_ok(&b) ? buf_finish(&b) : label;
}
static Str setting_check(Arena *a, b8 on, Str label) {
    return setting_label(a, label, on ? "[x] " : "[ ] ");
}
static Str setting_value(Arena *a, Str label) {
    return setting_label(a, label, "    ");
}

/* An answer that is not a number leaves the setting alone, since the
 * alternative is guessing at what was meant. */
static void ask_max_tokens(Config *cfg) {
    char typed[32];
    if (!tui_ask(STR("max tokens for one reply"), false, typed, sizeof typed))
        return;
    b8 ok = false;
    i64 v = str_int(str_trim(str_c(typed)), &ok);
    if (!ok || v < 1) return;
    cfg->max_tokens = v > (1 << 20) ? (1 << 20) : (i32)v;
}

static void choose_settings(Agent *ag) {
    Config *cfg = ag->cfg;
    Arena *scratch = ag->scratch;
    size_t sel = 0;
    for (;;) {
        arena_reset(scratch);
        char tokens[16];
        snprintf(tokens, sizeof tokens, "%d", cfg->max_tokens);
        TuiCmd rows[SET_N];
        rows[SET_VERBOSE] = (TuiCmd){
            setting_check(scratch, render_verbose(), STR("Verbose tool output")),
            STR("Every line a tool printed, untruncated") };
        rows[SET_RAW] = (TuiCmd){
            setting_check(scratch, md_raw(), STR("Raw Markdown")),
            STR("Replies as the model wrote them, unformatted") };
        rows[SET_STREAM] = (TuiCmd){
            setting_check(scratch, cfg->stream, STR("Stream replies")),
            STR("Paint a reply as it arrives, not once it is whole") };
        rows[SET_TELEMETRY] = (TuiCmd){
            setting_check(scratch, telemetry_on(), STR("Telemetry")),
            STR("An anonymized debug log, for a bug report") };
        rows[SET_MODE] = (TuiCmd){
            setting_value(scratch, STR("Mode")),
            cfg->mode == MODE_PLAN ? STR("Plan: read-only, ends with a plan")
                                   : STR("Build: edits files and runs commands") };
        rows[SET_MODEL] = (TuiCmd){
            setting_value(scratch, STR("Model")), cfg->model };
        rows[SET_PROVIDER] = (TuiCmd){
            setting_value(scratch, STR("Provider")),
            cfg->provider.n ? cfg->provider : STR("none yet") };
        rows[SET_MAX_TOKENS] = (TuiCmd){
            setting_value(scratch, STR("Max tokens")), str_c(tokens) };

        if (!tui_settings(STR("settings"), rows, SET_N, &sel)) break;
        switch (sel) {
            case SET_VERBOSE:
                render_set_verbose(!render_verbose());
                rerender_conv(ag->conv, scratch, 0);
                break;
            case SET_RAW:
                md_set_raw(!md_raw());
                rerender_conv(ag->conv, scratch, 0);
                break;
            case SET_STREAM:
                cfg->stream = !cfg->stream;
                break;
            case SET_TELEMETRY:
                if (telemetry_set(!telemetry_on(), scratch) && telemetry_on())
                    telemetry_session(cfg, ag->tools);
                break;
            case SET_MODE:
                agent_set_mode(ag, cfg->mode == MODE_PLAN ? MODE_BUILD
                                                          : MODE_PLAN);
                break;
            case SET_MODEL:   choose_model(cfg, ag->persist, scratch); break;
            case SET_PROVIDER:choose_provider(cfg, ag->persist, scratch); break;
            case SET_MAX_TOKENS: ask_max_tokens(cfg); break;
            default: break;
        }
    }
    arena_reset(scratch);
}

/* A '!' line runs here rather than reaching the model, and takes a
 * conversation slot of its own, so the model sees what the user ran, a
 * replay renders it and the session keeps it. */
static void run_shell(Agent *ag, Str cmd) {
    Conv *conv = ag->conv;
    arena_reset(ag->scratch);
    cmd = str_trim(cmd);
    if (!cmd.n) {
        tui_notice(STR("no command to run"));
        return;
    }
    Str stored = str_dup(ag->persist, cmd);
    size_t slot = stored.p ? conv_add_shell(conv, stored, (Str){0}) : CONV_NONE;
    if (slot == CONV_NONE) {
        tui_block();
        tui_write(STR("[conversation is full: /clear to start a new one]\n"));
        return;
    }
    render_shell_call(stored, (u32)(slot + 1), false);

    tui_set_status("running shell");
    f64 started = yoke_now_seconds();
    Buf out; buf_init(&out, ag->scratch, 4096);
    char err[256] = {0};
    if (!shell_capture(cmd, &out, err, sizeof err)) {
        if (!err[0]) snprintf(err, sizeof err, "shell failed");
        out.n = 0;
        buf_putf(&out, "ERROR: %s", err);
    }
    tui_set_status("ready");
    Str result = str_dup(ag->persist, buf_finish(&out));
    if (!result.p) result = STR("ERROR: out of memory");
    /* The command is the user's own text, so only its size is recorded. */
    TelEvent e;
    tel_open(&e, "shell");
    tel_shape(&e, "command", cmd);
    tel_int(&e, "ms", (i64)((yoke_now_seconds() - started) * 1000.0));
    tel_shape(&e, "output", result);
    tel_send(&e);
    conv->shell_out[slot] = result;
    render_tool_result(STR("shell"), result, (u32)(slot + 1), false);
    session_save(ag->sess, conv);
    arena_reset(ag->scratch);
}

/* Appends `text` as a user message and streams completions until the model
 * asks for no more tools. False when the turn ended on an error, an
 * interrupt or a full conversation, which is a one-shot run's exit status. */
static b8 agent_turn(Agent *ag, Str text);

/* The conversation that produced the plan is dropped whole and the plan
 * becomes the first message of the next one. It still lives in the scratch
 * arena, which the persistent rewind below does not touch. */
static b8 agent_handoff(Agent *ag) {
    Str plan = ag->handoff;
    ag->handoff = (Str){0};
    ag->conv->n = 1;
    ag->persist->off = ag->mark;
    agent_set_mode(ag, MODE_BUILD);
    session_begin(ag->sess);
    tui_clear();
    return agent_turn(ag, plan);
}

static b8 agent_turn(Agent *ag, Str text) {
    Conv *conv = ag->conv;

    Str user_text = str_dup(ag->persist, text);
    if (text.n && !user_text.p) {
        tui_block();
        tui_write(STR("[out of memory: /clear to start a new session]\n"));
        return false;
    }
    if (conv_add(conv, M_USER, user_text) == CONV_NONE) {
        tui_block();
        tui_write(STR("[conversation is full: /clear to start a new one]\n"));
        return false;
    }
    if (ag->echo) tui_write_user(text);
    session_save(ag->sess, conv);

    TelEvent te;
    tel_open(&te, "turn_start");
    tel_shape(&te, "prompt", text);
    tel_int(&te, "messages", (i64)conv->n);
    tel_str(&te, "mode", mode_name(ag->cfg->mode));
    tel_send(&te);
    f64 turn_started = yoke_now_seconds();
    i32 rounds = 0;

    /* The composer stays editable throughout; only submitting waits. */
    b8 ok = false;
    g_got_sigint = 0;
    tui_set_busy(true);
    /* No round cap: it would end a long build in the middle of itself, and a
     * user who walked away from one is not there to lift it. Stopping the
     * agent is theirs to do, through Ctrl-C. */
    for (;;) {
        rounds++;
        if (g_got_sigint) {
            tui_block();
            tui_write(STR("[interrupted]\n"));
            tui_set_status("ready");
            g_got_sigint = 0;
            break;
        }
        tui_set_status("thinking");
        g_reasoning = false;
        g_replying = false;
        Provider p = {
            .cfg = ag->cfg,
            .tools = ag->tools,
            .conv = conv,
            .persist = ag->persist,
            .scratch = ag->scratch,
            .on_text = on_text,
            .on_reason = on_reason,
            .on_tool_call = on_tool_call,
            .on_retry = on_retry,
            .ud = NULL,
            .on_idle = on_idle,
            .idle_fd = tui_input_fd(),
            .interrupt_flag = &g_got_sigint,
        };
        char err[256] = {0};
        arena_reset(ag->scratch);
        size_t before = conv->n;
        i32 rc = provider_run(&p, err, sizeof err);
        /* The completion is whole, so whatever line the renderer held back
         * has no more bytes coming. */
        md_end();
        if (p.usage_valid) tui_set_context_tokens(p.total_tokens);
        if (g_got_sigint) {
            tui_block();
            tui_write(STR("[interrupted]\n"));
            tui_set_status("ready");
            g_got_sigint = 0;
            break;
        }
        if (rc < 0) {
            /* `err` is formatted by yoke itself: a status or a limit, never
             * anything from the conversation. */
            TelEvent ee;
            tel_open(&ee, "error");
            tel_str(&ee, "where", STR("provider"));
            tel_str(&ee, "detail", str_c(err));
            tel_send(&ee);
            tui_block();
            tui_printf("[provider error: %s]\n", err);
            tui_set_status("ready");
            break;
        }
        if (rc == 0) {
            tui_set_status("ready");
            ok = true;
            break;
        }
        /* The turn appended one head slot plus a carrier per call at
         * [before, conv->n); run them straight off the conversation rather
         * than mirroring them into a second, separately capped array. */
        size_t tail = conv->n;
        TurnAction act = run_tool_calls(ag, before, tail);
        if (act == TURN_FULL) { tui_set_status("ready"); break; }
        if (act == TURN_HANDOFF) { ok = true; break; }
        if (act == TURN_DONE) {
            tui_set_status("ready");
            ok = true;
            break;
        }
    }
    tui_set_busy(false);
    session_save(ag->sess, conv);
    tel_open(&te, "turn_end");
    tel_bool(&te, "ok", ok);
    tel_int(&te, "rounds", rounds);
    tel_int(&te, "ms", (i64)((yoke_now_seconds() - turn_started) * 1000.0));
    tel_int(&te, "messages", (i64)conv->n);
    /* The budget a long session runs down; nothing on screen says where the
     * arenas stand. */
    tel_int(&te, "persist_used", (i64)arena_used(ag->persist));
    tel_int(&te, "scratch_used", (i64)arena_used(ag->scratch));
    tel_send(&te);
    if (ag->handoff.n) return agent_handoff(ag);
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
    /* Built up front, so switching mode later is an assignment rather than a
     * file read mid-turn. */
    cfg.plan_prompt = prompt_build_plan(&tools, &persist, &scratch, prompt_err,
                                        sizeof prompt_err);
    if (!cfg.plan_prompt.n) {
        fprintf(stderr, "yoke: %s\n", prompt_err);
        return 2;
    }
    arena_reset(&scratch);
    tools_set_mode(cfg.mode);

    /* Without a resolvable state dir, recall still works for this session and
     * only the on-disk copy is lost. */
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

    /* Without a resolvable data dir the conversation is not persisted. */
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
    if (cfg.provider.n) tui_set_provider(cfg.provider);
    tui_set_commands(g_commands, commands_init());
    tui_set_history(&hist);
    tui_set_interrupt_flag(&g_got_sigint);
    atexit(tui_stop);

    /* After tui_start, since the record names the shape of the terminal. */
    telemetry_init(&scratch);
    telemetry_session(&cfg, &tools);

    Agent agent = {
        .cfg = &cfg, .tools = &tools, .conv = &conv,
        .persist = &persist, .scratch = &scratch, .sess = &sess,
        .mark = session_mark,
        .echo = !opts.have_prompt,
    };

    /* One-shot: the reply is the output and the exit status reports whether
     * the turn completed. */
    if (opts.have_prompt) {
        if (no_provider(&cfg)) {
            tui_stop();
            fprintf(stderr, "yoke: no provider configured; run yoke without "
                            "-p and use /provider to add one\n");
            return 1;
        }
        b8 ok = agent_turn(&agent, opts.prompt);
        tui_stop();
        return ok ? 0 : 1;
    }

    /* The welcome screen names the command instead of a form opening unasked
     * over an empty screen. */
    if (no_provider(&cfg) && tui_is_fullscreen())
        tui_needs_provider(true);

    /* Static, not automatic: a megabyte of stack for a line the composer
     * already holds is a frame that turns a deep call into a crash. */
    static char line[YOKE_LINE_BUF];
    for (;;) {
        size_t ln = 0;
        if (!tui_readline("> ", line, sizeof line, &ln)) break;
        if (ln == 0) { g_got_sigint = 0; continue; }
        if (line[0] == '!') {
            run_shell(&agent, (Str){ line + 1, ln - 1 });
            continue;
        }
        if (line[0] == '/') telemetry_command((Str){ line, ln });
        if (!strcmp(line, "/exit")) break;
        if (!strcmp(line, "/clear")) {
            /* Slot 0 stays; everything else goes. */
            conv.n = 1;
            persist.off = session_mark;
            arena_reset(&scratch);
            session_begin(&sess);   /* the next message starts a new file */
            tui_clear();
            continue;
        }
        if (!strcmp(line, "/mode")) {
            agent_set_mode(&agent, cfg.mode == MODE_PLAN ? MODE_BUILD
                                                         : MODE_PLAN);
            tui_notice(cfg.mode == MODE_PLAN
                       ? STR("plan mode: read-only, and it ends with a plan "
                             "to approve")
                       : STR("build mode: the agent edits files and runs "
                             "commands"));
            continue;
        }
        if (!strcmp(line, "/fork")) {
            fork_session(&sess, &conv);
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
        if (!strcmp(line, "/settings")) {
            choose_settings(&agent);
            continue;
        }
        if (!strncmp(line, "/expand ", 8)) {
            /* Submitted by a click on a block's truncation tail; the id is
             * the slot it was rendered from. */
            unsigned long id = strtoul(line + 8, NULL, 10);
            if (id && id <= conv.n) {
                conv.expanded[id - 1] = !conv.expanded[id - 1];
                rerender_conv(&conv, &scratch, (u32)id);
            }
            continue;
        }
        if (!strcmp(line, "/model")) {
            choose_model(&cfg, &persist, &scratch);
            continue;
        }
        if (!strcmp(line, "/provider")) {
            choose_provider(&cfg, &persist, &scratch);
            continue;
        }
        if (!strcmp(line, "/resume")) {
            resume_session(&sess, &conv, &persist, &scratch, session_mark);
            continue;
        }
        /* A turn with no endpoint is an HTTP 401 the user cannot act on. */
        if (no_provider(&cfg)) {
            tui_notice(NO_PROVIDER_HINT);
            continue;
        }
        agent_turn(&agent, (Str){ line, ln });
    }

    tui_stop();
    return 0;
}
