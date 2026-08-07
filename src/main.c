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
    g_commands[n++] = (TuiCmd){ STR("/fork"), STR("Continue in a copy, leaving this session as it is") };
    g_commands[n++] = (TuiCmd){ STR("/model"), STR("Pick the model") };
    g_commands[n++] = (TuiCmd){ STR("/provider"), STR("Switch provider, or add one") };
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
        render_tool_call(name, args, scratch, (u32)(i + 1), conv->expanded[i]);
        char status[32];
        snprintf(status, sizeof status, "running %.*s", (i32)name.n, name.p);
        tui_set_status(status);
        b8 ok = tools_run(reg, tool, args, scratch, &out, err, sizeof err);
        if (!ok && !err[0]) snprintf(err, sizeof err, "tool failed");
        if (!ok) { out.n = 0; buf_putf(&out, "ERROR: %s", err); }
        Str result = buf_finish(&out);
        Str res_dup = str_dup(persist, result);
        if (result.n && !res_dup.p) res_dup = STR("ERROR: out of memory");
        size_t slot = conv_add_tool(conv, id, res_dup);
        if (slot == CONV_NONE) {
            tui_write(STR("\n[conversation is full: /clear to start a new one]\n"));
            return false;
        }
        render_tool_result(name, res_dup, (u32)(slot + 1),
                           conv->expanded[slot]);
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
            case M_USER:
                if (conv_is_shell(c, i)) {
                    render_shell_call(c->text[i], (u32)(i + 1), c->expanded[i]);
                    render_tool_result(STR("shell"), c->shell_out[i],
                                       (u32)(i + 1), c->expanded[i]);
                    tui_write(STR("\n"));
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
                    /* A live turn separates a reply from the tool output it
                     * followed; a replay is the same transcript. */
                    if (i && c->role[i - 1] == M_TOOL) tui_write(STR("\n"));
                    md_write(c->text[i]);
                    md_end();
                    tui_write(STR("\n"));
                }
                break;
        }
    }
}

/* Replay the transcript after a change to how it renders. `zone` is the block
 * the reader acted on, which keeps its place on screen while everything above
 * it is rebuilt; 0 when the change was not about one block. */
static void rerender_conv(const Conv *conv, Arena *scratch, u32 zone) {
    arena_reset(scratch);
    tui_anchor_zone(zone);
    tui_clear_transcript();
    render_conv(conv, scratch);
    tui_restore_anchor();
    arena_reset(scratch);
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
        if (conv->role[i] == M_USER && !conv_is_shell(conv, i)) count++;
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
        if (conv->role[i] != M_USER || conv_is_shell(conv, i)) continue;
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
    session_fork(sess, conv);
    arena_reset(scratch);
}

/* Continue in a copy of this conversation. Nothing on screen and nothing in
 * the conversation moves: only the file underneath changes, so every message
 * from here lands in a new session and the one it was forked from keeps what
 * it had, where /resume can still find it. */
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

/* A one-line answer built on the stack, which is what most commands leave
 * behind: it lives for the length of the call, and tui_notice copies it. */
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

/* Offer what `cfg`'s endpoint lists at /models. The pick points into
 * `scratch`, so a caller that keeps it copies it out before resetting. False
 * when nothing was listed or nothing was chosen, having said which. */
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

/* Offer what the provider's /models endpoint lists and switch to the chosen
 * one for this session, remembering it for the next. The conversation is
 * untouched: a model change is not part of it. */
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
    /* A model id only means something against the endpoint that served it, so
     * with a provider selected it is remembered on that entry. */
    b8 saved = cfg->provider.n
             ? endpoints_remember_model(cfg->provider, chosen, scratch)
             : config_remember_model(chosen, scratch);
    notice_fmt("model: %.*s%s", (i32)chosen.n, chosen.p,
               saved ? "" : " (not remembered: no state directory)");
    arena_reset(scratch);
}

/* Nothing to talk to: no key, and no endpoint named by a flag, the
 * environment, a config file or the store. The default base URL is a
 * placeholder here, not a destination. */
static b8 no_provider(const Config *cfg) {
    return !cfg->api_key.p && !cfg->base_url_set;
}

/* Make `name` the provider this session and the next ones talk to. The
 * strings are copied into `persist` because the endpoint store they came from
 * lives in the scratch arena, which the next turn rewinds. */
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
    /* A local server needs no key, so an empty answer is a valid one and only
     * the questions above can cancel the form. */
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

/* Offer the providers already stored, plus the entry that creates one. With
 * none stored there is nothing to pick from, so the form opens straight
 * away. */
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

/* A line typed in shell mode ('!' as its first byte): it runs here rather than
 * reaching the model, and takes a conversation slot of its own, so the model
 * sees what the user ran, a replay renders it and the session keeps it. */
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
        tui_write(STR("\n[conversation is full: /clear to start a new one]\n\n"));
        return;
    }
    render_shell_call(stored, (u32)(slot + 1), false);

    tui_set_status("running shell");
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
    conv->shell_out[slot] = result;
    render_tool_result(STR("shell"), result, (u32)(slot + 1), false);
    tui_write(STR("\n"));
    session_save(ag->sess, conv);
    arena_reset(ag->scratch);
}

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
    if (cfg.provider.n) tui_set_provider(cfg.provider);
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

    /* Nothing to talk to and nothing stored to talk to it with. The welcome
     * screen says so and names the command; a form opening unasked over an
     * empty screen is a question the user did not ask yet. */
    if (no_provider(&cfg) && tui_is_fullscreen())
        tui_needs_provider(true);

    /* Static, not automatic: a megabyte of stack for a line the composer
     * already holds is the kind of frame that turns a deep call into a
     * crash. */
    static char line[YOKE_LINE_BUF];
    for (;;) {
        size_t ln = 0;
        if (!tui_readline("> ", line, sizeof line, &ln)) break;
        if (ln == 0) { g_got_sigint = 0; continue; }
        if (line[0] == '!') {
            run_shell(&agent, (Str){ line + 1, ln - 1 });
            continue;
        }
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
        if (!strcmp(line, "/raw")) {
            md_set_raw(!md_raw());
            /* The transcript is a rendering of the conversation, so the
             * setting that produced it applies to what is already on screen
             * too: replay it under the new one. */
            rerender_conv(&conv, &scratch, 0);
            tui_notice(md_raw()
                       ? STR("raw: replies are shown as the model wrote them")
                       : STR("raw: off, Markdown is formatted"));
            continue;
        }
        if (!strncmp(line, "/expand ", 8)) {
            /* A click on a block's truncation tail, which the TUI submits as
             * this command: the id is the slot it was rendered from. */
            unsigned long id = strtoul(line + 8, NULL, 10);
            if (id && id <= conv.n) {
                conv.expanded[id - 1] = !conv.expanded[id - 1];
                rerender_conv(&conv, &scratch, (u32)id);
            }
            continue;
        }
        if (!strcmp(line, "/verbose")) {
            render_set_verbose(!render_verbose());
            rerender_conv(&conv, &scratch, 0);
            tui_notice(render_verbose()
                       ? STR("verbose: tool output is shown in full")
                       : STR("verbose: tool output is truncated"));
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
        /* A turn with no endpoint is an HTTP 401 the user cannot act on, so
         * it is refused where it was asked and the conversation stays empty. */
        if (no_provider(&cfg)) {
            tui_notice(NO_PROVIDER_HINT);
            continue;
        }
        agent_turn(&agent, (Str){ line, ln });
    }

    tui_stop();
    return 0;
}
