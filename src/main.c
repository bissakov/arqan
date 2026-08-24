#define _XOPEN_SOURCE   700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "agent.h"

#include "core.c"
#include "width.c"
#include "json.c"
#include "http.c"
#include "web.c"
#include "paths.c"
#include "spill.c"
#include "media.c"
#include "clipboard.c"
#include "settings.c"
#include "telemetry.c"
#include "history.c"
#include "secrets.c"
#include "endpoints.c"
#include "models.c"
#include "favorites.c"
#include "config.c"
#include "cli.c"
#include "ignore.c"
#include "tools.c"
#include "todo.c"
#include "prompt.c"
#include "provider.c"
#include "catalog.c"
#include "session.c"
#include "tui.c"
#include "context.c"
#include "notify.c"
#include "highlight.c"
#include "render.c"
#include "markdown.c"

#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>

static volatile sig_atomic_t g_got_sigint = 0;
static void on_sigint(i32 sig) {
    (void)sig;
    g_got_sigint = 1;
}

static alignas(64) u8 g_persist[AGENT_PERSIST_BYTES];
static alignas(64) u8 g_scratch[AGENT_ARENA_BYTES];
/* The rows of whichever modal screen is open. Only one is ever open, and its
 * rows have to survive an action that resets the scratch arena to rerender
 * the transcript under them. */
static alignas(64) u8 g_screen[AGENT_SCREEN_BYTES];


static struct {
    TuiCmd v[AGENT_MAX_COMMANDS];
    size_t n;
} g_commands;

static size_t commands_init(b8 images) {
    size_t n = 0;
    g_commands.v[n++] =
        (TuiCmd){STR("/clear"), STR("Start a fresh conversation")};
    g_commands.v[n++] = (TuiCmd){
        STR("/resume"),
        STR("Resume a saved session from this directory, or delete one")};
    g_commands.v[n++] = (TuiCmd){
        STR("/fork"), STR("Continue in a copy, leaving this session as it is")};
    g_commands.v[n++] =
        (TuiCmd){STR("/compact"),
                 STR("Summarize this session and continue in a new one")};
    g_commands.v[n++] =
        (TuiCmd){STR("/model"), STR("Pick the model, from any provider")};
    g_commands.v[n++] =
        (TuiCmd){STR("/provider"), STR("Add, edit or remove a provider")};
    g_commands.v[n++] = (TuiCmd){
        STR("/mode"), STR("Switch between Build and Plan mode (Shift+Tab)")};
    g_commands.v[n++] = (TuiCmd){
        STR("/rewind"), STR("Go back to an earlier message and edit it")};
    g_commands.v[n++] = (TuiCmd){
        STR("/title"),
        STR("Name this session, or `/title auto` to let the small model name it")};
    g_commands.v[n++] =
        (TuiCmd){STR("/copy"), STR("Copy the last response to the clipboard")};
    g_commands.v[n++] =
        (TuiCmd){STR("/todo"), STR("Show the step list for the work in hand")};
    if (images)
        g_commands.v[n++] = (TuiCmd){
            STR("/attach"),
            STR("Attach an image to the next message, by path or from the clipboard (Ctrl-V)")};
    g_commands.v[n++] =
        (TuiCmd){STR("/find"), STR("Search the transcript (Ctrl-R)")};
    g_commands.v[n++] =
        (TuiCmd){STR("/keys"), STR("Show the keyboard shortcuts")};
    g_commands.v[n++] =
        (TuiCmd){STR("/settings"), STR("Change how " AGENT_NAME " behaves")};
    g_commands.v[n++] =
        (TuiCmd){STR("/statusline"), STR("Choose what the status line shows")};
    g_commands.v[n++] = (TuiCmd){
        STR("/about"), STR("About " AGENT_NAME " and its contributors")};
    g_commands.v[n++] = (TuiCmd){
        STR("/help"), STR("Start a conversation about using " AGENT_NAME)};
    g_commands.v[n++] =
        (TuiCmd){STR("/restart"),
                 STR("Restart " AGENT_NAME
                     ", resuming this session only if the setting says so")};
    g_commands.v[n++] = (TuiCmd){STR("/exit"), STR("Quit " AGENT_NAME)};
    g_commands.v[n++] =
        (TuiCmd){STR("/export"), STR("Export this session as Markdown")};
    g_commands.n = n;
    return n;
}


#define ALIAS(a, b) {{(a), sizeof(a) - 1}, {(b), sizeof(b) - 1}}
static const TuiAlias k_aliases[] = {
    ALIAS("/config", "/settings"),
    ALIAS("/new", "/clear"),
    ALIAS("/search", "/find"),
    ALIAS("/quit", "/exit"),
};
#define ALIAS_N (sizeof k_aliases / sizeof k_aliases[0])

#define INFO_ROW(name, desc) \
    {{(name), sizeof(name) - 1}, {(desc), sizeof(desc) - 1}}
static const TuiCmd k_about[] = {
    INFO_ROW(AGENT_NAME " " AGENT_VERSION, "A tiny C17 terminal coding agent"),
    INFO_ROW("Created by", "Alikhan Bissakov"),
    INFO_ROW("Source", "github.com/bissakov/" AGENT_NAME),
    INFO_ROW("Built with", "C17, libc, libcurl and Lexbor"),
    INFO_ROW("Inspired by", "Claude Code, Codex, OpenCode and Pi"),
};
#define ABOUT_N (sizeof k_about / sizeof k_about[0])


static TuiCmd g_keys[AGENT_MAX_KEY_ROWS];
static size_t keys_rows(void) {
    return tui_key_rows(g_keys, sizeof g_keys / sizeof g_keys[0]);
}


static size_t resolve_alias(char *line, size_t ln, size_t cap) {
    for (size_t i = 0; i < ALIAS_N; i++) {
        Str name = k_aliases[i].name;
        if (!str_eq((Str){line, ln}, k_aliases[i].alias)) continue;
        if (name.n + 1 > cap) break;
        memcpy(line, name.p, name.n);
        line[name.n] = '\0';
        return name.n;
    }
    return ln;
}


/* Per-turn stream state; `one_shot` is the run mode and outlives every turn. */
static struct {
    b8 reasoning;
    b8 replying;
    b8 rerender_pending;
    b8 one_shot;
} g_turn;

static void one_shot_diag(const char *kind, Str name, Str text) {
    if (!g_turn.one_shot) return;
    Str head = str_clip_utf8(text, 2048);
    fprintf(stderr, AGENT_NAME ": %s", kind);
    if (name.n) fprintf(stderr, " %.*s", (i32)name.n, name.p);
    if (head.n) fprintf(stderr, ": %.*s", (i32)head.n, head.p);
    if (head.n < text.n)
        fprintf(stderr, " ... [%zu bytes omitted]", text.n - head.n);
    fputc('\n', stderr);
}


static void say_busy(const char *what) {
    tui_set_status(what);
    tui_activity(str_c(what));
}

static void on_reason(Str delta, void *ud) {
    (void)ud;
    if (g_turn.one_shot) return;
    if (!g_turn.reasoning) {
        g_turn.reasoning = true;
        say_busy("reasoning");
        md_set_muted(true);
        tui_block();
    }
    md_write(delta);
}
static void on_text(Str delta, void *ud) {
    (void)ud;
    if (g_turn.one_shot) return;
    if (!g_turn.replying) {
        g_turn.replying = true;
        g_turn.reasoning = false;
        md_set_muted(false);
        tui_set_status("thinking");
        tui_activity(STR("responding"));
        tui_block();
    }
    md_write(delta);
}
static void on_tool_call(i32 idx, Str id, Str name, Str args_delta, void *ud) {
    (void)ud;
    (void)idx;
    (void)id;
    (void)name;
    (void)args_delta;
    say_busy("preparing tool call");
}
/* The gauge behind the status line's context field. It outlives a turn: the
 * fit it holds is what lets the field answer between requests. */
static CtxGauge g_ctx;

/* What the provider's prompt cache is expected to do, and why it would not.
 * Every rewrite of text already sent stamps its cause here as it happens;
 * cache_guard_observe reads the response back against it. One concern, begun
 * with the session and reset wherever the conversation stops being the one
 * the cache holds. */
static CacheGuard g_cache;

static void cache_guard_begin(CacheGuard *g) {
    *g = (CacheGuard){0};
}

/* The first cause of a round stands. A second rewrite in the same round does
 * not make the miss any more expected, and the row names what began it. */
static void cache_guard_cause(CacheGuard *g, CacheCause cause, size_t freed) {
    if (g->cause != CACHE_CAUSE_NONE) return;
    g->cause = cause;
    g->freed_tokens = freed;
}

/* 88412 as "88,412": a rebuild is read as a price, and a price is read in
 * groups. */
static Str fmt_grouped(char *out, size_t cap, size_t v) {
    char digits[24];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    size_t w = 0;
    for (size_t i = n; i-- > 0 && w + 1 < cap;) {
        out[w++] = digits[i];
        if (i && i % 3 == 0 && w + 1 < cap) out[w++] = ',';
    }
    out[w] = '\0';
    return (Str){out, w};
}

/* Said in the transcript rather than in a notice, the way a retry is: it
 * belongs to the turn being read, and it never reaches Conv. */
static void say_cache(const char *kind, Str row, b8 bad) {
    if (g_turn.one_shot) {
        one_shot_diag(kind, (Str){0}, row);
        return;
    }
    tui_block();
    if (bad) {
        tui_write_error(row);
    } else {
        tui_write_muted(row);
    }
    g_turn.replying = false;
    g_turn.reasoning = false;
}

/* One request's answer to what the guard expected. True when the tool loop
 * has to stop: the prefix was rebuilt and nothing declared a rewrite, which
 * is a defect rather than a price.
 *
 * A response carrying no usage measures nothing and leaves the guard where
 * it stood; the next one is still read against the same prefix. */
static b8 cache_guard_observe(CacheGuard *g, size_t prompt_tokens,
                              size_t cache_read, size_t cache_creation) {
    if (!prompt_tokens) return false;
    f64 now = agent_now_seconds();
    if (cache_read) g->armed = true;
    /* The proxy strips cache_control.ttl, so only the five minute lifetime is
     * available: a longer gap than that is a miss nobody caused. */
    if (g->last_send_s && now - g->last_send_s > AGENT_CACHE_TTL_S)
        cache_guard_cause(g, CACHE_CAUSE_TTL, 0);
    if (!g->expect_tokens) cache_guard_cause(g, CACHE_CAUSE_FIRST, 0);

    b8 miss =
        g->armed && g->expect_tokens && cache_read < g->expect_tokens * 9 / 10;
    CacheCause cause = g->cause;
    size_t freed = g->freed_tokens, expect = g->expect_tokens;
    size_t wasted =
        cache_creation ? cache_creation : prompt_tokens - cache_read;

    g->expect_tokens = prompt_tokens;
    g->last_send_s = now;
    g->cause = CACHE_CAUSE_NONE;
    g->freed_tokens = 0;
    if (!miss) return false;

    g->misses++;
    g->wasted_tokens += wasted;
    TelEvent te;
    tel_open(&te, "cache_miss");
    tel_int(&te, "expected", (i64)expect);
    tel_int(&te, "read", (i64)cache_read);
    tel_int(&te, "rewritten", (i64)wasted);
    tel_str(&te, "cause", cache_cause_name(cause));
    tel_send(&te);

    char a[24], b[24], c[24];
    char row[256];
    i32 n;
    if (cause != CACHE_CAUSE_NONE) {
        Str name = cache_cause_name(cause);
        Str freed_s = fmt_grouped(a, sizeof a, freed);
        if (freed)
            n = snprintf(row, sizeof row,
                         "[cache rebuilt after %.*s: %.*s tokens freed]\n",
                         (i32)name.n, name.p, (i32)freed_s.n, freed_s.p);
        else
            n = snprintf(row, sizeof row, "[cache rebuilt after %.*s]\n",
                         (i32)name.n, name.p);
        if (n > 0) say_cache("cache", (Str){row, (size_t)n}, false);
        return false;
    }
    Str e = fmt_grouped(a, sizeof a, expect);
    Str r = fmt_grouped(b, sizeof b, cache_read);
    Str w = fmt_grouped(c, sizeof c, wasted);
    n = snprintf(row, sizeof row,
                 "[unexpected cache miss: %.*s expected, %.*s read, %.*s "
                 "rewritten\nstopped. this is a bug: %s]\n",
                 (i32)e.n, e.p, (i32)r.n, r.p, (i32)w.n, w.p, AGENT_ISSUES_URL);
    if (n > 0) say_cache("cache", (Str){row, (size_t)n}, true);
    return !g_turn.one_shot;
}

/* Every image this session holds. The table outlives a turn and is indexed
 * by the conversation, which is what lets a slot keep its attachments
 * through a replay; the bytes behind it live in the conversation's own
 * region, so a /clear or a resume releases them with it. */
static MediaSet g_media;

/* The context the request carried, heard from the response while it streams:
 * an interrupt cannot take it back, since nothing behind it is lost to the
 * turn having ended early. The conversation is still the one the request was
 * built from, the reply being appended only once the stream ends. */
static void on_usage(const Conv *conv, size_t prompt_tokens,
                     size_t completion_tokens, void *ud) {
    (void)completion_tokens;
    (void)ud;
    if (!conv) return;
    ctx_note_usage(&g_ctx, conv, prompt_tokens);
    ctx_sync(&g_ctx, conv);
}
/* Said in the transcript rather than in a notice: it belongs to the turn
 * being read. It never reaches Conv, so a replay does not repeat it. */
static void on_retry(i32 attempt, i32 attempts, i32 delay_ms, Str reason,
                     void *ud) {
    (void)ud;
    if (g_turn.one_shot) {
        char row[256];
        i32 n =
            snprintf(row, sizeof row, "%.*s; retrying (attempt %d of %d, %dms)",
                     (i32)reason.n, reason.p, attempt + 1, attempts, delay_ms);
        if (n > 0)
            one_shot_diag("retry", (Str){0},
                          (Str){row, (size_t)n < sizeof row ? (size_t)n
                                                            : sizeof row - 1});
        return;
    }
    say_busy("retrying");
    tui_block();
    char wait[32];
    if (delay_ms < 1000)
        snprintf(wait, sizeof wait, "%dms", delay_ms);
    else
        snprintf(wait, sizeof wait, "%.1fs", (f64)delay_ms / 1000.0);
    char row[256];
    i32 n =
        snprintf(row, sizeof row, "[%.*s; retrying in %s (attempt %d of %d)]\n",
                 (i32)reason.n, reason.p, wait, attempt + 1, attempts);
    if (n > 0)
        tui_write_error(
            (Str){row, (size_t)n < sizeof row ? (size_t)n : sizeof row - 1});
    g_turn.replying = false;
    g_turn.reasoning = false;
}
static void on_idle(void *ud) {
    (void)ud;
    tui_poll_input();
}

typedef struct {
    Config *cfg;
    ToolRegistry *tools;
    Conv *conv;
    Arena *persist;
    Arena *scratch;
    Session *sess;
    size_t mark; // persist offset a conversation starts at
    /* An approved plan on its way to a session of its own. It lives in the
     * scratch arena until the turn carrying it re-anchors it in persist,
     * which is what lets the conversation it came from be dropped whole. */
    Str handoff;
    /* Process-lifetime grants are neither conversation nor configuration:
     * rewinds and handoffs leave them alone, and no persistence path sees it. */
    u8 permission_grants;
    b8 permission_blocked_one_shot;
    b8 echo;
    b8 show_instructions;
    /* One attempt, or one word about it, per crossing: cleared when the next
     * turn starts, when a compaction succeeds, and as soon as the
     * conversation is back under the threshold. */
    b8 compact_seen;
    /* The same, for the crossing that had nothing old enough to compact.
     * Kept apart because that one is not an attempt: a turn still running
     * grows a tail as it goes, and the next round may well have one. */
    b8 compact_short;
    /* Session writes retry silently after the first warning, then announce
     * the transition back to durable checkpoints once. */
    b8 session_save_failed;

    size_t pending[AGENT_MAX_MEDIA_PER_TURN];
    size_t pending_n;
} Agent;

static b8 save_session(Agent *ag) {
    char err[256] = {0};
    if (session_save(ag->sess, ag->conv, err, sizeof err)) {
        if (ag->session_save_failed) {
            if (g_turn.one_shot)
                one_shot_diag("warning", (Str){0},
                              STR("session saving recovered"));
            else
                tui_notice(STR("session saving recovered"));
        }
        ag->session_save_failed = false;
        return true;
    }
    if (!ag->session_save_failed) {
        char msg[384];
        i32 n = snprintf(msg, sizeof msg,
                         "session was not saved: %s; it remains in memory",
                         err[0] ? err : "unknown persistence failure");
        Str text = {msg, n > 0 && (size_t)n < sizeof msg ? (size_t)n
                                                         : sizeof msg - 1};
        if (g_turn.one_shot)
            one_shot_diag("warning", (Str){0}, text);
        else
            tui_notice(text);
    }
    ag->session_save_failed = true;
    return false;
}

static Str mode_name(AgentMode m) {
    return m == MODE_PLAN ? STR("plan") : STR("build");
}

static Str permission_name(PermissionPolicy policy) {
    return policy == PERMISSION_FREE ? STR("free") : STR("ask");
}


static void telemetry_session(const Config *cfg, const ToolRegistry *tools) {
    TelEvent e;
    tel_open(&e, "session");
    tel_str(&e, "version", STR(AGENT_VERSION));
    tel_str(&e, "model", cfg->model);
    tel_str(&e, "provider", cfg->provider);
    tel_str(&e, "mode", mode_name(cfg->mode));
    tel_bool(&e, "permissions_free", cfg->permissions == PERMISSION_FREE);
    tel_int(&e, "tools", (i64)tools->n);
    size_t off = 0;
    for (size_t i = 0; i < tools->n; i++)
        if (tools_disabled(tools, i)) off++;
    tel_int(&e, "tools_off", (i64)off);
    tel_int(&e, "max_tokens", cfg->max_tokens);
    tel_int(&e, "max_messages", (i64)cfg->max_messages);
    tel_bool(&e, "has_key", cfg->api_key.p != NULL);
    tel_int(&e, "cols", (i64)tui_body_cols());
    tel_bool(&e, "fullscreen", tui_is_fullscreen());
    char cwd[AGENT_MAX_PATH];
    if (getcwd(cwd, sizeof cwd)) tel_hash_field(&e, "cwd", str_c(cwd));
    tel_send(&e);
}

typedef struct {
    const Config *cfg;
    const ToolRegistry *tools;
} TelHead;

static void telemetry_header(void *ud) {
    const TelHead *h = (const TelHead *)ud;
    telemetry_session(h->cfg, h->tools);
}

typedef enum {
    TURN_CONTINUE,
    TURN_DONE,
    TURN_HANDOFF,
    TURN_FULL,
    TURN_DENIED
} TurnAction;

static void rerender_conv(const Conv *c, const Config *cfg,
                          b8 show_instructions, Arena *scratch, u32 zone);


static void agent_set_mode(Agent *ag, AgentMode mode) {
    TelEvent e;
    tel_open(&e, "mode");
    tel_str(&e, "from", mode_name(ag->cfg->mode));
    tel_str(&e, "to", mode_name(mode));
    tel_send(&e);
    if (ag->cfg->mode != mode) cache_guard_cause(&g_cache, CACHE_CAUSE_MODE, 0);
    ag->cfg->mode = mode;
    if (!conf_remember(CONF_MODE, mode_name(mode), ag->scratch)) {
        if (g_turn.one_shot)
            one_shot_diag("warning", (Str){0},
                          STR("mode changed but was not remembered"));
        else
            tui_notice(STR(
                "setting changed but was not remembered: could not write state"));
    }
    tools_set_mode(mode);
    if (ag->conv->n && ag->conv->role[0] == M_SYSTEM)
        ag->conv->text[0] =
            mode == MODE_PLAN ? ag->cfg->plan_prompt : ag->cfg->system_prompt;
    tui_set_mode(mode);
    if (ag->show_instructions)
        rerender_conv(ag->conv, ag->cfg, true, ag->scratch, 0);
}

static void agent_set_permissions(Agent *ag, PermissionPolicy policy) {
    TelEvent e;
    tel_open(&e, "permissions");
    tel_bool(&e, "free", policy == PERMISSION_FREE);
    tel_send(&e);
    ag->cfg->permissions = policy;
    if (!conf_remember(CONF_PERMISSIONS, permission_name(policy),
                       ag->scratch)) {
        if (g_turn.one_shot)
            one_shot_diag("warning", (Str){0},
                          STR("permissions changed but were not remembered"));
        else
            tui_notice(STR(
                "setting changed but was not remembered: could not write state"));
    }
    tui_set_permissions(policy);
}

/* A failed run answers where its output would have been, since the result
 * reaches the model either way. `dflt` covers a failure that said nothing. */
static void buf_error(Buf *out, const char *err, const char *dflt) {
    out->n = 0;
    buf_putf(out, "ERROR: %s", err[0] ? err : dflt);
}

/* The result as the conversation keeps it. An arena that could not take it
 * still answers the call, since a call left unanswered is a conversation the
 * provider refuses. */
static Str keep_result(Arena *persist, Str result) {
    Str kept = str_dup(persist, result);
    return kept.p ? kept : STR("ERROR: out of memory");
}

/* The one thing a conversation with no room left has to say, in the
 * transcript because it answers the message that did not fit. */
static void say_conv_full(void) {
    if (g_turn.one_shot) {
        one_shot_diag("error", (Str){0}, STR("conversation is full"));
        return;
    }
    tui_block();
    tui_write(STR("[conversation is full: /clear to start a new one]\n"));
}

static b8 add_result(Agent *ag, size_t call, Str name, Str result, u32 ms) {
    Conv *conv = ag->conv;
    size_t slot = conv_add_tool(conv, conv->tool_call_id[call], result);
    if (slot == CONV_NONE) {
        say_conv_full();
        return false;
    }
    conv->ms[slot] = ms;
    if (g_turn.one_shot)
        one_shot_diag("tool result", name, result);
    else
        render_tool_result(name, conv->text[call], result, ag->scratch,
                           (u32)(slot + 1), conv->expanded[slot], ms);
    /* Saved per result, not per turn: a build that dies in its tenth round
     * must still be resumable up to its ninth. */
    save_session(ag);
    return true;
}

/* Milliseconds since `started`, saturating: a run long enough to overflow one
 * is a number nobody reads anyway. Never 0, which is reserved for a slot
 * whose work was never timed, so a run too fast to measure still reads as one
 * that was. */
static u32 elapsed_ms(f64 started) {
    f64 ms = (agent_now_seconds() - started) * 1000.0;
    if (ms < 1.0) return 1;
    return ms > (f64)UINT32_MAX ? UINT32_MAX : (u32)ms;
}

/* The rows are the options the model offered, the list opens on the one it
 * recommends, and a last row hands the composer over for an answer it did
 * not think of. A recommended option is also what the question answers
 * itself with once ask_timeout_ms passes unanswered, so a user who stepped
 * away costs a wait rather than the provider's whole prompt cache. Empty
 * when the question was dismissed. */
static Str ask_user_answer(Agent *ag, Str args) {
    JVal *j = json_parse(ag->scratch, args);
    Str question = json_str(j, STR("question"));
    const JVal *opts = j ? json_get(j, STR("options")) : NULL;
    size_t n = opts && opts->type == J_ARR ? opts->u.arr.n : 0;
    if (n > AGENT_MAX_POPUP - 1) n = AGENT_MAX_POPUP - 1;

    size_t at = tui_transcript_pos();
    if (!g_turn.one_shot) render_question(question);

    TuiCmd *items = arena_new(ag->scratch, TuiCmd, n + 1);
    if (!items) return (Str){0};
    size_t start = 0;
    b8 recommended = false;
    for (size_t i = 0; i < n; i++) {
        const JVal *o = json_at(opts, i);
        Str label = json_str(o, STR("label"));
        Str detail = json_str(o, STR("detail"));
        if (json_bool(o, STR("recommended"))) {
            start = i;
            recommended = true;
            Buf b;
            buf_init(&b, ag->scratch, detail.n + 24);
            buf_puts(&b, STR("recommended"));
            if (detail.n) {
                buf_puts(&b, STR(" \u00b7 "));
                buf_puts(&b, detail);
            }
            if (buf_ok(&b)) detail = buf_finish(&b);
        }
        items[i] = (TuiCmd){label, detail};
    }
    items[n] =
        (TuiCmd){STR("+ something else"), STR("Answer in your own words")};


    i32 wait_ms = recommended ? ag->cfg->ask_timeout_ms : 0;
    if (wait_ms > 0) {
        char hint[96];
        i32 hn = snprintf(hint, sizeof hint,
                          "no answer in %ds picks the recommended option",
                          wait_ms / 1000 > 0 ? wait_ms / 1000 : 1);
        if (hn > 0 && (size_t)hn < sizeof hint)
            tui_notice((Str){hint, (size_t)hn});
    }

    size_t pick = 0;
    b8 expired = false;
    tui_keep_visible(at);
    if (!tui_pick_timed(STR("pick an answer"), question, items, n + 1,
                        TUI_PICK_FIRST, start, wait_ms, &pick, &expired))
        return (Str){0};
    if (pick < n) {
        if (!expired) return str_dup(ag->persist, items[pick].name);

        Buf b;
        buf_init(&b, ag->persist, items[pick].name.n + 160);
        buf_puts(&b, items[pick].name);
        buf_puts(&b, STR("\n\n(Nobody answered in time, so the recommended "
                         "option was taken automatically. The user has not "
                         "seen the question: do not read it as approval for "
                         "anything irreversible.)"));
        return buf_ok(&b) ? buf_finish(&b)
                          : str_dup(ag->persist, items[pick].name);
    }

    char typed[512];
    if (!tui_ask(STR("your answer"), false, typed, sizeof typed))
        return (Str){0};
    return str_dup(ag->persist, str_c(typed));
}


static ToolAuthorization
tool_authorization(Agent *ag, ToolApprovalClass approval, size_t at) {
    if (approval == TOOL_APPROVAL_NONE
        || ag->cfg->permissions == PERMISSION_FREE
        || (ag->permission_grants & ((u8)1u << (u8)approval)))
        return TOOL_AUTH_GRANTED;

    Str cls = tools_approval_name(approval);
    if (g_turn.one_shot) {
        one_shot_diag(
            "approval required for assistant", cls,
            STR("guarded call denied in non-interactive Ask mode; configure permissions=free for trusted automation"));
        ag->permission_blocked_one_shot = true;
        return TOOL_AUTH_DENIED;
    }

    tui_activity_end();
    notify_event(NOTIFY_INPUT_NEEDED, cls, 0);
    Str once = STR("Approve only this call");
    Str remembered = STR("Approve this class until the process exits");
    if (approval == TOOL_APPROVAL_BASH) {
        once = STR("Run this shell command");
        remembered = STR("Run future shell commands until the process exits");
    } else if (approval == TOOL_APPROVAL_WRITE) {
        once = STR("Write this file");
        remembered =
            STR("Allow future whole-file writes until the process exits");
    } else if (approval == TOOL_APPROVAL_PATCH) {
        once = STR("Apply this patch");
        remembered = STR("Allow future patches until the process exits");
    }
    TuiCmd items[] = {
        {STR("Yes"), once},
        {STR("Yes and remember"), remembered},
        {STR("No"), STR("Execute nothing and report the denial")},
    };
    char title[32];
    i32 n = snprintf(title, sizeof title, "allow %.*s?", (i32)cls.n, cls.p);
    size_t pick = 2;
    tui_keep_visible(at);
    if (n <= 0
        || !tui_pick((Str){title, (size_t)n}, items, 3, TUI_PICK_FIRST, 0,
                     &pick))
        pick = 2;
    if (pick == 1) ag->permission_grants |= (u8)((u8)1u << (u8)approval);
    return pick < 2 ? TOOL_AUTH_GRANTED : TOOL_AUTH_DENIED;
}

static TurnAction submit_plan_answer(Agent *ag, Str args, Str *result) {
    Str plan = json_str(json_parse(ag->scratch, args), STR("plan"));
    if (!plan.n) {
        *result = STR("ERROR: submit_plan requires a non-empty string plan in "
                      "complete valid JSON. Call submit_plan again with the "
                      "complete plan.");
        return TURN_CONTINUE;
    }
    size_t at = tui_transcript_pos();
    if (!g_turn.one_shot) render_plan(plan);

    const TuiCmd items[] = {
        {STR("Yes"), STR("Switch to Build mode and carry the plan out")},
        {STR("Yes, but from a new session"),
         STR("Start over with the plan as the only context")},
        {STR("No"), STR("Keep planning; say what to change")},
    };
    size_t pick = 2;
    tui_keep_visible(at);
    if (!tui_pick(STR("continue?"), items, 3, TUI_PICK_FIRST, TUI_PICK_NONE,
                  &pick))
        pick = 2;
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

static b8 tool_result_has(Str result, Str needle) {
    if (!needle.n || needle.n > result.n) return false;
    for (size_t i = 0; i + needle.n <= result.n; i++)
        if (!memcmp(result.p + i, needle.p, needle.n)) return true;
    return false;
}

static const char *tool_outcome(Str name, Str result, b8 ran) {
    if (str_eq(name, STR("bash"))) {
        Str marker = STR("\n[exit ");
        for (size_t i = result.n; i >= marker.n; i--) {
            size_t at = i - marker.n;
            if (!memcmp(result.p + at, marker.p, marker.n)) {
                size_t digit = at + marker.n;
                return digit < result.n && result.p[digit] == '0'
                           ? "success"
                           : "command_nonzero";
            }
            if (!at) break;
        }
    }
    if (!str_starts(result, STR("ERROR:"))) return "success";
    if (tool_result_has(result, STR("must be a whole number"))
        || tool_result_has(result, STR("missing ")))
        return "invalid_arguments";
    if (str_eq(name, STR("patch"))
        && (tool_result_has(result, STR("context not found"))
            || tool_result_has(result, STR("context matches"))))
        return "placement_failure";
    if (str_eq(name, STR("patch"))
        && (tool_result_has(result, STR("apply_patch"))
            || tool_result_has(result, STR("file header"))))
        return "unsupported_input";
    if (tool_result_has(result, STR("does not exist"))
        || tool_result_has(result, STR("ERROR: open ")))
        return "missing_path";
    return ran ? "operation_failure" : "invocation_failure";
}

/* Run the carrier slots in [first, last) and append each result. The agent UI
 * tools wait for the user rather than doing work, so they are answered here
 * instead of through tools_run, which cannot reach the screen. */
static TurnAction run_tool_calls(Agent *ag, size_t first, size_t last) {
    Conv *conv = ag->conv;
    ag->permission_blocked_one_shot = false;

    TurnAction pending = TURN_CONTINUE;
    for (size_t i = first; i < last; i++) {
        if (!conv_is_call(conv, i)) continue;
        Str name = conv->tool_name[i];
        Str args = conv->text[i];
        size_t tool = tools_find(ag->tools, name);
        b8 agent_ui =
            str_eq(name, STR("submit_plan")) || str_eq(name, STR("ask_user"));
        if (agent_ui
            && (tool == TOOL_NONE
                || !tools_available(ag->tools, tool, ag->cfg->mode))) {
            char msg[128];
            u8 mode = ag->cfg->mode == MODE_PLAN ? TOOL_IN_PLAN : TOOL_IN_BUILD;
            i32 n =
                tool != TOOL_NONE && !(ag->tools->modes[tool] & mode)
                    ? snprintf(msg, sizeof msg,
                               "ERROR: %.*s is not available in %s mode",
                               (i32)name.n, name.p,
                               ag->cfg->mode == MODE_PLAN ? "plan" : "build")
                    : snprintf(msg, sizeof msg,
                               "ERROR: %.*s is not available in this "
                               "non-interactive session",
                               (i32)name.n, name.p);
            size_t len =
                n > 0 && (size_t)n < sizeof msg ? (size_t)n : sizeof msg - 1;
            Str result = keep_result(ag->persist, (Str){msg, len});
            if (!add_result(ag, i, name, result, 0)) return TURN_FULL;
            continue;
        }

        if (str_eq(name, STR("submit_plan"))) {
            tui_activity_end();
            notify_event(NOTIFY_INPUT_NEEDED, STR("a plan is ready to review"),
                         0);
            Str result = {0};
            TurnAction act = submit_plan_answer(ag, args, &result);
            if (!add_result(ag, i, STR("plan"), result, 0)) return TURN_FULL;
            if (act != TURN_CONTINUE) pending = act;
            continue;
        }
        if (str_eq(name, STR("ask_user"))) {
            tui_activity_end();
            notify_event(NOTIFY_INPUT_NEEDED,
                         STR("the assistant asked a question"), 0);
            Str answer = ask_user_answer(ag, args);
            b8 dismissed = !answer.n;
            if (dismissed)
                answer = STR("The user dismissed the question without "
                             "answering. Stop and wait for their next "
                             "message.");
            if (!add_result(ag, i, STR("ask"), answer, 0)) return TURN_FULL;
            if (dismissed) pending = TURN_DONE;
            continue;
        }
        Buf out;
        buf_init(&out, ag->scratch, 4096);
        char err[AGENT_TOOL_ERR] = {0};
        if (g_turn.one_shot) one_shot_diag("tool call", name, args);
        size_t call_at = tui_transcript_pos();
        if (!g_turn.one_shot)
            render_tool_call(name, args, ag->scratch, (u32)(i + 1),
                             conv->expanded[i], conv, i);
        ToolApprovalClass approval = TOOL_APPROVAL_NONE;
        if (tool != TOOL_NONE && !tools_disabled(ag->tools, tool)
            && tools_available(ag->tools, tool, ag->cfg->mode))
            approval = tools_approval_class(ag->tools, tool);
        ToolAuthorization authorization =
            tool_authorization(ag, approval, call_at);
        if (authorization == TOOL_AUTH_DENIED
            && approval != TOOL_APPROVAL_NONE) {
            Str cls = tools_approval_name(approval);
            (void)tools_run(ag->tools, tool, args, authorization, ag->scratch,
                            &out, err, sizeof err);
            out.n = 0;
            buf_putf(&out,
                     "DENIED: the user did not approve this %.*s call. "
                     "Do not retry it blindly.",
                     (i32)cls.n, cls.p);
            Str result = buf_finish(&out);
            if (!add_result(ag, i, name, keep_result(ag->persist, result), 0))
                return TURN_FULL;
            if (ag->permission_blocked_one_shot) return TURN_DENIED;
            continue;
        }
        char status[32];
        snprintf(status, sizeof status, "running %.*s", (i32)name.n, name.p);
        say_busy(status);
        f64 started = agent_now_seconds();
        b8 ok = tools_run(ag->tools, tool, args, authorization, ag->scratch,
                          &out, err, sizeof err);
        if (!ok) buf_error(&out, err, "tool failed");
        Str result = buf_finish(&out);

        TelEvent e;
        tel_open(&e, "tool");
        tel_str(&e, "name", name);
        const char *outcome = tool_outcome(name, result, ok);
        tel_str(&e, "outcome", (Str){outcome, strlen(outcome)});
        tel_bool(&e, "known", tool != TOOL_NONE);
        tel_int(&e, "args_bytes", (i64)args.n);
        tel_arg_keys(&e, "args", args, ag->scratch);
        u32 ms = elapsed_ms(started);
        tel_int(&e, "ms", (i64)ms);
        tel_bool(&e, "ok", ok);
        tel_shape(&e, "result", result);
        tel_send(&e);
        if (!add_result(ag, i, name, keep_result(ag->persist, result), ms))
            return TURN_FULL;
    }
    return pending;
}


static size_t call_slot(const Conv *c, size_t result) {
    for (size_t i = result; i-- > 0;)
        if (conv_is_call(c, i)
            && str_eq(c->tool_call_id[i], c->tool_call_id[result]))
            return i;
    return CONV_NONE;
}


static void render_instruction_source(Str label, Str path, Str text) {
    tui_block();
    tui_write_tool(STR("\u25c6  "));
    tui_write_tool(label);
    if (path.n) {
        tui_write_tool(STR(" · "));
        tui_write_tool(path);
    }
    tui_write_tool(STR("\n"));
    tui_write(text);
    tui_write(STR("\n"));
}

static void render_instructions(const Config *cfg) {
    const PromptSources *sources =
        cfg->mode == MODE_PLAN ? &cfg->plan_sources : &cfg->system_sources;
    tui_block();
    tui_write_tool(cfg->mode == MODE_PLAN ? STR("Instructions · Plan\n")
                                          : STR("Instructions · Build\n"));
    render_instruction_source(sources->primary_label, sources->primary_path,
                              sources->primary);
    for (size_t i = sources->n_agents; i-- > 0;)
        render_instruction_source(STR("AGENTS.md"), sources->agent_paths[i],
                                  sources->agents[i]);
}

/* One row per image the turn carries, inside the user panel and under the
 * text that refers to them: the transcript is cells, so an image is named
 * rather than drawn. */
static void render_user_media(const Conv *c, size_t i) {
    if (!c->media || !c->media_n[i]) return;
    for (size_t k = 0; k < c->media_n[i]; k++) {
        size_t id = (size_t)c->media_off[i] + k;
        if (id >= c->media->n) break;
        char what[64];
        media_describe(what, sizeof what, c->media, id);
        char row[192];
        i32 n =
            snprintf(row, sizeof row, "\n[Image #%zu] %.*s - %s", k + 1,
                     (i32)c->media->label[id].n, c->media->label[id].p, what);
        if (n > 0) tui_write_styled((Str){row, (size_t)n}, TUI_QUOTE);
    }
}

static void render_user_message(const Conv *c, size_t i) {
    Str text = c->text[i];
    tui_pin((u32)(i + 1));
    tui_user_begin();
    md_write(text);
    md_end();
    render_user_media(c, i);
    tui_user_end();
}

static void render_saved_thinking(Str raw, Arena *scratch) {
    if (!raw.n) return;
    size_t mark = scratch->off;
    const JVal *blocks = json_parse(scratch, raw);
    if (!blocks || blocks->type != J_ARR) {
        scratch->off = mark;
        return;
    }
    b8 was_muted = md_muted();
    for (size_t i = 0; i < blocks->u.arr.n; i++) {
        const JVal *blk = &blocks->u.arr.items[i];
        if (!str_eq(json_str(blk, STR("type")), STR("thinking"))) continue;
        Str thought = json_str(blk, STR("thinking"));
        if (!thought.n) continue;
        md_set_muted(true);
        tui_block();
        md_write(thought);
        md_end();
        md_set_muted(was_muted);
    }
    scratch->off = mark;
}

static void render_conv(const Conv *c, const Config *cfg, b8 show_instructions,
                        Arena *scratch) {
    for (size_t i = 0; i < c->n; i++) {
        tui_pin((u32)(i + 1));
        switch (c->role[i]) {
            case M_SYSTEM:
                if (show_instructions) render_instructions(cfg);
                break;
            case M_USER:
                if (conv_is_shell(c, i)) {
                    render_shell_call(c->text[i], (u32)(i + 1), c->expanded[i]);
                    render_tool_result(STR("shell"), (Str){0}, c->shell_out[i],
                                       scratch, (u32)(i + 1), c->expanded[i],
                                       c->ms[i]);
                } else {
                    render_user_message(c, i);
                }
                break;
            case M_TOOL: {
                size_t call = call_slot(c, i);
                Str name = call == CONV_NONE ? (Str){0} : c->tool_name[call];
                Str args = call == CONV_NONE ? (Str){0} : c->text[call];
                render_tool_result(name, args, c->text[i], scratch,
                                   (u32)(i + 1), c->expanded[i], c->ms[i]);
            } break;
            case M_ASSISTANT:
                if (conv_is_call(c, i)) {
                    render_tool_call(c->tool_name[i], c->text[i], scratch,
                                     (u32)(i + 1), c->expanded[i], c, i);
                } else {
                    render_saved_thinking(c->anthropic_thinking[i], scratch);
                    if (c->text[i].n) {
                        tui_block();
                        md_write(c->text[i]);
                        md_end();
                    }
                }
                break;
        }
    }
}


static void rerender_conv(const Conv *conv, const Config *cfg,
                          b8 show_instructions, Arena *scratch, u32 zone) {
    arena_reset(scratch);
    if (zone)
        tui_anchor_zone(zone);
    else
        tui_anchor_view();
    tui_batch_begin();
    tui_clear_transcript();
    render_conv(conv, cfg, show_instructions, scratch);
    tui_restore_anchor();
    tui_batch_end();
    arena_reset(scratch);
}

static const char *help_toggle(b8 on) {
    return on ? "on" : "off";
}


static void help_path(Buf *b, const char *label, Str path) {
    buf_putf(b, "- %s: ", label);
    if (!path.n || path.n >= AGENT_MAX_PATH) {
        buf_puts(b, STR("unresolved\n"));
        return;
    }
    buf_json_str(b, path);

    char z[AGENT_MAX_PATH];
    memcpy(z, path.p, path.n);
    z[path.n] = '\0';
    struct stat st;
    if (stat(z, &st) != 0) {
        if (errno == ENOENT)
            buf_puts(b, STR(" - missing\n"));
        else
            buf_putf(b, " - inaccessible: %s\n", strerror(errno));
        return;
    }
    const char *kind = S_ISDIR(st.st_mode)   ? "directory"
                       : S_ISREG(st.st_mode) ? "regular file"
                                             : "other";
    buf_putf(b, " - %s, mode %04o, %s", kind, (unsigned)(st.st_mode & 07777u),
             access(z, W_OK) == 0 ? "writable" : "not writable");
    if (S_ISREG(st.st_mode)) buf_putf(b, ", %lld bytes", (long long)st.st_size);
    buf_putc(b, '\n');
}

static void help_path_candidates(Buf *b, Arena *a, Str name,
                                 const char *label) {
    Str paths[AGENT_MAX_CONFIG_FILES];
    size_t n = paths_config_files(name, a, paths, AGENT_MAX_CONFIG_FILES);
    for (size_t i = 0; i < n; i++) help_path(b, label, paths[i]);
}

/* Existing project files prompt discovery and the path picker can read while
 * walking from the working directory to the root. Missing candidates are
 * summarized by the effective prompt source instead of flooding the report. */
static void help_project_paths(Buf *b, Str cwd) {
    static const char *const suffixes[] = {
        "/." AGENT_NAME "/config.toml",
        "/." AGENT_NAME "/SYSTEM.md",
        "/." AGENT_NAME "/PLAN.md",
        "/AGENTS.md",
        "/.gitignore",
        "/.ignore",
    };
    if (!cwd.n || cwd.n >= AGENT_MAX_PATH || cwd.p[0] != '/') return;
    char dir[AGENT_MAX_PATH], full[AGENT_MAX_PATH];
    memcpy(dir, cwd.p, cwd.n);
    size_t n = cwd.n;
    while (n > 1 && dir[n - 1] == '/') n--;
    for (;;) {
        size_t base = n == 1 ? 0 : n;
        for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
            size_t sn = strlen(suffixes[i]);
            if (base + sn >= sizeof full) continue;
            memcpy(full, dir, base);
            memcpy(full + base, suffixes[i], sn);
            full[base + sn] = '\0';
            struct stat st;
            if (stat(full, &st) == 0)
                help_path(b, "project discovery", (Str){full, base + sn});
        }
        if (n == 1) break;
        while (n > 1 && dir[n - 1] != '/') n--;
        while (n > 1 && dir[n - 1] == '/') n--;
    }
}

static void help_prompt_sources(Buf *b, const char *mode,
                                const PromptSources *s) {
    buf_putf(b, "### %s mode\n", mode);
    if (s->primary_path.n)
        help_path(b, "primary prompt", s->primary_path);
    else
        buf_putf(b, "- primary prompt: %.*s (no file)\n",
                 (i32)s->primary_label.n, s->primary_label.p);
    for (size_t i = 0; i < s->n_agents; i++)
        help_path(b, "project instructions", s->agent_paths[i]);
}

static Str help_build(Agent *ag) {
    Config *cfg = ag->cfg;
    ToolRegistry *tools = ag->tools;
    Arena *a = ag->scratch;
    Buf b;
    buf_init(&b, a, 16384);
    buf_puts(
        &b,
        STR("# " AGENT_NAME " help context\n\n"
            "I am using " AGENT_NAME " " AGENT_VERSION
            ", a C17 terminal coding agent. "
            "This is a live snapshot generated by /help. Help me understand, "
            "configure, or troubleshoot " AGENT_NAME
            " using this context. Never "
            "reveal "
            "or request API key values.\n\n"
            "## Effective configuration\n"));
    buf_putf(&b, "- mode: %s\n", cfg->mode == MODE_PLAN ? "plan" : "build");
    buf_putf(&b, "- permissions: %s\n",
             cfg->permissions == PERMISSION_FREE ? "free" : "ask");
    buf_putf(&b, "- API: %.*s\n", (i32)api_name(cfg->api).n,
             api_name(cfg->api).p);
    buf_putf(
        &b, "- provider serving it: %.*s\n",
        (i32)(cfg->provider.n ? cfg->provider.n : STR("unnamed override").n),
        cfg->provider.n ? cfg->provider.p : STR("unnamed override").p);
    buf_putf(&b, "- model: %.*s\n", (i32)cfg->model.n, cfg->model.p);
    buf_puts(&b, STR("- base URL: "));
    buf_json_str(&b, cfg->base_url);
    buf_putc(&b, '\n');
    buf_putf(&b, "- active API key: %s\n",
             cfg->api_key.n ? "present" : "missing");
    if (cfg->provider.n) {
        Str src = secret_source_name(endpoints_key_source(cfg->provider, a));
        buf_putf(&b, "- API key store: %.*s\n", (i32)src.n, src.p);
    }
    buf_putf(&b, "- streaming: %s\n", help_toggle(cfg->stream));
    buf_putf(&b, "- verbose tool output: %s\n", help_toggle(render_verbose()));
    buf_putf(&b, "- raw Markdown: %s\n", help_toggle(md_raw()));
    buf_putf(&b, "- ignored files in picker: %s\n",
             help_toggle(tui_show_ignored()));
    buf_putf(&b, "- show instructions: %s\n",
             help_toggle(ag->show_instructions));
    buf_putf(&b, "- telemetry: %s\n", help_toggle(telemetry_on()));
    buf_putf(&b, "- text wrap: %s\n", tui_justify() ? "justified" : "word");
    buf_putf(&b, "- max reply tokens: %d\n", cfg->max_tokens);
    buf_putf(&b, "- max conversation messages: %zu\n", cfg->max_messages);
    if (cfg->context_window)
        buf_putf(&b, "- context window: %zu tokens\n", cfg->context_window);
    else
        buf_puts(&b, STR("- context window: not configured, so automatic "
                         "compaction never fires and /compact sizes its "
                         "tail from the conversation\n"));
    buf_putf(&b,
             "- compact context: %s, at %u%% of the window, with the %s "
             "model\n",
             cfg->compact == COMPACT_OFF      ? "off"
             : cfg->compact == COMPACT_MANUAL ? "manual"
                                              : "auto",
             cfg->compact_at, cfg->compact_small ? "small" : "main");
    buf_putf(&b, "- retries after an empty response: %d\n", cfg->retries);
    buf_putf(&b, "- initial retry delay: %d ms\n", cfg->retry_delay_ms);
    buf_putf(&b, "- reasoning effort: %.*s\n",
             (i32)(cfg->reasoning_effort.n ? cfg->reasoning_effort.n : 3u),
             cfg->reasoning_effort.n ? cfg->reasoning_effort.p : "off");
    buf_putf(&b, "- thinking budget: %.*s\n",
             (i32)(cfg->thinking_budget.n ? cfg->thinking_budget.n : 3u),
             cfg->thinking_budget.n ? cfg->thinking_budget.p : "off");

    static const char *const status_names[TUI_STATUS_N] = {
        "state",
        "model",
        "reasoning effort",
        "thinking budget",
        "mode",
        "provider",
        "working directory",
        "context tokens",
        "copy confirmation",
        "permissions",
    };
    buf_puts(&b, STR("\n### Status fields\n"));
    for (size_t i = 0; i < TUI_STATUS_N; i++)
        buf_putf(&b, "- %s: %s\n", status_names[i],
                 help_toggle(tui_status_visible((TuiStatusItem)i)));

    buf_puts(&b, STR("\n### Tools\n"));
    for (size_t i = 0; i < tools->n; i++) {
        const char *state = tools_disabled(tools, i) ? "disabled"
                            : tools_available(tools, i, cfg->mode)
                                ? "enabled"
                                : "unavailable in this mode";
        buf_putf(&b, "- %.*s: %s\n", (i32)tools->name[i].n, tools->name[i].p,
                 state);
    }

    buf_puts(&b, STR("\n### Slash commands\n"));
    for (size_t i = 0; i < g_commands.n; i++)
        buf_putf(&b, "- %.*s: %.*s\n", (i32)g_commands.v[i].name.n,
                 g_commands.v[i].name.p, (i32)g_commands.v[i].desc.n,
                 g_commands.v[i].desc.p);
    buf_puts(&b, STR("\nWhile a turn is running, /settings, /statusline, "
                     "/about and /copy are submitted where they stand; any "
                     "other command and any message wait in the composer "
                     "until the turn ends.\n"));
    buf_puts(&b, STR("\n### Command aliases\n"));
    for (size_t i = 0; i < ALIAS_N; i++)
        buf_putf(&b, "- %.*s -> %.*s\n", (i32)k_aliases[i].alias.n,
                 k_aliases[i].alias.p, (i32)k_aliases[i].name.n,
                 k_aliases[i].name.p);

    buf_puts(&b, STR("\n## Configured providers\n"));
    Endpoints endpoints;
    endpoints_load(&endpoints, a);
    if (!endpoints.n) buf_puts(&b, STR("- none\n"));
    for (size_t i = 0; i < endpoints.n; i++) {
        size_t mark = a->off;
        char key_err[AGENT_MAX_PATH + 96] = {0};
        Str key =
            endpoints_key(endpoints.name[i], a, a, key_err, sizeof key_err);
        b8 has_key = key.n != 0;
        a->off = mark;
        buf_putf(&b, "### %.*s%s\n", (i32)endpoints.name[i].n,
                 endpoints.name[i].p,
                 str_eq(endpoints.name[i], cfg->provider)
                     ? " (serving the chosen model)"
                     : "");
        buf_putf(&b, "- API: %.*s\n", (i32)api_name(endpoints.api[i]).n,
                 api_name(endpoints.api[i]).p);
        buf_puts(&b, STR("- base URL: "));
        buf_json_str(&b, endpoints.base_url[i]);
        buf_putc(&b, '\n');
        buf_putf(&b, "- default model: %.*s\n", (i32)endpoints.model[i].n,
                 endpoints.model[i].p);
        buf_putf(&b, "- API key: %s\n",
                 key_err[0] ? key_err
                 : has_key  ? "present"
                            : "missing");
    }

    buf_puts(&b, STR("\n## Effective prompt sources\n"));
    help_prompt_sources(&b, "Build", &cfg->system_sources);
    help_prompt_sources(&b, "Plan", &cfg->plan_sources);

    buf_puts(&b, STR("\n## Resolved paths\n"));
    char cwd_buf[AGENT_MAX_PATH];
    Str cwd = getcwd(cwd_buf, sizeof cwd_buf) ? str_c(cwd_buf) : (Str){0};
    help_path(&b, "working directory", cwd);
    help_path(&b, "user config directory", paths_dir(AGENT_DIR_CONFIG, a));
    help_path_candidates(&b, a, AGENT_CONFIG_NAME, "config candidate");
    {
        Str project[AGENT_MAX_PROJECT_FILES];
        size_t pn = paths_project_files(AGENT_CONFIG_NAME, a, project,
                                        AGENT_MAX_PROJECT_FILES);
        for (size_t i = 0; i < pn; i++)
            help_path(&b, "project config", project[i]);
    }
    help_path_candidates(&b, a, STR("SYSTEM.md"),
                         "global Build prompt candidate");
    help_path_candidates(&b, a, STR("PLAN.md"), "global Plan prompt candidate");
    help_path(&b, "state directory", paths_dir(AGENT_DIR_STATE, a));
    help_path(&b, "state file",
              paths_file(AGENT_DIR_STATE, AGENT_STATE_NAME, a));
    help_path(&b, "credentials file",
              paths_file(AGENT_DIR_STATE, AGENT_CREDENTIALS_NAME, a));
    help_path(&b, "prompt history", history_path(a, a));
    help_path(&b, "telemetry root",
              paths_file(AGENT_DIR_STATE, STR("telemetry"), a));
    help_path(&b, "data directory", paths_dir(AGENT_DIR_DATA, a));
    help_path(&b, "session directory", ag->sess->dir);
    if (ag->sess->path.n) {
        buf_puts(&b, STR("- current session file: "));
        buf_json_str(&b, ag->sess->path);
        buf_puts(&b, STR(" - reserved; created when this message is saved\n"));
    }
    help_project_paths(&b, cwd);

    buf_puts(
        &b,
        STR("\nThis snapshot intentionally contains no API key values. Wait for "
            "my next message before taking action.\n"));
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}


static void start_help_session(Agent *ag) {
    Conv *conv = ag->conv;
    conv_truncate(conv, 1);
    cache_guard_begin(&g_cache);
    ag->pending_n = 0;
    ag->persist->off = ag->mark;
    arena_reset(ag->scratch);
    session_begin(ag->sess);
    tui_clear();

    Str built = help_build(ag);
    Str prompt = str_dup_opt(ag->persist, built);
    arena_reset(ag->scratch);
    if (!prompt.p) {
        tui_notice(STR("out of memory building help context"));
        return;
    }
    if (conv_add(conv, M_USER, prompt) == CONV_NONE) {
        tui_notice(STR("conversation is full"));
        return;
    }
    render_user_message(conv, conv->n - 1);
    save_session(ag);
}


typedef struct {
    Agent *ag;
    SessionList list;
    TuiCmd *rows;
    size_t n;
    size_t armed;
    b8 has_armed;
    size_t deleted;
} SessionPick;

/* Ctrl-X on a session row. The first press arms the row and says so; the
 * second deletes the file and drops the row, which is what the picker
 * redraws. Anything that refuses answers in the notice slot and leaves the
 * list as it was, since a screen that closed on an error is an error the
 * reader has to go looking for. */
static size_t session_delete_row(void *ud, size_t row, size_t *moved) {
    SessionPick *sp = ud;
    if (row >= sp->n) return sp->n;
    *moved = row;
    Str name = sp->list.name[row];
    char msg[128];

    if (!sp->has_armed || sp->armed != row) {
        sp->has_armed = true;
        sp->armed = row;
        snprintf(msg, sizeof msg, "Press Ctrl-X again to delete %.*s",
                 (i32)name.n, name.p);
        tui_notice(str_c(msg));
        return sp->n;
    }
    sp->has_armed = false;
    if (sp->ag->sess->path.n
        && str_eq(sp->list.path[row], sp->ag->sess->path)) {
        tui_notice(STR("that session is the one running: /clear first"));
        return sp->n;
    }
    if (!session_delete(sp->ag->sess, sp->list.path[row])) {
        tui_notice(STR("could not delete that session"));
        return sp->n;
    }

    sp->n--;
    sp->deleted++;
    for (size_t i = row; i < sp->n; i++) {
        sp->list.name[i] = sp->list.name[i + 1];
        sp->list.path[i] = sp->list.path[i + 1];
        sp->list.preview[i] = sp->list.preview[i + 1];
        sp->rows[i] = sp->rows[i + 1];
    }
    sp->list.n = sp->n;
    *moved = row < sp->n ? row : (sp->n ? sp->n - 1 : 0);
    snprintf(msg, sizeof msg, "deleted session: %.*s", (i32)name.n, name.p);
    tui_notice(str_c(msg));
    return sp->n;
}

/* Nothing to open leaves the view exactly as it was and answers in the
 * popup's own slot: a session that did not open is not part of the
 * conversation, so it has no business in the transcript.
 *
 * The scratch arena is rewound on the way in rather than at each way out,
 * since every one of its consumers starts by rewinding it. */
static void resume_session(Agent *ag) {
    Session *sess = ag->sess;
    Conv *conv = ag->conv;
    Arena *persist = ag->persist;
    Arena *scratch = ag->scratch;
    size_t session_mark = ag->mark;
    arena_reset(scratch);
    SessionPick sp = {ag, {0}, NULL, 0, 0, false, 0};
    size_t n = session_list(sess, scratch, &sp.list, AGENT_MAX_SESSIONS);
    if (!n) {
        tui_notice(STR("no saved sessions in this directory"));
        return;
    }
    TuiCmd *items = arena_new(scratch, TuiCmd, n);
    if (!items) {
        tui_notice(STR("out of memory listing sessions"));
        return;
    }

    for (size_t i = 0; i < n; i++) {
        if (!sp.list.title[i].n) {
            items[i] = (TuiCmd){sp.list.name[i], sp.list.preview[i]};
            continue;
        }
        Buf desc;
        buf_init(&desc, scratch, sp.list.name[i].n + sp.list.preview[i].n + 8);
        buf_puts(&desc, sp.list.name[i]);
        if (sp.list.preview[i].n) {
            buf_puts(&desc, STR(" \xc2\xb7 "));
            buf_puts(&desc, sp.list.preview[i]);
        }
        items[i] =
            (TuiCmd){sp.list.title[i],
                     buf_ok(&desc) ? buf_finish(&desc) : sp.list.preview[i]};
    }
    sp.rows = items;
    sp.n = n;

    /* Deleting is a key on the list rather than an entry at the end of it:
     * the row that removes a session is the one the reader is looking at,
     * and a directory with a hundred sessions never scrolls it out of
     * reach. */
    size_t pick = 0;
    TuiPickBinding binding = {session_delete_row, &sp, 0x18};
    TuiPickAction act = {items, n, &binding, 1,
                         STR("Ctrl-X deletes the selected session")};
    b8 chosen = tui_pick_action(STR("pick a session"), n, n, TUI_PICK_FIRST,
                                TUI_PICK_NONE, &act, &pick);
    /* The screen restores the notice slot it borrowed, so what the deletes
     * did is said again once it has closed. */
    if (sp.deleted && !chosen) {
        char msg[64];
        snprintf(msg, sizeof msg, "deleted %zu saved session%s", sp.deleted,
                 sp.deleted == 1 ? "" : "s");
        tui_notice(str_c(msg));
    }
    if (!chosen || pick >= sp.n) return;

    /* Read first: replaying overwrites the live conversation's storage, so a
     * session that cannot be read must not cost the one that is running. */
    Str src = session_read(sp.list.path[pick], scratch);
    if (!src.n) {
        tui_notice(STR("could not read that session"));
        return;
    }
    conv_truncate(conv, 1);
    cache_guard_begin(&g_cache);
    ag->pending_n = 0;
    persist->off = session_mark;
    b8 whole = session_apply(sess, src, sp.list.path[pick], sp.list.name[pick],
                             conv, persist, scratch);
    tui_batch_begin();
    tui_clear();
    render_conv(conv, ag->cfg, ag->show_instructions, scratch);
    tui_batch_end();
    b8 saved = save_session(ag);
    if (!whole && saved)
        tui_notice(STR("session truncated: the conversation is full"));
}

/* Reopen this directory's newest session, for a start that was configured to
 * continue rather than to greet. It runs before the screen exists, so
 * nothing is painted here and the caller renders the conversation once the
 * TUI is up; `truncated` says the conversation filled up on the way in.
 * False means there was nothing to reopen, and leaves the session unnamed so
 * the caller reserves a fresh one as usual. */
static b8 resume_latest(Session *sess, Conv *conv, Arena *persist,
                        Arena *scratch, b8 *truncated) {
    *truncated = false;
    arena_reset(scratch);
    SessionList list;
    Str src = {0};
    if (session_list(sess, scratch, &list, 1))
        src = session_read(list.path[0], scratch);
    if (!src.n) {
        arena_reset(scratch);
        return false;
    }
    *truncated = !session_apply(sess, src, list.path[0], list.name[0], conv,
                                persist, scratch);
    arena_reset(scratch);
    return true;
}


#define REWIND_PREVIEW_BYTES 72
static Str preview_line(Arena *a, Str s) {
    char tmp[REWIND_PREVIEW_BYTES];
    size_t n = 0;
    for (size_t i = 0; i < s.n && n < sizeof tmp; i++) {
        u8 c = (u8)s.p[i];
        tmp[n++] = c < 0x20 ? ' ' : (char)c;
    }
    while (n && ((u8)tmp[n - 1] & 0xc0u) == 0x80u) n--;
    Buf b;
    buf_init(&b, a, n + 8);
    buf_put(&b, tmp, n);
    if (s.n > n) buf_puts(&b, STR("..."));
    return buf_ok(&b) ? buf_finish(&b) : (Str){0};
}

/* The chosen message returns to the composer and everything from it onward
 * leaves the conversation. The persistent arena is not rewound: the composer
 * is loaded from the text that is about to be dropped. */
static void rewind_conversation(Agent *ag) {
    Conv *conv = ag->conv;
    Session *sess = ag->sess;
    Arena *scratch = ag->scratch;
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
    size_t skip = count > AGENT_MAX_POPUP ? count - AGENT_MAX_POPUP : 0;
    size_t cap = count - skip;
    size_t *at = arena_new(scratch, size_t, cap);
    TuiCmd *items = arena_new(scratch, TuiCmd, cap);
    if (!at || !items) {
        tui_notice(STR("out of memory listing messages"));
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < conv->n; i++) {
        if (conv->role[i] != M_USER || conv_is_shell(conv, i)) continue;
        if (skip) {
            skip--;
            continue;
        }
        at[n] = i;
        items[n] = (TuiCmd){preview_line(scratch, conv->text[i]), (Str){0}};
        n++;
    }


    size_t pick = 0;
    if (!tui_pick(STR("rewind to a message"), items, n, TUI_PICK_LAST,
                  TUI_PICK_NONE, &pick))
        return;
    size_t slot = at[pick];
    /* The message returns to the composer with its images still attached:
     * the placeholders in the text it restores would otherwise name nothing.
     * They are the table's last live entries once the slot is dropped, and
     * this path rewinds no arena, so the bytes behind them are still there. */
    size_t img_off = conv->media_off[slot], img_n = conv->media_n[slot];
    tui_set_input(conv->text[slot]);
    conv_truncate(conv, slot);
    cache_guard_begin(&g_cache);
    ag->pending_n = 0;
    if (img_n && conv->media && img_n <= AGENT_MAX_MEDIA_PER_TURN) {
        conv->media->n = img_off + img_n;
        for (size_t k = 0; k < img_n; k++)
            ag->pending[ag->pending_n++] = img_off + k;
    }
    tui_batch_begin();
    tui_clear();
    render_conv(conv, ag->cfg, ag->show_instructions, scratch);
    tui_batch_end();

    char err[256] = {0};
    if (!session_fork(sess, conv, err, sizeof err)) {
        /* The old append-only file cannot represent a rewind. Reserve a new
         * destination so the in-memory branch can recover on a later save. */
        session_begin(sess);
        save_session(ag);
    }
}


static void fork_session(Agent *ag) {
    Session *sess = ag->sess;
    const Conv *conv = ag->conv;
    if (conv->n <= 1) {
        tui_notice(STR("nothing to fork yet"));
        return;
    }
    if (!sess->dir.n) {
        tui_notice(STR("sessions are not saved here: nothing to fork"));
        return;
    }
    char err[256] = {0};
    if (!session_fork(sess, conv, err, sizeof err)) {
        char msg[384];
        snprintf(msg, sizeof msg, "could not start a forked session: %s",
                 err[0] ? err : "unknown persistence failure");
        tui_notice(str_c(msg));
        return;
    }
    ag->session_save_failed = false;
    tui_notice(STR("forked: this copy continues, the original is unchanged"));
}

/* The last reply as the Markdown it was written in, which the conversation
 * still holds while the transcript has only a wrapped rendering of it. A slot
 * carrying a tool call holds JSON arguments rather than prose. */
static void copy_last_reply(const Conv *conv) {
    for (size_t i = conv->n; i-- > 0;) {
        if (conv->role[i] != M_ASSISTANT || conv_is_call(conv, i)) continue;
        if (!conv->text[i].n) continue;
        if (!tui_copy(conv->text[i]))
            tui_notice(STR("that response is too large to copy"));
        else
            tui_notice(tui_clipboard_via_tmux()
                           ? AGENT_TMUX_COPY_NOTICE
                           : STR("copied the last response"));
        return;
    }
    tui_notice(STR("no response to copy"));
}

/* The step list as its own screen, since the transcript only holds the
 * checklist as it stood when each call was made. */
static void show_todo(void) {
    /* Static: a screen opened during a turn outlives the frame that opened
     * it, and the rows point into the list, which is static too. */
    static struct {
        TuiCmd rows[AGENT_MAX_TODOS];
    } view;

    const TodoList *l = todo_current();
    if (!l->n) {
        tui_notice(STR("no step list yet; the model writes one for work of "
                       "several steps"));
        return;
    }
    for (size_t i = 0; i < l->n; i++) {
        view.rows[i].name = l->status[i] == TODO_DONE     ? STR("done")
                            : l->status[i] == TODO_ACTIVE ? STR("now")
                                                          : STR("next");
        view.rows[i].desc = todo_text(l, i);
    }
    tui_info(STR("step list"), view.rows, l->n);
}

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
    tui_notice((Str){msg, n});
}


static void attach_notice(const Agent *ag, size_t id) {
    char what[64];
    media_describe(what, sizeof what, &g_media, id);
    notice_fmt("attached [Image #%zu] %.*s - %s", ag->pending_n,
               (i32)g_media.label[id].n, g_media.label[id].p, what);
}


static b8 draft_names_image(Str draft, size_t num) {
    char tag[24];
    i32 n = snprintf(tag, sizeof tag, "[Image #%zu]", num);
    if (n <= 0) return false;
    Str want = {tag, (size_t)n};
    for (size_t i = 0; i + want.n <= draft.n; i++)
        if (str_starts(str_drop(draft, i), want)) return true;
    return false;
}


static b8 image_tag_at(Str text, size_t i, size_t pending_n, size_t *num,
                       size_t *len) {
    static const Str lead = {"[Image #", 8};
    if (i + lead.n >= text.n || memcmp(text.p + i, lead.p, lead.n))
        return false;
    size_t j = i + lead.n, n = 0;
    while (j < text.n && text.p[j] >= '0' && text.p[j] <= '9') {
        n = n * 10 + (size_t)(text.p[j++] - '0');
        if (n > pending_n) return false;
    }
    if (j == i + lead.n || j >= text.n || text.p[j] != ']' || !n) return false;
    *num = n;
    *len = j + 1 - i;
    return true;
}

/* An attachment lives as long as its placeholder, so an image whose marker
 * the user deleted is dropped before the next one is added rather than coming
 * back numbered behind it. `carried` is the marker text a submitted line took
 * out of the composer, which still counts as written: a typed /attach empties
 * the box, a gesture leaves the draft where it was. The survivors and the
 * markers left in the draft are renumbered together, so both count alike. */
static void pending_drop_unnamed(Agent *ag, Str carried) {
    if (!ag->pending_n) return;
    size_t mark = ag->scratch->off;
    Str draft = tui_input();

    b8 named[AGENT_MAX_MEDIA_PER_TURN] = {0};
    size_t live = 0;
    for (size_t i = 0; i < ag->pending_n; i++) {
        named[i] = draft_names_image(draft, i + 1)
                   || draft_names_image(carried, i + 1);
        if (named[i]) live++;
    }
    if (live == ag->pending_n) {
        ag->scratch->off = mark;
        return;
    }

    size_t renum[AGENT_MAX_MEDIA_PER_TURN] = {0};
    size_t kept[AGENT_MAX_MEDIA_PER_TURN];
    size_t n = 0;
    for (size_t i = 0; i < ag->pending_n; i++)
        if (named[i]) {
            kept[n] = ag->pending[i];
            renum[i] = ++n;
        }

    Buf b;
    buf_init(&b, ag->scratch, draft.n + 32);
    for (size_t i = 0; i < draft.n; i++) {
        size_t num, len;
        if (!image_tag_at(draft, i, ag->pending_n, &num, &len)) {
            buf_putc(&b, draft.p[i]);
            continue;
        }
        if (renum[num - 1]) buf_putf(&b, "[Image #%zu]", renum[num - 1]);
        i += len - 1;
    }
    Str line = buf_finish(&b);
    if (buf_ok(&b) && !str_eq(line, draft)) tui_set_input(line);

    size_t base = ag->pending[0];
    if (n)
        media_keep(&g_media, base, kept, n);
    else if (base <= g_media.n)
        g_media.n = base;
    for (size_t i = 0; i < n; i++) ag->pending[i] = base + i;
    ag->pending_n = n;
    ag->scratch->off = mark;
}


static void composer_restore_pending(const Agent *ag) {
    if (!ag->pending_n) return;
    size_t mark = ag->scratch->off;
    Str draft = tui_input();
    Buf b;
    buf_init(&b, ag->scratch, draft.n + 32 * ag->pending_n);
    buf_puts(&b, draft);
    for (size_t i = 0; i < ag->pending_n; i++) {
        if (draft_names_image(draft, i + 1)) continue;
        if (b.n && b.p[b.n - 1] != ' ') buf_putc(&b, ' ');
        buf_putf(&b, "[Image #%zu] ", i + 1);
    }
    Str line = buf_finish(&b);
    if (line.n && !str_eq(line, draft)) tui_set_input(line);
    ag->scratch->off = mark;
}

/* The clipboard's image, added like a file's. The helper's bytes land in
 * `scratch` and media_add refuses a format or a size there exactly as it
 * would from a path, so the two ways to attach answer alike. */
static size_t attach_from_clipboard(Agent *ag, char *err, size_t err_cap) {
    Str bytes;
    if (!clipboard_image(ag->scratch, &bytes, err, err_cap)) return MEDIA_NONE;
    return media_add(&g_media, ag->persist, bytes, STR("clipboard"), err,
                     err_cap);
}

static void attach_image(Agent *ag, Str path, Str carried) {
    path = str_trim(path);

    if (!ag->conv->media) {
        notice_fmt("images are off: set images = auto to attach one");
        return;
    }
    pending_drop_unnamed(ag, carried);

    TelEvent e;
    tel_open(&e, "attach");
    tel_bool(&e, "clipboard", !path.n);
    if (ag->pending_n >= AGENT_MAX_MEDIA_PER_TURN) {
        notice_fmt("a message carries at most %d images; send this one first",
                   AGENT_MAX_MEDIA_PER_TURN);
        composer_restore_pending(ag);
        tel_bool(&e, "ok", false);
        tel_send(&e);
        return;
    }
    char err[256] = {0};
    size_t mark = ag->scratch->off;

    size_t id = path.n ? media_add_file(&g_media, ag->persist, ag->scratch,
                                        path, err, sizeof err)
                       : attach_from_clipboard(ag, err, sizeof err);
    if (id == MEDIA_NONE) {
        ag->scratch->off = mark;
        notice_fmt("%s", err);
        composer_restore_pending(ag);
        tel_bool(&e, "ok", false);
        tel_send(&e);
        return;
    }
    ag->pending[ag->pending_n++] = id;
    composer_restore_pending(ag);
    attach_notice(ag, id);
    ag->scratch->off = mark;
    tel_bool(&e, "ok", true);
    tel_bucket(&e, "kb", g_media.bytes[id].n / 1024);
    tel_bucket(&e, "pixels", (u64)g_media.w[id] * g_media.h[id]);
    tel_int(&e, "pending", (i64)ag->pending_n);
    tel_send(&e);
}


static size_t pending_prefix(const Agent *ag, const char *line, size_t n) {
    Str text = {line, n};
    size_t at = 0;
    while (at < n) {
        size_t num, len;
        if (!image_tag_at(text, at, ag->pending_n, &num, &len)) break;
        at += len;
        while (at < n && line[at] == ' ') at++;
    }
    return at < n && (line[at] == '/' || line[at] == '!') ? at : 0;
}

/* The message as it will be stored, and the images it carries. A placeholder
 * the user deleted detaches its image, which is the only way to take one
 * back; the survivors are renumbered so the text and the blocks beside it
 * count the same way. An image nothing names is appended at the end rather
 * than dropped silently, since a message can also be submitted from a
 * queue that never saw the composer. Empty `.p` means the arena is full. */
static Str turn_bind_images(Agent *ag, Str text, size_t *off, size_t *count) {
    *off = *count = 0;
    if (!ag->pending_n) return str_dup(ag->persist, text);
    size_t mark = ag->scratch->off;

    b8 named[AGENT_MAX_MEDIA_PER_TURN] = {0};
    for (size_t i = 0; i < text.n; i++) {
        size_t num, len;
        if (!image_tag_at(text, i, ag->pending_n, &num, &len)) continue;
        named[num - 1] = true;
        i += len - 1;
    }

    b8 none = true;
    for (size_t i = 0; i < ag->pending_n; i++) none = none && !named[i];
    if (none)
        for (size_t i = 0; i < ag->pending_n; i++) named[i] = true;

    size_t renum[AGENT_MAX_MEDIA_PER_TURN] = {0};
    size_t kept[AGENT_MAX_MEDIA_PER_TURN];
    size_t n = 0;
    for (size_t i = 0; i < ag->pending_n; i++)
        if (named[i]) {
            kept[n] = ag->pending[i];
            renum[i] = ++n;
        }

    Buf b;
    buf_init(&b, ag->scratch, text.n + 32);
    b8 appended[AGENT_MAX_MEDIA_PER_TURN] = {0};
    for (size_t i = 0; i < text.n; i++) {
        size_t num, len;
        if (!image_tag_at(text, i, ag->pending_n, &num, &len)) {
            buf_putc(&b, text.p[i]);
            continue;
        }
        if (renum[num - 1]) {
            buf_putf(&b, "[Image #%zu]", renum[num - 1]);
            appended[num - 1] = true;
        }
        i += len - 1;
    }
    for (size_t i = 0; i < ag->pending_n; i++) {
        if (!renum[i] || appended[i]) continue;
        if (b.n) buf_putc(&b, '\n');
        buf_putf(&b, "[Image #%zu]", renum[i]);
    }
    Str full = buf_finish(&b);

    Str stored = buf_ok(&b) ? str_dup(ag->persist, str_trim(full)) : (Str){0};
    ag->scratch->off = mark;
    if (!stored.p) return (Str){0};
    if (n) *off = media_keep(&g_media, ag->pending[0], kept, n);
    *count = n;
    return stored;
}

static void export_session(const Conv *conv, Str requested) {
    if (conv->n <= 1) {
        tui_notice(STR("nothing to export yet"));
        return;
    }
    char path[AGENT_MAX_PATH];
    char err[256];
    if (!session_export_markdown(conv, requested, path, sizeof path, err,
                                 sizeof err)) {
        notice_fmt("could not export session: %s", err);
        return;
    }
    notice_fmt("exported session to %s", path);
}


static b8 name_session(Agent *ag, b8 manual, b8 *interrupted_out);


static void title_command(Agent *ag, Str arg) {
    Session *sess = ag->sess;
    arg = str_trim(arg);

    if (!sess->path.n || ag->conv->n <= 1) {
        tui_notice(STR("nothing to name yet"));
        return;
    }
    if (str_eq(arg, STR("auto"))) {
        name_session(ag, true, NULL);
        return;
    }
    char buf[AGENT_MAX_TITLE + 1];
    if (!arg.n) {
        snprintf(buf, sizeof buf, "%.*s", (i32)sess->title.n, sess->title.p);
        if (!tui_ask_edit(STR("session title"), true, buf, sizeof buf)) return;
        arg = str_c(buf);
    }
    if (!session_set_title(sess, arg)) {
        tui_notice(STR("could not name this session"));
        return;
    }
    if (!sess->title.n) {
        tui_notice(STR("session title cleared"));
        return;
    }
    notice_fmt("session named: %.*s", (i32)sess->title.n, sess->title.p);
}

static b8 command_offered(Str name) {
    for (size_t i = 0; i < g_commands.n; i++)
        if (str_eq(g_commands.v[i].name, name)) return true;
    return false;
}


static void telemetry_command(Str line) {
    Str word = line;
    for (size_t i = 0; i < line.n; i++)
        if (line.p[i] == ' ') {
            word = str_take(line, i);
            break;
        }
    b8 offered = command_offered(word);
    TelEvent e;
    tel_open(&e, "command");
    tel_str(&e, "name", offered ? word : STR("(unknown)"));
    tel_send(&e);
}

/* ---- /model --------------------------------------------------------------
 * One list, every provider. A model is a (provider, model) pair, because an
 * id belongs to the endpoint that serves it: choosing a row selects the
 * connection as well as the model, and nothing else in a session selects an
 * endpoint. /provider creates connections; this is where they are used.
 */

/* What a key pressed in the picker asked for, answered once it has closed: a
 * question or a form must not open under a popup. */
enum { MODEL_ACTION_NONE, MODEL_ACTION_MANUAL, MODEL_ACTION_CONFIGURE };

/* The picker's rows. `label` is a row's name, built once: the id alone while
 * one endpoint serves the list, and "<id> @ <provider>" once more than one
 * does, since the picker searches names and two providers may serve one id.
 * `starred` is the same name with the pin marker, and `order` maps a visible
 * row back to a catalog entry.
 *
 * Entries at or past `live` came from pins rather than from a listing, which
 * is what keeps the models of an unreachable provider reachable: a pin is
 * local state. One of those whose model is emptied has just been unpinned and
 * is no longer a row.
 */
typedef struct {
    Catalog *cat;
    Str *label;
    Str *starred;
    size_t *order;
    TuiCmd *rows;
    size_t live;
    Favorites fav;
    b8 named;
    Str provider;
    Str current;
    Str small;
    Str small_owner;
    Config *cfg;
    Arena *arena;
    u8 requested;
    size_t acted;
    char msg[192];
} ModelPick;


static b8 edit_model_profile(Config *cfg, Str provider, Str model,
                             Arena *scratch) {
    if (!provider.n || !model.n) {
        tui_notice(STR("model settings need a named provider"));
        return false;
    }
    b8 live = str_eq(provider, cfg->provider) && str_eq(model, cfg->model);
    ModelProfile old;
    model_profile_load(&old, provider, model, scratch, scratch);
    char window[32] = {0};
    char efforts[AGENT_MAX_REASONING_LIST + 1] = {0};
    char budgets[AGENT_MAX_REASONING_LIST + 1] = {0};
    char effort[AGENT_MAX_REASONING_LIST + 1] = {0};
    char budget[AGENT_MAX_REASONING_LIST + 1] = {0};
    char templ[AGENT_MAX_REASONING_TEMPLATE + 1] = {0};
    if (old.context_window)
        snprintf(window, sizeof window, "%zu", old.context_window);
    const TuiCmd modes[] = {
        {STR("Off"), STR("Send no reasoning fields")},
        {STR("Named efforts"), STR("Choose from user-defined string values")},
        {STR("Token budgets"), STR("Choose from user-defined token counts")},
        {STR("Custom JSON"), STR("Add a user-defined request object")},
    };
    size_t mode = old.reasoning_template.n  ? 3
                  : old.reasoning_efforts.n ? 1
                  : old.thinking_budgets.n  ? 2
                                            : 0;
    if (!tui_ask_edit(STR("context window (tokens; empty is unknown)"), true,
                      window, sizeof window)
        || !tui_pick(STR("reasoning control for this model"), modes, 4,
                     TUI_PICK_FIRST, mode, &mode))
        return false;
    if (mode == 1) {
        snprintf(efforts, sizeof efforts, "%.*s", (i32)old.reasoning_efforts.n,
                 old.reasoning_efforts.p);
        snprintf(effort, sizeof effort, "%.*s", (i32)old.reasoning_effort.n,
                 old.reasoning_effort.p);
        if (!tui_ask_edit(STR("reasoning efforts (comma separated)"), false,
                          efforts, sizeof efforts)
            || !tui_ask_edit(STR("active effort (empty is Off)"), true, effort,
                             sizeof effort))
            return false;
    } else if (mode == 2) {
        snprintf(budgets, sizeof budgets, "%.*s", (i32)old.thinking_budgets.n,
                 old.thinking_budgets.p);
        snprintf(budget, sizeof budget, "%.*s", (i32)old.thinking_budget.n,
                 old.thinking_budget.p);
        if (!tui_ask_edit(STR("thinking budgets (comma separated)"), false,
                          budgets, sizeof budgets)
            || !tui_ask_edit(STR("active budget (empty is Off)"), true, budget,
                             sizeof budget))
            return false;
    } else if (mode == 3) {
        snprintf(templ, sizeof templ, "%.*s", (i32)old.reasoning_template.n,
                 old.reasoning_template.p);
        if (!tui_ask_edit(STR("request JSON object"), false, templ,
                          sizeof templ))
            return false;
    }
    ModelProfile p = {.reasoning_efforts = str_c(efforts),
                      .thinking_budgets = str_c(budgets),
                      .reasoning_effort = str_c(effort),
                      .thinking_budget = str_c(budget),
                      .reasoning_template = str_c(templ),
                      .configured = true};
    if (window[0]) {
        b8 ok = false;
        i64 n = str_int(str_c(window), &ok);
        if (!ok || n <= 0 || (u64)n > (u64)AGENT_MAX_CONTEXT_WINDOW) {
            tui_notice(STR("context window must be a positive token count"));
            return false;
        }
        p.context_window = (size_t)n;
    }
    if (!model_profile_save(provider, model, &p, scratch)
        || (live && !config_set_model_profile(cfg, &p))) {
        tui_notice(STR("invalid model settings or could not write config"));
        return false;
    }
    if (live) {
        ctx_set_window(&g_ctx, cfg->context_window);
        tui_set_reasoning(cfg->reasoning_effort, cfg->thinking_budget);
    }
    tui_notice(STR("model settings saved"));
    return true;
}

/* A row's name. The provider is part of it, rather than of the column beside
 * it, because the picker searches names: two providers serving one id would
 * otherwise be two rows with one name, and neither could be narrowed to by
 * the provider that serves it. Empty when it cannot be built, which the
 * caller answers with the bare id rather than by losing the row. */
static Str model_label(ModelPick *mp, size_t i) {
    if (!mp->label[i].p) {
        Str name = mp->cat->model[i], owner = mp->cat->provider[i];
        if (!mp->named || !owner.n) return name;
        Buf b;
        buf_init(&b, mp->arena, name.n + owner.n + 4);
        buf_puts(&b, name);
        buf_puts(&b, STR(" @ "));
        buf_puts(&b, owner);
        if (!buf_ok(&b)) return name;
        mp->label[i] = buf_finish(&b);
    }
    return mp->label[i];
}


static Str model_starred(ModelPick *mp, size_t i) {
    if (!mp->starred[i].p) {
        Str name = model_label(mp, i);
        Buf b;
        buf_init(&b, mp->arena, name.n + 3);
        buf_puts(&b, STR("* "));
        buf_puts(&b, name);
        if (!buf_ok(&b)) return name;
        mp->starred[i] = buf_finish(&b);
    }
    return mp->starred[i];
}


static b8 model_is_current(const ModelPick *mp, size_t i) {
    return str_eq(mp->cat->model[i], mp->current)
           && str_eq(mp->cat->provider[i], mp->provider);
}

static b8 model_is_small(const ModelPick *mp, size_t i) {
    if (!mp->small.n || !str_eq(mp->cat->model[i], mp->small)) return false;
    Str owner = mp->small_owner.n ? mp->small_owner : mp->provider;
    return str_eq(mp->cat->provider[i], owner);
}

static void model_row(ModelPick *mp, size_t row, size_t i, b8 fav) {
    mp->order[row] = i;
    Str name = fav ? model_starred(mp, i) : model_label(mp, i);
    b8 current = model_is_current(mp, i);
    b8 small = model_is_small(mp, i);
    Str desc = current && small ? STR("current \xc2\xb7 small")
               : current        ? STR("current")
               : small          ? STR("small")
                                : (Str){0};
    mp->rows[row] = (TuiCmd){name, desc};
}


static size_t model_entry(const ModelPick *mp, Str provider, Str model) {
    for (size_t i = 0; i < mp->cat->n; i++)
        if (mp->cat->model[i].n && str_eq(mp->cat->model[i], model)
            && str_eq(mp->cat->provider[i], provider))
            return i;
    return SIZE_MAX;
}


static size_t model_build(void *ud) {
    ModelPick *mp = ud;
    size_t row = 0;
    for (size_t f = 0; f < mp->fav.n; f++) {
        size_t i = model_entry(mp, mp->fav.provider[f], mp->fav.model[f]);
        if (i != SIZE_MAX) model_row(mp, row++, i, true);
    }
    for (size_t i = 0; i < mp->cat->n; i++)
        if (mp->cat->model[i].n
            && !favorites_has(&mp->fav, mp->cat->provider[i],
                              mp->cat->model[i]))
            model_row(mp, row++, i, false);
    return row;
}

static size_t model_favorite(void *ud, size_t row, size_t *moved) {
    ModelPick *mp = ud;
    if (row == SIZE_MAX) {
        *moved = 0;
        return model_build(mp);
    }
    size_t i = mp->order[row];
    b8 on = false;
    char err[128] = {0};

    if (!favorites_toggle(&mp->fav, mp->cat->provider[i], mp->cat->model[i],
                          mp->arena, &on, err, sizeof err))
        snprintf(mp->msg, sizeof mp->msg, "%s",
                 err[0] ? err : "could not save the favorites");

    if (!on && i >= mp->live) mp->cat->model[i] = (Str){0};
    size_t rows = model_build(mp);
    *moved = row < rows ? row : rows ? rows - 1 : 0;
    for (size_t r = 0; r < rows; r++)
        if (mp->order[r] == i) {
            *moved = r;
            break;
        }
    return rows;
}

static size_t model_manual_action(void *ud, size_t row, size_t *moved) {
    ModelPick *mp = ud;
    (void)row;
    *moved = 0;
    mp->requested = MODEL_ACTION_MANUAL;
    return 0;
}


static size_t model_configure_action(void *ud, size_t row, size_t *moved) {
    ModelPick *mp = ud;
    *moved = 0;
    mp->acted = row == SIZE_MAX ? SIZE_MAX : mp->order[row];
    mp->requested = MODEL_ACTION_CONFIGURE;
    return 0;
}


static size_t model_small_action(void *ud, size_t row, size_t *moved) {
    ModelPick *mp = ud;
    if (row == SIZE_MAX) {
        *moved = 0;
        return model_build(mp);
    }
    size_t i = mp->order[row];
    b8 on = model_is_small(mp, i);
    Str next = on ? (Str){0} : mp->cat->model[i];
    Str owner = on ? (Str){0} : mp->cat->provider[i];
    b8 saved = conf_remember_pair(CONF_SMALL_MODEL, next, CONF_SMALL_PROVIDER,
                                  owner, mp->arena);
    if (!config_set_small_model(mp->cfg, next, owner))
        snprintf(mp->msg, sizeof mp->msg,
                 "out of memory storing the small model");
    else if (!saved)
        snprintf(mp->msg, sizeof mp->msg, "could not save the small model");
    mp->small = mp->cfg->small_model;
    mp->small_owner = mp->cfg->small_provider;
    *moved = row;
    return model_build(mp);
}


static b8 manual_provider(const Config *cfg, const Endpoints *eps,
                          Arena *scratch, Str *out) {
    Str names[AGENT_MAX_ENDPOINTS + 1];
    size_t n = catalog_endpoints(cfg, eps, names, AGENT_MAX_ENDPOINTS + 1);
    if (!n) return false;
    if (n == 1) {
        *out = names[0];
        return true;
    }
    TuiCmd *items = arena_new(scratch, TuiCmd, n);
    if (!items) return false;
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
        items[i] = (TuiCmd){
            names[i].n ? names[i] : STR("(the configured base URL)"), (Str){0}};
        if (str_eq(names[i], cfg->provider)) at = i;
    }
    size_t pick = 0;
    if (!tui_pick(STR("which provider serves it"), items, n, TUI_PICK_FIRST, at,
                  &pick))
        return false;
    *out = names[pick];
    return true;
}

/* An id typed in rather than picked from a listing, for a provider the caller
 * has already settled. The id points into `scratch`, so a caller that keeps it
 * copies it out before resetting. False when nothing was entered. */
static b8 manual_model_id(Arena *scratch, const char *why, Str *model) {
    char question[256];
    if (why && *why)
        snprintf(question, sizeof question, "%s; enter a model manually", why);
    else
        snprintf(question, sizeof question, "model id (not verified)");
    char id[AGENT_MAX_MODEL_NAME + 1];
    if (!tui_ask(str_c(question), false, id, sizeof id)) return false;
    Str saved = str_dup(scratch, str_c(id));
    if (!saved.p) {
        tui_notice(STR("out of memory storing the model"));
        return false;
    }
    *model = saved;
    return true;
}


static b8 manual_model(const Config *cfg, const Endpoints *eps, Arena *scratch,
                       const char *why, Str *provider, Str *model) {
    return manual_provider(cfg, eps, scratch, provider)
           && manual_model_id(scratch, why, model);
}

/* What the list is missing, in one line: the providers that did not answer,
 * with the reason when there is one of them to give it for, and the cap if it
 * was reached. Their pinned models are still offered, so this is a missing
 * part of the list rather than a failed command. */
static void catalog_report(const Catalog *cat, Arena *scratch) {
    if (!cat->n_failed && !cat->full) return;
    size_t mark = scratch->off;
    Buf b;
    buf_init(&b, scratch,
             AGENT_MAX_ENDPOINTS * (AGENT_MAX_ENDPOINT_NAME + 2) + 256);
    if (cat->n_failed == 1) {
        buf_puts(&b, STR("could not list "));
        if (cat->failed[0].n) {
            buf_puts(&b, cat->failed[0]);
            buf_puts(&b, STR(": "));
        } else {
            buf_puts(&b, STR("models: "));
        }
        buf_puts(&b,
                 cat->reason[0].n ? cat->reason[0] : STR("no models returned"));
    } else if (cat->n_failed) {
        char head[64];
        snprintf(head, sizeof head,
                 "could not list %zu providers: ", cat->n_failed);
        buf_puts(&b, str_c(head));
        for (size_t i = 0; i < cat->n_failed; i++) {
            if (i) buf_puts(&b, STR(", "));
            buf_puts(&b, cat->failed[i].n ? cat->failed[i]
                                          : STR("the configured base URL"));
        }
    }
    if (cat->full) {
        char cap[64];
        snprintf(cap, sizeof cap, "only the first %d models are listed",
                 AGENT_MAX_MODELS);
        if (cat->n_failed) buf_puts(&b, STR("; "));
        buf_puts(&b, str_c(cap));
    }
    if (buf_ok(&b)) tui_notice(buf_finish(&b));
    scratch->off = mark;
}

/* Whether a provider's listing arrived. A pin of one that did not is still
 * offered; a pin of one that answered is not, since the listing is then what
 * that provider serves. */
static b8 catalog_missed(const Catalog *cat, Str provider) {
    for (size_t i = 0; i < cat->n_failed; i++)
        if (str_eq(cat->failed[i], provider)) return true;
    return false;
}

/* The picker over `cat`. `*provider` and `*model` name the pick and point
 * into `scratch`; `*verified` is false for an id entered by hand. `cat` is
 * appended to and its entries are edited, so it belongs to this call. */
static b8 pick_model(Config *cfg, const Endpoints *eps, Catalog *cat,
                     Arena *scratch, Str *provider, Str *model, b8 *verified) {
    *verified = false;
    ModelPick mp = {0};
    mp.cat = cat;
    mp.live = cat->n;
    mp.arena = scratch;
    favorites_load(&mp.fav, eps, scratch);

    for (size_t f = 0; f < mp.fav.n; f++) {
        if (model_entry(&mp, mp.fav.provider[f], mp.fav.model[f]) != SIZE_MAX)
            continue;
        if (!catalog_missed(cat, mp.fav.provider[f])) continue;
        catalog_add(cat, mp.fav.provider[f], mp.fav.model[f]);
    }
    if (!cat->n) {
        const char *why = cat->n_failed && cat->reason[0].n
                              ? cat->reason[0].p
                              : "no models returned";
        return manual_model(cfg, eps, scratch, why, provider, model);
    }


    for (size_t i = 1; i < cat->n && !mp.named; i++)
        mp.named = !str_eq(cat->provider[i], cat->provider[0]);
    mp.label = arena_new(scratch, Str, cat->n);
    mp.starred = arena_new(scratch, Str, cat->n);
    mp.order = arena_new(scratch, size_t, cat->n);
    mp.rows = arena_new(scratch, TuiCmd, cat->n);
    if (!mp.label || !mp.starred || !mp.order || !mp.rows) {
        tui_notice(STR("out of memory listing models"));
        return false;
    }
    memset(mp.label, 0, cat->n * sizeof *mp.label);
    memset(mp.starred, 0, cat->n * sizeof *mp.starred);
    mp.provider = cfg->provider;
    mp.current = cfg->model;
    mp.small = cfg->small_model;
    mp.small_owner = cfg->small_provider;
    mp.cfg = cfg;
    mp.acted = SIZE_MAX;

    size_t rows = model_build(&mp);
    TuiPickBinding bindings[] = {
        {model_favorite, &mp, 0x06},
        {model_manual_action, &mp, 0x0f},
        {model_configure_action, &mp, 0x05},

        {model_small_action, &mp, 0x13},
    };

    TuiPickAction act = {
        mp.rows, cat->n, bindings, 4,
        STR("Ctrl-F pins \xc2\xb7 Ctrl-O manual entry \xc2\xb7 "
            "Ctrl-E configures \xc2\xb7 Ctrl-S small model")};
    size_t pick = 0;
    /* The cursor opens on the model the session is on when it is a row, so
     * the first keystroke is a move rather than a change. */
    size_t start = 0;
    for (size_t r = 0; r < rows; r++)
        if (model_is_current(&mp, mp.order[r])) {
            start = r;
            break;
        }
    b8 chosen = tui_pick_action(STR("pick a model"), rows, cat->n,
                                TUI_PICK_FIRST, start, &act, &pick);
    if (mp.msg[0]) tui_notice(str_c(mp.msg));
    if (mp.requested == MODEL_ACTION_MANUAL)
        return manual_model(cfg, eps, scratch, NULL, provider, model);
    if (mp.requested == MODEL_ACTION_CONFIGURE) {
        if (mp.acted < cat->n)
            (void)edit_model_profile(cfg, cat->provider[mp.acted],
                                     cat->model[mp.acted], scratch);
        return false;
    }
    if (!chosen) return false;
    size_t i = pick < rows ? mp.order[pick] : cat->n;
    if (i >= cat->n || !cat->model[i].n) return false;
    *provider = cat->provider[i];
    *model = cat->model[i];
    *verified = true;
    return true;
}

static b8 apply_model_profile(Config *cfg, Arena *scratch) {
    ModelProfile p;
    model_profile_load(&p, cfg->provider, cfg->model, scratch, scratch);
    return config_set_model_profile(cfg, &p);
}


static b8 no_provider(const Config *cfg) {
    return !cfg->api_key.p && !cfg->base_url_set;
}


static Str setup_hint(const Config *cfg, Arena *scratch) {
    if (!no_provider(cfg)) return (Str){0};
    size_t mark = scratch->off;
    Endpoints eps;
    size_t n = endpoints_load(&eps, scratch);
    scratch->off = mark;
    return n ? NO_MODEL_HINT : NO_PROVIDER_HINT;
}


static b8 use_model(Config *cfg, const Endpoints *eps, Str provider, Str model,
                    Arena *scratch, b8 remember, b8 *saved) {
    size_t mark = scratch->off;
    if (provider.n) {
        size_t i = endpoints_find(eps, provider);
        if (i == ENDPOINT_NONE) {
            notice_fmt("provider %.*s is no longer configured", (i32)provider.n,
                       provider.p);
            return false;
        }
        char err[AGENT_MAX_PATH + 96] = {0};
        Str key = endpoints_key(provider, scratch, scratch, err, sizeof err);
        if (err[0]) {
            tui_notice(str_c(err));
            scratch->off = mark;
            return false;
        }
        /* The key is copied into the Config's own storage, so it outlives the
         * arena it was read into. */
        b8 ok = config_set_endpoint(cfg, provider, eps->base_url[i], model,
                                    eps->api[i], key);
        scratch->off = mark;
        if (!ok) {
            tui_notice(STR("out of memory storing the model"));
            return false;
        }
    } else if (!config_set_model(cfg, model)) {
        tui_notice(STR("out of memory storing the model"));
        return false;
    }
    if (!apply_model_profile(cfg, scratch)) {
        tui_notice(STR("out of memory storing model settings"));
        return false;
    }

    if (!cfg->small_provider.n)
        config_set_small_model(
            cfg, endpoints_small_model(cfg->provider, scratch), (Str){0});
    tui_set_provider(cfg->provider);
    tui_set_model(cfg->model);
    ctx_model_changed(&g_ctx);
    /* Begun rather than blamed: the new endpoint may not report cache reads
     * at all, and a guard still armed from the old one would read every
     * request it answers as a miss. */
    cache_guard_begin(&g_cache);
    ctx_set_window(&g_ctx, cfg->context_window);
    tui_set_reasoning(cfg->reasoning_effort, cfg->thinking_budget);
    tui_set_setup_hint((Str){0});
    tui_set_setup(false);
    b8 wrote =
        !remember || config_remember_model(cfg->provider, cfg->model, scratch);
    if (saved) *saved = wrote;
    TelEvent e;
    tel_open(&e, "model");
    tel_str(&e, "name", cfg->model);
    tel_str(&e, "provider", cfg->provider);
    tel_str(&e, "api", api_name(cfg->api));
    tel_bool(&e, "has_key", cfg->api_key.p != NULL);
    tel_send(&e);
    return true;
}


static void model_chosen_notice(Str provider, Str model, b8 verified,
                                b8 saved) {
    notice_fmt("model: %.*s%s%.*s%s%s", (i32)model.n, model.p,
               provider.n ? " @ " : "", (i32)(provider.n ? provider.n : 0),
               provider.n ? provider.p : "",
               verified ? "" : " (entered manually; not verified)",
               saved ? "" : " (not remembered: could not write state)");
}

static void model_status(Str provider, void *ud) {
    (void)ud;
    char status[AGENT_MAX_ENDPOINT_NAME + 32];
    if (provider.n)
        snprintf(status, sizeof status, "listing models: %.*s", (i32)provider.n,
                 provider.p);
    else
        snprintf(status, sizeof status, "loading models");
    tui_set_status(status);
}


static void choose_model(Config *cfg, Arena *scratch) {
    arena_reset(scratch);
    Endpoints eps;
    endpoints_load(&eps, scratch);
    Str names[AGENT_MAX_ENDPOINTS + 1];

    if (!catalog_endpoints(cfg, &eps, names, AGENT_MAX_ENDPOINTS + 1)) {
        tui_notice(NO_PROVIDER_HINT);
        return;
    }
    Catalog cat;
    catalog_load(&cat, cfg, &eps, AGENT_MAX_MODELS, scratch, model_status,
                 NULL);
    tui_set_status("ready");
    catalog_report(&cat, scratch);

    Str provider = {0}, model = {0};
    b8 verified = false, saved = false;
    if (!pick_model(cfg, &eps, &cat, scratch, &provider, &model, &verified))
        return;
    if (!use_model(cfg, &eps, provider, model, scratch, true, &saved)) return;
    model_chosen_notice(provider, model, verified, saved);
}


/* Where the key is kept. The file is offered first because it always works:
 * a keyring needs its daemon and its helper installed, and a run with no
 * session bus has neither. Only the stores arqan can write to are listed;
 * `command` is deliberately absent, since a provider that runs a program of
 * the user's choosing is set up by editing the credentials file. */
static b8 pick_key_source(SecretSource *out) {
    const TuiCmd stores[] = {
        {STR("Credentials file"),
         STR("$XDG_STATE_HOME/" AGENT_NAME "/credentials.toml, mode 0600")},
        {STR("System keyring"), STR("Secret Service, through secret-tool")},
        {STR("Password store"), STR("pass, under " AGENT_NAME "/<provider>")},
    };
    size_t pick = *out == SECRET_SERVICE ? 1 : *out == SECRET_PASS ? 2 : 0;
    if (!tui_pick(STR("where should the key be kept"), stores, 3,
                  TUI_PICK_FIRST, pick, &pick))
        return false;
    *out = pick == 1 ? SECRET_SERVICE : pick == 2 ? SECRET_PASS : SECRET_STORED;
    return true;
}

/* Which wire format an endpoint speaks. The default is offered first, since
 * it is what most of them are. */
static b8 pick_api(ApiKind *out) {
    const TuiCmd apis[] = {
        {STR("openai"), STR("OpenAI-compatible chat completions")},
        {STR("anthropic"), STR("Anthropic-compatible messages API")},
    };
    size_t pick = *out == API_ANTHROPIC ? 1 : 0;
    if (!tui_pick(STR("which API does it speak"), apis, 2, TUI_PICK_FIRST, pick,
                  &pick))
        return false;
    *out = pick == 1 ? API_ANTHROPIC : API_OPENAI;
    return true;
}

/* The connection, asked of the endpoint itself: listing its models is the
 * check that the URL and the key work, so a typo is answered here rather than
 * on the first turn. `out` receives the ids, which live in `scratch`. Zero
 * with `err` set when the listing did not arrive. */
static size_t probe_endpoint(const Config *cfg, Str base_url, ApiKind api,
                             Str key, Arena *scratch, Str *out, size_t max,
                             char *err, size_t err_cap) {
    Config probe = *cfg;
    probe.base_url = base_url;
    probe.api = api;
    probe.api_key = key;
    probe.provider = (Str){0};
    probe.model = (Str){0};
    tui_set_status("checking the provider");
    size_t n = provider_models(&probe, scratch, out, max, err, err_cap);
    tui_set_status("ready");
    return n;
}

/* A listing that did not arrive is said and asked about rather than refused:
 * an endpoint may be down, or may not serve /models at all, and the user is
 * the one who knows which. */
static b8 keep_unlisted_endpoint(Str name, const char *why, Arena *scratch) {
    const TuiCmd actions[] = {
        {STR("Cancel"), STR("Change nothing; the provider is not stored")},
        {STR("Store anyway"), STR("Keep it; /model says if it stays silent")},
    };
    Buf title;
    buf_init(&title, scratch, name.n + strlen(why) + 32);
    buf_puts(&title, name);
    buf_puts(&title, STR(" could not be listed: "));
    buf_puts(&title, str_c(why));
    size_t action = 0;
    return tui_pick(buf_ok(&title) ? buf_finish(&title)
                                   : STR("the provider could not be listed"),
                    actions, 2, TUI_PICK_FIRST, 0, &action)
           && action == 1;
}


static void notice_models(Str name, size_t n) {
    notice_fmt("provider: %.*s (%zu model%s)", (i32)name.n, name.p, n,
               n == 1 ? "" : "s");
}

/* Ask for a provider and store it: a name, the API it speaks, its base URL
 * and a key, checked by listing its models. Nothing is written until the check
 * has been answered one way or the other. A run that has no model yet is
 * offered the ones this provider just listed, which is the whole of first-run
 * setup; a session already talking to a model is left alone, since adding a
 * connection is not choosing one. */
static b8 add_endpoint(Config *cfg, Arena *scratch) {
    arena_reset(scratch);
    char name[AGENT_MAX_ENDPOINT_NAME + 1];
    char url[AGENT_MAX_URL + 1];
    char key[AGENT_MAX_API_KEY + 1];
    if (!tui_ask(STR("a name for this provider"), false, name, sizeof name))
        return false;
    if (!endpoint_name_ok(str_c(name))) {
        tui_notice(STR("a provider name takes letters, digits, - and _"));
        return false;
    }

    Endpoints eps;
    endpoints_load(&eps, scratch);
    if (endpoints_find(&eps, str_c(name)) != ENDPOINT_NONE) {
        notice_fmt("a provider named %s already exists", name);
        return false;
    }
    if (eps.n >= AGENT_MAX_ENDPOINTS) {
        notice_fmt("no room for another provider (%d)", AGENT_MAX_ENDPOINTS);
        return false;
    }
    ApiKind api = API_OPENAI;
    if (!pick_api(&api)) return false;
    if (!tui_ask(STR("its base URL, ending in /v1"), false, url, sizeof url))
        return false;
    if (!str_starts(str_c(url), STR("http://"))
        && !str_starts(str_c(url), STR("https://"))) {
        tui_notice(STR("a base URL starts with http:// or https://"));
        return false;
    }
    if (!tui_ask(STR("its API key (empty if it needs none)"), true, key,
                 sizeof key))
        key[0] = '\0';
    SecretSource key_source = SECRET_STORED;
    if (key[0] && !pick_key_source(&key_source)) return false;

    Str *listed = arena_new(scratch, Str, AGENT_MAX_MODELS);
    if (!listed) {
        tui_notice(STR("out of memory listing models"));
        return false;
    }
    char list_err[192] = {0};
    size_t n_listed = probe_endpoint(
        cfg, str_c(url), api, key[0] ? str_c(key) : (Str){0}, scratch, listed,
        AGENT_MAX_MODELS, list_err, sizeof list_err);
    if (!n_listed
        && !keep_unlisted_endpoint(
            str_c(name), list_err[0] ? list_err : "no models returned",
            scratch))
        return false;

    char err[AGENT_MAX_PATH + 64] = {0};
    if (!endpoints_put(&eps, str_c(name), str_c(url), api, scratch)
        || !endpoints_save_one(str_c(name), str_c(url), api, scratch)) {
        tui_notice(STR("could not write the provider store"));
        return false;
    }

    if (!endpoints_set_key(str_c(name), str_c(key), key_source, scratch, err,
                           sizeof err)) {
        tui_notice(str_c(err[0] ? err : "could not store the API key"));
        return false;
    }
    size_t at = endpoints_find(&eps, str_c(name));
    if (at == ENDPOINT_NONE) return false;
    Str stored = eps.name[at];
    if (!no_provider(cfg)) {
        notice_models(stored, n_listed);
        return true;
    }
    /* First-run setup ends in a model: the listing that verified the provider
     * is the list it is picked from, so no second request is made, and one
     * that would not list at all is asked for an id instead. */
    Str provider = stored, model = {0};
    b8 verified = false, saved = false;
    if (n_listed) {
        Catalog cat;
        if (catalog_init(&cat, n_listed, scratch)) {
            for (size_t i = 0; i < n_listed; i++)
                catalog_add(&cat, stored, listed[i]);
            if (!pick_model(cfg, &eps, &cat, scratch, &provider, &model,
                            &verified))
                model = (Str){0};
        } else {
            tui_notice(STR("out of memory listing models"));
        }
    } else if (!manual_model_id(scratch,
                                list_err[0] ? list_err : "no models returned",
                                &model)) {
        model = (Str){0};
    }

    if (!model.n
        || !use_model(cfg, &eps, provider, model, scratch, true, &saved)) {
        notice_fmt("provider: %.*s (no model chosen yet: type /model)",
                   (i32)stored.n, stored.p);
        tui_set_setup_hint(setup_hint(cfg, scratch));
        return true;
    }
    model_chosen_notice(provider, model, verified, saved);
    return true;
}


static b8 edit_endpoint(Config *cfg, Endpoints *eps, size_t i, Arena *persist,
                        Arena *scratch) {
    char url[AGENT_MAX_URL + 1];
    char key[AGENT_MAX_API_KEY + 1];
    snprintf(url, sizeof url, "%.*s", (i32)eps->base_url[i].n,
             eps->base_url[i].p);
    if (!tui_ask_edit(STR("base URL"), false, url, sizeof url)) return false;
    if (!str_starts(str_c(url), STR("http://"))
        && !str_starts(str_c(url), STR("https://"))) {
        tui_notice(STR("a base URL starts with http:// or https://"));
        return false;
    }
    ApiKind api = eps->api[i];
    if (!pick_api(&api)) return false;

    SecretSource key_source = endpoints_key_source(eps->name[i], scratch);
    Str store = secret_source_name(key_source);
    Buf keep_desc;
    buf_init(&keep_desc, scratch, store.n + 40);
    buf_puts(&keep_desc, STR("Leave it where it is, in: "));
    buf_puts(&keep_desc, store);
    const TuiCmd key_actions[] = {
        {STR("Keep current"),
         buf_ok(&keep_desc) ? buf_finish(&keep_desc)
                            : STR("Leave the stored credential unchanged")},
        {STR("Replace"), STR("Enter and store a different credential")},
        {STR("Move"), STR("Keep the same key, change which store holds it")},
        {STR("Clear"), STR("Remove the stored credential")},
    };
    enum { KEY_KEEP, KEY_REPLACE, KEY_MOVE, KEY_CLEAR };
    size_t key_action = KEY_KEEP;
    if (!tui_pick(STR("API key"), key_actions, 4, TUI_PICK_FIRST, KEY_KEEP,
                  &key_action))
        return false;
    if (key_action == KEY_REPLACE
        && (!tui_ask(STR("replacement API key"), true, key, sizeof key)
            || !pick_key_source(&key_source)))
        return false;
    if (key_action == KEY_MOVE && !pick_key_source(&key_source)) return false;
    char err[AGENT_MAX_PATH + 64] = {0};
    Str saved_key = {0};
    /* Moving reads the key arqan already holds rather than asking for it
     * again: requiring the value is what keeps keys in the file. */
    if (key_action == KEY_KEEP || key_action == KEY_MOVE) {
        saved_key =
            endpoints_key(eps->name[i], persist, scratch, err, sizeof err);
        if (err[0]) {
            tui_notice(str_c(err));
            return false;
        }
        if (key_action == KEY_MOVE && !saved_key.n) {
            tui_notice(STR("this provider has no stored key to move"));
            return false;
        }
    } else if (key_action == KEY_REPLACE) {
        saved_key = str_dup(persist, str_c(key));
        if (!saved_key.p) {
            tui_notice(STR("out of memory storing the provider"));
            return false;
        }
    }

    b8 changed = !str_eq(str_c(url), eps->base_url[i]) || api != eps->api[i]
                 || key_action != KEY_KEEP;
    size_t n_listed = 0;
    char list_err[192] = {0};
    if (changed) {
        Str *listed = arena_new(scratch, Str, AGENT_MAX_MODELS);
        if (!listed) {
            tui_notice(STR("out of memory listing models"));
            return false;
        }
        n_listed =
            probe_endpoint(cfg, str_c(url), api, saved_key, scratch, listed,
                           AGENT_MAX_MODELS, list_err, sizeof list_err);
        if (!n_listed
            && !keep_unlisted_endpoint(
                eps->name[i], list_err[0] ? list_err : "no models returned",
                scratch))
            return false;
    }
    if (!endpoints_put(eps, eps->name[i], str_c(url), api, scratch)
        || !endpoints_save_one(eps->name[i], str_c(url), api, scratch)) {
        tui_notice(STR("invalid provider settings"));
        return false;
    }
    if (key_action != KEY_KEEP
        && !endpoints_set_key(eps->name[i],
                              key_action == KEY_CLEAR  ? (Str){0}
                              : key_action == KEY_MOVE ? saved_key
                                                       : str_c(key),
                              key_source, scratch, err, sizeof err)) {
        tui_notice(str_c(err[0] ? err : "could not store the API key"));
        return false;
    }

    if (str_eq(eps->name[i], cfg->provider) && cfg->model.n
        && !use_model(cfg, eps, eps->name[i], cfg->model, scratch, false, NULL))
        return false;
    if (changed && n_listed)
        notice_models(eps->name[i], n_listed);
    else
        notice_fmt("provider: %.*s updated", (i32)eps->name[i].n,
                   eps->name[i].p);
    return true;
}

static b8 delete_endpoint(Config *cfg, const Endpoints *eps, size_t i,
                          Arena *scratch) {
    const TuiCmd actions[] = {
        {STR("Keep provider"), STR("Cancel without changing either store")},
        {STR("Delete provider"), STR("Remove its settings and credential")},
    };
    Buf title;
    buf_init(&title, scratch, eps->name[i].n + 20);
    buf_puts(&title, STR("delete provider "));
    buf_puts(&title, eps->name[i]);
    buf_putc(&title, '?');
    if (!buf_ok(&title)) return false;
    size_t action = 0;
    if (!tui_pick(buf_finish(&title), actions, 2, TUI_PICK_FIRST, 0, &action)
        || action == 0)
        return false;

    Str name = eps->name[i];
    b8 serving = str_eq(name, cfg->provider);
    char err[AGENT_MAX_PATH + 64] = {0};
    if (!endpoints_delete(name, scratch, err, sizeof err)) {
        tui_notice(str_c(err[0] ? err : "could not delete the provider"));
        return false;
    }

    favorites_forget(name, scratch);
    if (serving) {
        /* The model went with the connection, so the choice is forgotten
         * rather than left naming an endpoint that is not there. */
        config_remember_model((Str){0}, (Str){0}, scratch);
        cfg->provider = (Str){0};
        cfg->api_key = (Str){0};
        cfg->base_url_set = false;
        cfg->reasoning_efforts = (Str){0};
        cfg->thinking_budgets = (Str){0};
        cfg->reasoning_effort = (Str){0};
        cfg->thinking_budget = (Str){0};
        cfg->reasoning_template = (Str){0};
        tui_set_reasoning((Str){0}, (Str){0});
        tui_set_setup(true);
        tui_set_setup_hint(setup_hint(cfg, scratch));
    }
    notice_fmt("deleted provider: %.*s", (i32)name.n, name.p);
    return true;
}

/* The providers already stored, with the one serving the chosen model marked.
 * Enter edits a provider, since editing is the only thing left to do to one.
 * With none stored there is nothing to list, so the form opens straight
 * away. */
static void manage_providers(Config *cfg, Arena *persist, Arena *scratch) {
    arena_reset(scratch);
    Endpoints eps;
    size_t n = endpoints_load(&eps, scratch);
    if (!n) {
        add_endpoint(cfg, scratch);
        return;
    }
    TuiCmd *items = arena_new(scratch, TuiCmd, n + 2);
    if (!items) {
        tui_notice(STR("out of memory listing providers"));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        Str desc = eps.base_url[i];

        b8 serving = str_eq(eps.name[i], cfg->provider);
        b8 anth = eps.api[i] == API_ANTHROPIC;
        if (serving || anth) {
            Buf b;
            buf_init(&b, scratch, desc.n + 32);
            if (serving) buf_puts(&b, STR("in use \xc2\xb7 "));
            if (anth) buf_puts(&b, STR("anthropic \xc2\xb7 "));
            buf_puts(&b, eps.base_url[i]);
            if (buf_ok(&b)) desc = buf_finish(&b);
        }
        items[i] = (TuiCmd){eps.name[i], desc};
    }
    items[n] = (TuiCmd){STR("+ add a provider"),
                        STR("An OpenAI- or Anthropic-compatible endpoint")};
    items[n + 1] = (TuiCmd){STR("+ delete a provider"),
                            STR("Remove its settings and stored credential")};

    size_t pick = 0;
    if (!tui_pick(STR("providers"), items, n + 2, TUI_PICK_FIRST, TUI_PICK_NONE,
                  &pick))
        return;
    if (pick == n) {
        add_endpoint(cfg, scratch);
        return;
    }
    if (pick == n + 1) {
        size_t del = 0;
        if (tui_pick(STR("delete a provider"), items, n, TUI_PICK_FIRST,
                     TUI_PICK_NONE, &del))
            delete_endpoint(cfg, &eps, del, scratch);
        return;
    }
    edit_endpoint(cfg, &eps, pick, persist, scratch);
}

/* ---- /settings -----------------------------------------------------------
 * The screen is its own answer, so nothing here writes a notice: a toggle
 * that refused to change is a box that stayed empty. Every UI-owned choice
 * is remembered in state; telemetry and reasoning keep their own paths.
 * Every row is changed where it is read,
 * which is why the tools are rows of this list rather than a screen behind
 * one, and why nothing here opens a question: a setting a reader has to walk
 * to is a setting they have to find twice. The model and the provider are
 * not settings of a session but the endpoint it talks to, and `/model` and
 * `/provider` are where they are chosen. A row with more than two answers
 * lists them all rather than describing the live one, since the reader is
 * choosing between them, not reading about one.
 */
/* What a row changes, since the rows a session offers vary: the tools it
 * runs and the active model's reasoning controls are both conditional.
 * Checkboxes come first, then the tools, then the rows that step between
 * options: a list a reader scans is a list whose answers look alike. */
enum {
    SET_VERBOSE,
    SET_RAW,
    SET_STREAM,
    SET_IGNORED,
    SET_TELEMETRY,
    SET_SHOW_INSTRUCTIONS,
    SET_AUTO_TITLE,
    SET_TOOL,
    SET_WRAP,
    SET_MODE,
    SET_MAX_TOKENS,
    SET_EFFORT,
    SET_BUDGET,
    SET_PERMISSIONS,
    SET_RESUME_LAST,
    SET_COMPACT,
    SET_COMPACT_AT,
    SET_COMPACT_MODEL
};
#define SET_MAX_ROWS (21 + AGENT_MAX_TOOLS)

/* "[x] label" for a toggle and the same column for a value row, so the two
 * kinds read as one list. A row that lost its checkbox to a full arena is
 * still the row it was. */
static Str setting_label(Arena *a, Str label, const char *box) {
    Buf b;
    buf_init(&b, a, label.n + 8);
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

/* Every option the row steps between, on one line, with the current one
 * marked for the renderer. A row that says what it will reach is a row a
 * reader steps through once rather than twice. */
static Str setting_options(Arena *a, const Str *opts, size_t n, size_t cur,
                           TuiMark *mark) {
    *mark = (TuiMark){0, 0};
    Buf b;
    buf_init(&b, a, 64);
    for (size_t i = 0; i < n; i++) {
        if (i) buf_puts(&b, STR("  "));
        if (i == cur) *mark = (TuiMark){b.n, opts[i].n};
        buf_puts(&b, opts[i]);
    }
    if (!buf_ok(&b)) {
        *mark = (TuiMark){0, 0};
        return (Str){0};
    }
    return buf_finish(&b);
}

static void remember_ui(Arena *scratch, ConfKey key, Str value) {
    if (!conf_remember(key, value, scratch))
        tui_notice(STR(
            "setting changed but was not remembered: could not write state"));
}

static void remember_ui_bool(Arena *scratch, ConfKey key, b8 on) {
    remember_ui(scratch, key, on ? STR("true") : STR("false"));
}

static void remember_tools(const ToolRegistry *reg, Arena *scratch) {
    Buf list;
    buf_init(&list, scratch, 128);
    for (size_t i = 0; i < reg->n; i++) {
        if (!tools_can_disable(reg, i) || !tools_disabled(reg, i)) continue;
        if (list.n) buf_putc(&list, ',');
        buf_puts(&list, reg->name[i]);
    }
    if (!buf_ok(&list)) {
        tui_notice(
            STR("setting changed but was not remembered: out of memory"));
        return;
    }
    Str value = list.n ? buf_finish(&list) : STR("none");
    remember_ui(scratch, CONF_DISABLE_TOOLS, value);
}

/* The rows the screen is showing, rebuilt in place after every change: the
 * strings live in the scratch arena, which each rebuild resets, so nothing
 * outside a build call may hold one. */
typedef struct {
    Arena *scratch;
    Arena rows_arena;
    TuiCmd rows[TUI_STATUS_N];
} StatusView;

static size_t statusline_build(void *ud) {
    const Str labels[TUI_STATUS_N] = {
        STR("State"),
        STR("Model"),
        STR("Reasoning effort"),
        STR("Thinking budget"),
        STR("Mode"),
        STR("Provider"),
        STR("Working directory"),
        STR("Context tokens"),
        STR("Copy confirmation"),
        STR("Permissions"),
    };
    const Str descriptions[TUI_STATUS_N] = {
        STR("Current ready, thinking, or error state"),
        STR("Model used for the next request"),
        STR("Active reasoning effort when configured"),
        STR("Active thinking budget when configured"),
        STR("Build or Plan mode"),
        STR("Active provider name or endpoint host"),
        STR("Current project directory"),
        STR("Context the next request carries; ~ marks an estimate"),
        STR("Brief acknowledgement after /copy"),
        STR("Ask or Free approval policy"),
    };
    StatusView *v = ud;
    arena_reset(&v->rows_arena);
    for (size_t i = 0; i < TUI_STATUS_N; i++) {
        TuiStatusItem item = (TuiStatusItem)i;
        v->rows[i] = (TuiCmd){
            setting_check(&v->rows_arena, tui_status_visible(item), labels[i]),
            descriptions[i],
        };
    }
    return TUI_STATUS_N;
}

static void statusline_act(void *ud, size_t row, i32 delta) {
    StatusView *v = ud;
    (void)delta;
    if (row >= TUI_STATUS_N) return;
    /* Mid-turn the scratch arena belongs to the request in flight, so what
     * remembering a choice writes through it is given back. */
    size_t mark = v->scratch->off;
    TuiStatusItem item = (TuiStatusItem)row;
    tui_set_status_visible(item, !tui_status_visible(item));
    u64 mask = 0;
    for (size_t i = 0; i < TUI_STATUS_N; i++)
        if (tui_status_visible((TuiStatusItem)i)) mask |= (u64)1 << i;
    char value[32];
    snprintf(value, sizeof value, "%llu", (unsigned long long)mask);
    remember_ui(v->scratch, CONF_STATUS_FIELDS, str_c(value));
    v->scratch->off = mark;
}

/* The screen's own state, static because a screen opened during a turn
 * outlives the frame that opened it. */
static const TuiSettings *statusline_screen(Arena *scratch) {
    static StatusView view;
    static TuiSettings set;
    view.scratch = scratch;
    arena_init(&view.rows_arena, g_screen, sizeof g_screen);
    set = (TuiSettings){
        .rows = view.rows,
        .marks = NULL,
        .max = TUI_STATUS_N,
        .build = statusline_build,
        .act = statusline_act,
        .ud = &view,
    };
    return &set;
}

static void choose_statusline(Arena *scratch) {
    tui_settings(STR("status line"), statusline_screen(scratch));
}

/* The values Max tokens steps between, since a number typed into a question
 * is a screen away from the row it belongs to. A setting that arrived from a
 * config file or a flag sits wherever it sits: a step moves to the nearest
 * rung past it in the direction asked for, and the ends hold. */
static const i32 g_token_steps[] = {1024,  2048,  4096,   8192,  16384,
                                    32768, 65536, 131072, 262144};

static i32 max_tokens_step(i32 cur, i32 dir) {
    size_t n = sizeof g_token_steps / sizeof g_token_steps[0];
    if (dir > 0) {
        for (size_t i = 0; i < n; i++)
            if (g_token_steps[i] > cur) return g_token_steps[i];
        return g_token_steps[n - 1];
    }
    for (size_t i = n; i-- > 0;)
        if (g_token_steps[i] < cur) return g_token_steps[i];
    return g_token_steps[0];
}

/* The compaction threshold walks in fives between the bounds the settings
 * table states, and the ends hold rather than wrap: a percentage is a dial,
 * not a list of answers. */
#define COMPACT_AT_MIN  50u
#define COMPACT_AT_MAX  95u
#define COMPACT_AT_STEP 5u

static u32 compact_at_step(u32 cur, i32 dir) {
    if (dir > 0)
        return cur + COMPACT_AT_STEP > COMPACT_AT_MAX ? COMPACT_AT_MAX
                                                      : cur + COMPACT_AT_STEP;
    return cur < COMPACT_AT_MIN + COMPACT_AT_STEP ? COMPACT_AT_MIN
                                                  : cur - COMPACT_AT_STEP;
}

/* A configured comma list as the options a row offers: "Off" first, since a
 * provider control the user has not set is a control that is not sent. */
#define SET_MAX_OPTIONS 64
static size_t list_options(Str list, Str *out, size_t max) {
    size_t n = 0, off = 0;
    out[n++] = STR("Off");
    while (off < list.n && n < max) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') end++;
        Str item = str_trim((Str){list.p + off, end - off});
        if (item.n) out[n++] = item;
        off = end + 1;
    }
    return n;
}

static size_t list_at(const Str *opts, size_t n, Str current) {
    for (size_t i = 1; i < n; i++)
        if (str_eq(opts[i], current)) return i;
    return 0;
}

static Str list_step(Str list, Str current, i32 dir) {
    Str opt[SET_MAX_OPTIONS];
    size_t n = list_options(list, opt, SET_MAX_OPTIONS);
    i32 pos = (i32)list_at(opt, n, current) + (dir > 0 ? 1 : -1);
    if (pos >= (i32)n) pos = 0;
    if (pos < 0) pos = (i32)n - 1;
    return pos ? opt[(size_t)pos] : (Str){0};
}

static b8 remember_reasoning(Config *cfg, Arena *scratch, b8 effort,
                             Str value) {
    if (!cfg->provider.n) return false;
    size_t mark = scratch->off;
    ModelProfile p;
    model_profile_load(&p, cfg->provider, cfg->model, scratch, scratch);
    if (!p.configured || !config_set_reasoning(cfg, effort, value)) {
        scratch->off = mark;
        return false;
    }
    tui_set_reasoning(cfg->reasoning_effort, cfg->thinking_budget);
    if (effort)
        p.reasoning_effort = value;
    else
        p.thinking_budget = value;
    b8 ok = model_profile_save(cfg->provider, cfg->model, &p, scratch);
    scratch->off = mark;
    return ok;
}

/* The rows the screen is showing, with what each one changes beside it. Like
 * the status line view, every string points into the scratch arena that the
 * next build resets. */
typedef struct {
    Agent *ag;
    Arena rows_arena;
    TuiCmd rows[SET_MAX_ROWS];
    TuiMark marks[SET_MAX_ROWS];
    u8 kind[SET_MAX_ROWS];
    size_t tool[SET_MAX_ROWS];
    size_t n;
} SettingsView;

static size_t settings_build(void *ud) {
    SettingsView *v = ud;
    Agent *ag = v->ag;
    Config *cfg = ag->cfg;
    ToolRegistry *reg = ag->tools;
    Arena *rows_arena = &v->rows_arena;
    arena_reset(rows_arena);
    TuiCmd *rows = v->rows;
    TuiMark *marks = v->marks;
    u8 *kind = v->kind;
    size_t n = 0;
    memset(marks, 0, sizeof v->marks);

    kind[n] = SET_VERBOSE;
    rows[n++] = (TuiCmd){
        setting_check(rows_arena, render_verbose(), STR("Verbose tool output")),
        STR("Every line a tool printed, untruncated")};
    kind[n] = SET_RAW;
    rows[n++] =
        (TuiCmd){setting_check(rows_arena, md_raw(), STR("Display raw")),
                 md_raw() ? STR("No Markdown or syntax highlighting")
                          : STR("Markdown and syntax highlighting")};
    kind[n] = SET_STREAM;
    rows[n++] =
        (TuiCmd){setting_check(rows_arena, cfg->stream, STR("Stream replies")),
                 STR("Paint a reply as it arrives, not once it is whole")};
    kind[n] = SET_IGNORED;
    rows[n++] = (TuiCmd){
        setting_check(rows_arena, tui_show_ignored(), STR("Ignored files")),
        STR("Offer what .gitignore and .ignore exclude")};
    kind[n] = SET_TELEMETRY;
    rows[n++] =
        (TuiCmd){setting_check(rows_arena, telemetry_on(), STR("Telemetry")),
                 STR("An anonymized debug log, for a bug report")};
    kind[n] = SET_SHOW_INSTRUCTIONS;
    rows[n++] = (TuiCmd){
        setting_check(rows_arena, ag->show_instructions,
                      STR("Show instructions")),
        STR("Reveal the active system and project instructions in the transcript")};
    kind[n] = SET_AUTO_TITLE;
    rows[n++] = (TuiCmd){
        setting_check(rows_arena, cfg->auto_title, STR("Name sessions")),
        STR("Name a session after its first turn, with a small model configured")};
    kind[n] = SET_RESUME_LAST;
    rows[n++] = (TuiCmd){
        setting_check(rows_arena, cfg->resume_last, STR("Resume last session")),
        STR("Start in this directory's newest session instead of the welcome screen")};

    /* One checkbox per tool a turn may call. A tool the mode does not offer
     * is still listed, since turning bash off is a statement about the
     * session rather than about plan mode. The two plan-mode tools are
     * absent: the agent loop answers them, so "disabled" would mean a mode
     * that cannot end.
     *
     * Nine rows are held back for the option rows below: a registry that
     * outgrew the array is a screen missing its settings, not its tools. */
    for (size_t i = 0; i < reg->n && n + 9 < SET_MAX_ROWS; i++) {
        if (!tools_can_disable(reg, i)) continue;
        kind[n] = SET_TOOL;
        v->tool[n] = i;
        rows[n++] = (TuiCmd){
            setting_check(rows_arena, !tools_disabled(reg, i), reg->name[i]),
            reg->brief[i]};
    }

    const Str wrap_opts[2] = {STR("Word"), STR("Justified")};
    kind[n] = SET_WRAP;
    rows[n] = (TuiCmd){setting_value(rows_arena, STR("Text wrap")),
                       setting_options(rows_arena, wrap_opts, 2,
                                       tui_justify() ? 1 : 0, &marks[n])};
    n++;
    const Str mode_opts[2] = {STR("Build"), STR("Plan")};
    kind[n] = SET_MODE;
    rows[n] =
        (TuiCmd){setting_value(rows_arena, STR("Mode")),
                 setting_options(rows_arena, mode_opts, 2,
                                 cfg->mode == MODE_PLAN ? 1 : 0, &marks[n])};
    n++;
    const Str permission_opts[2] = {STR("Ask"), STR("Free")};
    kind[n] = SET_PERMISSIONS;
    rows[n] =
        (TuiCmd){setting_value(rows_arena, STR("Permissions")),
                 setting_options(rows_arena, permission_opts, 2,
                                 cfg->permissions == PERMISSION_FREE ? 1 : 0,
                                 &marks[n])};
    n++;
    /* The rungs are too many to list beside the row, so this one says where
     * it stands and the arrows walk it. The rows outlive this call, so the
     * number is kept in the arena rather than in the frame it was formatted
     * in. */
    char tokens[16];
    snprintf(tokens, sizeof tokens, "%d", cfg->max_tokens);
    kind[n] = SET_MAX_TOKENS;
    rows[n++] = (TuiCmd){setting_value(rows_arena, STR("Max tokens")),
                         str_dup(rows_arena, str_c(tokens))};

    const Str compact_opts[3] = {STR("Off"), STR("Manual"), STR("Auto")};
    kind[n] = SET_COMPACT;
    rows[n] = (TuiCmd){setting_value(rows_arena, STR("Compact context")),
                       setting_options(rows_arena, compact_opts, 3,
                                       (size_t)cfg->compact, &marks[n])};
    n++;
    /* Only ever a share of a window the user configured: without one there
     * is no percentage to be past, and the row says so. */
    char at[32];
    if (cfg->context_window)
        snprintf(at, sizeof at, "%u%%", cfg->compact_at);
    else
        snprintf(at, sizeof at, "%u%% (no window set)", cfg->compact_at);
    kind[n] = SET_COMPACT_AT;
    rows[n++] = (TuiCmd){setting_value(rows_arena, STR("Compact at")),
                         str_dup(rows_arena, str_c(at))};
    const Str compact_model_opts[2] = {STR("Main"), STR("Small")};
    kind[n] = SET_COMPACT_MODEL;
    rows[n] = (TuiCmd){setting_value(rows_arena, STR("Compact with")),
                       setting_options(rows_arena, compact_model_opts, 2,
                                       cfg->compact_small ? 1 : 0, &marks[n])};
    n++;

    Str opt[SET_MAX_OPTIONS];
    if (cfg->reasoning_efforts.n && n < SET_MAX_ROWS) {
        size_t opts =
            list_options(cfg->reasoning_efforts, opt, SET_MAX_OPTIONS);
        kind[n] = SET_EFFORT;
        rows[n] =
            (TuiCmd){setting_value(rows_arena, STR("Reasoning effort")),
                     setting_options(rows_arena, opt, opts,
                                     list_at(opt, opts, cfg->reasoning_effort),
                                     &marks[n])};
        n++;
    }
    if (cfg->thinking_budgets.n && n < SET_MAX_ROWS) {
        size_t opts = list_options(cfg->thinking_budgets, opt, SET_MAX_OPTIONS);
        kind[n] = SET_BUDGET;
        rows[n] =
            (TuiCmd){setting_value(rows_arena, STR("Thinking budget")),
                     setting_options(rows_arena, opt, opts,
                                     list_at(opt, opts, cfg->thinking_budget),
                                     &marks[n])};
        n++;
    }
    v->n = n;
    return n;
}

static void rerender_or_defer(Agent *ag) {
    if (tui_busy()) {
        g_turn.rerender_pending = true;
        return;
    }
    rerender_conv(ag->conv, ag->cfg, ag->show_instructions, ag->scratch, 0);
}


static void find_expand(void *ud) {
    Agent *ag = ud;
    tui_set_find_expand(NULL, NULL);
    if (render_verbose()) return;
    render_set_verbose(true);
    rerender_or_defer(ag);
}


static b8 setting_shapes_request(u8 kind) {
    switch (kind) {
        case SET_TOOL:
        case SET_MODE:
        case SET_STREAM:
        case SET_MAX_TOKENS:
        case SET_EFFORT:
        case SET_BUDGET: return true;
        default: return false;
    }
}

static void settings_apply(SettingsView *v, size_t row, i32 delta) {
    Agent *ag = v->ag;
    Config *cfg = ag->cfg;
    Arena *scratch = ag->scratch;
    switch (v->kind[row]) {
        case SET_TOOL: {
            size_t t = v->tool[row];
            tools_set_disabled(ag->tools, t, !tools_disabled(ag->tools, t));
            remember_tools(ag->tools, scratch);
            cache_guard_cause(&g_cache, CACHE_CAUSE_TOOLS, 0);
            break;
        }
        case SET_EFFORT:
        case SET_BUDGET: {
            b8 is_effort = v->kind[row] == SET_EFFORT;
            Str next = list_step(
                is_effort ? cfg->reasoning_efforts : cfg->thinking_budgets,
                is_effort ? cfg->reasoning_effort : cfg->thinking_budget,
                delta);
            if (!remember_reasoning(cfg, scratch, is_effort, next))
                tui_notice(STR(
                    "setting changed but was not remembered: could not write provider settings"));
            break;
        }
        case SET_VERBOSE:
            render_set_verbose(!render_verbose());
            remember_ui_bool(scratch, CONF_VERBOSE_TOOLS, render_verbose());
            rerender_or_defer(ag);
            break;
        case SET_RAW:
            md_set_raw(!md_raw());
            remember_ui_bool(scratch, CONF_RAW_MARKDOWN, md_raw());
            rerender_or_defer(ag);
            break;
        case SET_STREAM:
            cfg->stream = !cfg->stream;
            remember_ui_bool(scratch, CONF_STREAM, cfg->stream);
            break;
        case SET_IGNORED:
            tui_set_show_ignored(!tui_show_ignored());
            remember_ui_bool(scratch, CONF_SHOW_IGNORED, tui_show_ignored());
            break;
        case SET_TELEMETRY:
            if (!telemetry_set(!telemetry_on(), scratch))
                tui_notice(STR(
                    "setting changed but was not remembered: could not write state"));
            break;
        case SET_WRAP:
            tui_set_justify(!tui_justify());
            remember_ui(scratch, CONF_WRAP,
                        tui_justify() ? STR("justified") : STR("word"));
            break;
        case SET_MODE:
            agent_set_mode(ag, cfg->mode == MODE_PLAN ? MODE_BUILD : MODE_PLAN);
            break;
        case SET_PERMISSIONS:
            agent_set_permissions(ag, cfg->permissions == PERMISSION_ASK
                                          ? PERMISSION_FREE
                                          : PERMISSION_ASK);
            break;
        case SET_SHOW_INSTRUCTIONS:
            ag->show_instructions = !ag->show_instructions;
            remember_ui_bool(scratch, CONF_SHOW_INSTRUCTIONS,
                             ag->show_instructions);
            rerender_or_defer(ag);
            break;
        case SET_AUTO_TITLE:
            cfg->auto_title = !cfg->auto_title;
            remember_ui_bool(scratch, CONF_AUTO_TITLE, cfg->auto_title);
            break;
        case SET_RESUME_LAST:
            cfg->resume_last = !cfg->resume_last;
            remember_ui_bool(scratch, CONF_RESUME_LAST, cfg->resume_last);
            break;
        case SET_MAX_TOKENS:
            cfg->max_tokens = max_tokens_step(cfg->max_tokens, delta);
            {
                char value[16];
                snprintf(value, sizeof value, "%d", cfg->max_tokens);
                remember_ui(scratch, CONF_MAX_TOKENS, str_c(value));
            }
            break;
        case SET_COMPACT: {
            const Str names[3] = {STR("off"), STR("manual"), STR("auto")};
            i32 next = (i32)cfg->compact + (delta > 0 ? 1 : -1);
            if (next > 2) next = 0;
            if (next < 0) next = 2;
            cfg->compact = (CompactMode)next;
            ag->compact_seen = false;
            ag->compact_short = false;
            remember_ui(scratch, CONF_COMPACT, names[next]);
            break;
        }
        case SET_COMPACT_AT: {
            cfg->compact_at = compact_at_step(cfg->compact_at, delta);
            ag->compact_seen = false;
            ag->compact_short = false;
            char value[16];
            snprintf(value, sizeof value, "%u", cfg->compact_at);
            remember_ui(scratch, CONF_COMPACT_AT, str_c(value));
            break;
        }
        case SET_COMPACT_MODEL:
            cfg->compact_small = !cfg->compact_small;
            remember_ui(scratch, CONF_COMPACT_MODEL,
                        cfg->compact_small ? STR("small") : STR("main"));
            break;
        default: break;
    }
}

static void settings_act(void *ud, size_t row, i32 delta) {
    SettingsView *v = ud;
    if (row >= v->n) return;
    if (!tui_busy()) {
        settings_apply(v, row, delta);
        return;
    }
    if (setting_shapes_request(v->kind[row])) {
        tui_notice(STR("that one shapes the request; it changes once the "
                       "turn ends"));
        return;
    }
    /* Mid-turn the scratch arena belongs to the request in flight: what
     * remembering a choice writes through it is borrowed from above the
     * request's own allocations and given back. */
    Arena *scratch = v->ag->scratch;
    size_t mark = scratch->off;
    settings_apply(v, row, delta);
    scratch->off = mark;
}

/* Static: the rows and their bookkeeping are larger than a frame of the
 * command loop should carry, and a screen opened during a turn outlives the
 * frame that opened it. */
static const TuiSettings *settings_screen(Agent *ag) {
    static SettingsView view;
    static TuiSettings set;
    view.ag = ag;
    arena_init(&view.rows_arena, g_screen, sizeof g_screen);
    set = (TuiSettings){
        .rows = view.rows,
        .marks = view.marks,
        .max = SET_MAX_ROWS,
        .build = settings_build,
        .act = settings_act,
        .ud = &view,
    };
    return &set;
}

static void choose_settings(Agent *ag) {
    tui_settings(STR("settings"), settings_screen(ag));
}

/* Open all text behind block `i`. The view copies it before returning, so a
 * decoded argument and the runs a highlighter answered with may live in the
 * running request's scratch tail without making that tail part of the
 * window's lifetime. Only the part that carries source is offered runs: a
 * shell run's output is what a command printed, not code. */
static b8 open_block_view(Agent *ag, size_t i) {
    const Conv *c = ag->conv;
    TuiViewPart parts[2] = {0};
    size_t shown[2] = {0};
    size_t part_n = 0;
    char name_buf[64];
    i32 len = 0;
    size_t scratch_mark = ag->scratch->off;
    YhlResult *syntax =
        arena_alloc(ag->scratch, sizeof *syntax, alignof(YhlResult));
    if (c->role[i] == M_USER && conv_is_shell(c, i)) {
        parts[part_n].text =
            render_shell_text(c->text[i], &shown[part_n], syntax);
        parts[part_n].syntax = syntax;
        part_n++;
        parts[part_n].text =
            render_result_text(STR("shell"), (Str){0}, c->shell_out[i], NULL,
                               &shown[part_n], NULL);
        part_n++;
        len = snprintf(name_buf, 32, "shell run");
    } else if (c->role[i] == M_ASSISTANT && conv_is_call(c, i)) {
        Str name = c->tool_name[i];
        parts[part_n].text = render_call_text(name, c->text[i], ag->scratch,
                                              &shown[part_n], syntax);
        parts[part_n].syntax = syntax;
        part_n++;
        len = snprintf(name_buf, sizeof name_buf, "%.*s input", (i32)name.n,
                       name.p);
    } else if (c->role[i] == M_TOOL) {
        size_t call = call_slot(c, i);
        Str name = call == CONV_NONE ? (Str){0} : c->tool_name[call];
        Str args = call == CONV_NONE ? (Str){0} : c->text[call];
        parts[part_n].text = render_result_text(
            name, args, c->text[i], ag->scratch, &shown[part_n], syntax);
        parts[part_n].syntax = syntax;
        part_n++;
        len = snprintf(name_buf, sizeof name_buf, "%.*s output", (i32)name.n,
                       name.p);
    } else {
        ag->scratch->off = scratch_mark;
        return false;
    }
    Str title = {name_buf,
                 len > 0 && (size_t)len < sizeof name_buf ? (size_t)len : 0};
    size_t start = 0, lines = 0;
    for (size_t p = 0; p < part_n; p++) {
        if (!parts[p].text.n) continue;
        if (lines) lines++;
        size_t part_top = lines;
        size_t part_lines = str_lines(parts[p].text);
        lines += part_lines;
        if (part_lines > shown[p]) start = part_top + shown[p];
    }
    b8 opened = lines && tui_view_open(title, parts, part_n, start);
    ag->scratch->off = scratch_mark;
    return opened;
}

/* Enter while the assistant is working. Only the commands that leave the
 * conversation, the request in flight and the streaming transcript alone run
 * where they stand; the screens they open are driven by the same poll that
 * keeps the turn alive. Anything else is refused and handed back to the
 * composer, since the turn it would change is the one still running. */
static b8 on_busy_command(Str line, void *ud) {
    Agent *ag = ud;
    /* An attachment belongs to the message after this one, so it runs where
     * it stands: the image joins the draft in the composer and goes out with
     * whatever is sent next. Ctrl-V and the path popup submit this. It is
     * read from the line rather than the bounded copy below, since a path is
     * longer than any command name. */
    if (str_eq(line, STR("/attach")) || str_starts(line, STR("/attach "))) {
        attach_image(ag, str_drop(line, 7), line);
        telemetry_command(STR("/attach"));
        return true;
    }
    char cmd[64];
    if (!line.n || line.n >= sizeof cmd) return false;
    memcpy(cmd, line.p, line.n);
    cmd[line.n] = '\0';
    /* A click on a block's tail is answered where it lands: with a window
     * over the block while the turn owns the transcript, and by folding the
     * block itself once the turn has stopped writing to it. */
    if (!strncmp(cmd, "/expand ", 8)) {
        unsigned long id = strtoul(cmd + 8, NULL, 10);
        if (!id || id > ag->conv->n) return false;
        if (tui_busy()) {
            if (!open_block_view(ag, id - 1))
                tui_notice(STR("could not open that block in a window"));
            return true;
        }
        ag->conv->expanded[id - 1] = !ag->conv->expanded[id - 1];
        rerender_or_defer(ag);
        return true;
    }
    Str name = {cmd, resolve_alias(cmd, line.n, sizeof cmd)};

    b8 ran;
    if (str_eq(name, STR("/settings")))
        ran = tui_settings_open(STR("settings"), settings_screen(ag));
    else if (str_eq(name, STR("/statusline")))
        ran = tui_settings_open(STR("status line"),
                                statusline_screen(ag->scratch));
    else if (str_eq(name, STR("/about")))
        ran = tui_info_open(STR("about " AGENT_NAME), k_about, ABOUT_N);
    else if (str_eq(name, STR("/keys")))
        ran = tui_info_open(STR("keyboard shortcuts"), g_keys, keys_rows());
    else if (str_eq(name, STR("/copy"))) {
        copy_last_reply(ag->conv);
        ran = true;
    } else if (command_offered(name)) {
        notice_fmt("%.*s waits until the turn ends; Esc stops the turn",
                   (i32)name.n, name.p);
        return false;
    } else {
        notice_fmt("unknown command: %.*s", (i32)name.n, name.p);
        return false;
    }
    if (ran) telemetry_command(name);
    return ran;
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
        say_conv_full();
        return;
    }

    g_got_sigint = 0;
    say_busy("running shell");
    render_shell_call(stored, (u32)(slot + 1), false);
    f64 started = agent_now_seconds();
    Buf out;
    buf_init(&out, ag->scratch, 4096);
    char err[256] = {0};
    if (!shell_capture(cmd, &out, err, sizeof err))
        buf_error(&out, err, "shell failed");
    u32 ms = elapsed_ms(started);
    Str result = keep_result(ag->persist, buf_finish(&out));
    TelEvent e;
    tel_open(&e, "shell");
    tel_shape(&e, "command", cmd);
    tel_int(&e, "ms", (i64)ms);
    tel_shape(&e, "output", result);
    tel_send(&e);
    conv->shell_out[slot] = result;
    conv->ms[slot] = ms;
    render_tool_result(STR("shell"), (Str){0}, result, ag->scratch,
                       (u32)(slot + 1), false, ms);
    save_session(ag);

    tui_set_status("ready");
    tui_activity_end();
}


static b8 agent_turn(Agent *ag, Str text);

#define TITLE_EXCERPT_BYTES 1024


#define TITLE_MAX_TOKENS 1024

/* Point `small` at the endpoint that serves the small model, which may not be
 * the one the conversation is on: the URL, the API and the key all come from
 * the stored provider, and only this request goes there. False when that
 * provider is gone or its key cannot be read, which leaves the errand undone
 * rather than sending what it carries somewhere else. */
static b8 small_model_endpoint(Config *small, Str name, Str model, b8 manual,
                               Arena *scratch) {
    size_t mark = scratch->off;
    Endpoints eps;
    endpoints_load(&eps, scratch);
    size_t i = endpoints_find(&eps, name);
    char err[AGENT_MAX_PATH + 96] = {0};
    Str key = i == ENDPOINT_NONE
                  ? (Str){0}
                  : endpoints_key(name, scratch, scratch, err, sizeof err);
    b8 ok = i != ENDPOINT_NONE && !err[0]
            && config_set_endpoint(small, name, eps.base_url[i], model,
                                   eps.api[i], key);
    scratch->off = mark;
    if (!ok && manual)
        notice_fmt("the small model's provider %.*s is not usable", (i32)name.n,
                   name.p);
    return ok;
}

/* The small model as a request of its own: the same connection unless a
 * provider was stored for it, and without the main model's reasoning
 * controls, which describe that model rather than this one. False when none
 * is configured or its provider is gone, which leaves the errand undone
 * rather than sending it somewhere else. `manual` says the user asked, which
 * is what decides whether a missing piece is reported. */
static b8 small_config(Config *small, const Config *cfg, Arena *scratch,
                       b8 manual) {
    if (!cfg->small_model.n) return false;
    *small = *cfg;
    if (!config_set_model(small, cfg->small_model)) {
        if (manual) tui_notice(STR("could not use the small model"));
        return false;
    }
    if (cfg->small_provider.n && !str_eq(cfg->small_provider, cfg->provider)
        && !small_model_endpoint(small, cfg->small_provider, cfg->small_model,
                                 manual, scratch))
        return false;
    small->reasoning_effort = (Str){0};
    small->thinking_budget = (Str){0};
    small->reasoning_template = (Str){0};
    return true;
}

typedef enum {
    COMPACT_SUM_OK,
    COMPACT_SUM_NOMEM,
    COMPACT_SUM_INTERRUPTED,
    COMPACT_SUM_EMPTY,
    COMPACT_SUM_ERROR
} CompactOutcome;

/* One request that condenses slots [0, upto) into a checkpoint message.
 *
 * The request is made over a copy of that head, appended to in a part of the
 * persistent arena that is rewound either way, so a compaction that fails,
 * is interrupted or comes back empty leaves the conversation and the session
 * file it is appending to exactly as they were. The checkpoint is built in
 * the scratch arena, which outlives that rewind: the caller copies it out
 * before resetting scratch. */
static CompactOutcome compact_summarize(Agent *ag, size_t upto, Str *out,
                                        char *err, size_t err_cap) {
    Arena *persist = ag->persist;
    Config *cfg = ag->cfg;
    *out = (Str){0};

    size_t mark = persist->off;
    Conv tmp;
    /* Room for the question, the reply, and whatever calls a model asks for
     * instead of answering: a full copy would be reported as an error, and
     * the point of the copy is that it cannot cost the conversation. */
    if (!conv_clone_head(&tmp, ag->conv, upto, persist,
                         AGENT_MAX_TOOL_CALLS + 2)) {
        persist->off = mark;
        return COMPACT_SUM_NOMEM;
    }
    /* Slot 0 is the system prompt of the conversation being summarized, which
     * describes how to do the work rather than how to describe it. */
    tmp.role[0] = M_SYSTEM;
    tmp.text[0] = prompt_compact();
    tmp.anthropic_thinking[0] = (Str){0};
    if (conv_add(&tmp, M_USER, prompt_compact_ask()) == CONV_NONE) {
        persist->off = mark;
        return COMPACT_SUM_NOMEM;
    }

    /* Static: a Config carries kilobytes of owned buffers, and this runs from
     * a turn's frame rather than from one with room for them. */
    static Config small;
    b8 use_small =
        cfg->compact_small && small_config(&small, cfg, ag->scratch, false);

    /* Busy is restored rather than cleared: this also runs inside a turn,
     * which stays busy until it ends. */
    b8 was_busy = tui_busy();
    g_got_sigint = 0;
    tui_set_busy(true);
    say_busy("compacting");

    Provider p = {
        .cfg = use_small ? &small : cfg,
        .tools = use_small ? NULL : ag->tools,
        .conv = &tmp,
        .persist = persist,
        .scratch = ag->scratch,
        .on_retry = on_retry,
        .on_idle = on_idle,
        .idle_fd = tui_input_fd(),
        .interrupt_flag = &g_got_sigint,
    };
    f64 started = agent_now_seconds();
    arena_reset(ag->scratch);
    i32 rc = provider_run(&p, err, err_cap);
    tui_set_busy(was_busy);
    tui_activity_end();
    tui_set_status("ready");

    Str summary = {0};
    if (rc >= 0) {
        for (size_t i = tmp.n; i-- > upto;) {
            if (tmp.role[i] != M_ASSISTANT || conv_is_call(&tmp, i)) continue;
            if (tmp.text[i].n) {
                summary = str_trim(tmp.text[i]);
                break;
            }
        }
    }

    TelEvent ce;
    tel_open(&ce, "compact");
    tel_int(&ce, "messages", (i64)upto);
    tel_int(&ce, "kept", (i64)(ag->conv->n - upto));
    tel_bool(&ce, "small", use_small);
    tel_int(&ce, "rc", rc);
    tel_int(&ce, "ms", (i64)((agent_now_seconds() - started) * 1000.0));
    tel_shape(&ce, "summary", summary);
    tel_send(&ce);

    b8 interrupted = g_got_sigint != 0;
    if (interrupted) g_got_sigint = 0;
    CompactOutcome outcome = interrupted            ? COMPACT_SUM_INTERRUPTED
                             : rc == PROVIDER_EMPTY ? COMPACT_SUM_EMPTY
                             : rc < 0               ? COMPACT_SUM_ERROR
                             : !summary.n           ? COMPACT_SUM_EMPTY
                                                    : COMPACT_SUM_OK;

    if (outcome == COMPACT_SUM_OK) {
        Buf b;
        const TodoList *todos = todo_current();
        buf_init(&b, ag->scratch,
                 summary.n + 256 + todos->n * (AGENT_MAX_TODO_TEXT + 24));
        buf_puts(&b, STR("# Context checkpoint\n\nThe conversation before this "
                         "point was compacted into the summary below. Continue "
                         "the work from it.\n\n"));
        buf_puts(&b, summary);
        if (todos->n) {
            buf_puts(&b, STR("\n\n## Step list\n\nThe todo call that carried "
                             "this list was compacted away. Send the whole "
                             "list again with the next update.\n\n"));
            todo_write_md(&b, todos);
        }
        *out = buf_ok(&b) ? buf_finish(&b) : (Str){0};
        if (!out->n) outcome = COMPACT_SUM_NOMEM;
    }
    persist->off = mark;
    return outcome;
}

/* /compact: the conversation condensed into a checkpoint, then a new session
 * that starts from it. The tail automatic compaction would keep is kept here
 * too, so asking for room does not cost the work in hand. Where there is no
 * window to budget a tail against, or too little conversation to be worth
 * splitting, the whole of it is summarized and the new session opens on the
 * checkpoint alone. */
static void compact_session(Agent *ag) {
    Conv *conv = ag->conv;
    Arena *persist = ag->persist;
    if (conv->n <= 1) {
        tui_notice(STR("nothing to compact yet"));
        return;
    }
    if (no_provider(ag->cfg)) {
        tui_notice(setup_hint(ag->cfg, ag->scratch));
        return;
    }

    size_t keep = ctx_compact_split(&g_ctx, conv);
    if (!keep) keep = conv->n;
    Str built = {0};
    char err[256] = {0};
    switch (compact_summarize(ag, keep, &built, err, sizeof err)) {
        case COMPACT_SUM_OK: break;
        case COMPACT_SUM_NOMEM:
            tui_notice(STR("out of memory compacting"));
            return;
        case COMPACT_SUM_INTERRUPTED:
            tui_notice(
                STR("compaction interrupted: this session is unchanged"));
            return;
        case COMPACT_SUM_EMPTY:
            tui_notice(
                STR("the model sent no summary: this session is unchanged"));
            return;
        case COMPACT_SUM_ERROR:
            notice_fmt("could not compact: %s; this session is unchanged", err);
            return;
    }

    char title[AGENT_MAX_TITLE + 1];
    size_t title_n = ag->sess->title.n < sizeof title ? ag->sess->title.n : 0;
    if (title_n) memcpy(title, ag->sess->title.p, title_n);

    if (keep < conv->n) {
        /* Nothing is given back to the persistent arena: the kept tail still
         * points into it, and only /clear rewinds that far. */
        Str stored = str_dup(persist, built);
        if (!stored.p || !conv_compact_head(conv, keep, stored)) {
            tui_notice(STR("out of memory keeping the summary"));
            return;
        }
        session_begin(ag->sess);
        tui_clear();
        rerender_conv(conv, ag->cfg, ag->show_instructions, ag->scratch, 0);
        if (!save_session(ag)) {
            ctx_sync(&g_ctx, conv);
            return;
        }
        if (title_n) session_set_title(ag->sess, (Str){title, title_n});
        ctx_sync(&g_ctx, conv);
        tui_notice(STR("compacted: a new session continues from the summary "
                       "and the newest work"));
        return;
    }

    conv_truncate(conv, 1);
    cache_guard_cause(&g_cache, CACHE_CAUSE_COMPACT, 0);
    ag->pending_n = 0;
    persist->off = ag->mark;
    Str stored = str_dup(persist, built);
    if (!stored.p) {
        tui_notice(STR("out of memory keeping the summary"));
        return;
    }

    session_begin(ag->sess);
    tui_clear();
    if (conv_add(conv, M_USER, stored) == CONV_NONE) {
        say_conv_full();
        return;
    }
    conv_set_checkpoint(conv, conv->n - 1);
    render_user_message(conv, conv->n - 1);
    if (!save_session(ag)) {
        ctx_sync(&g_ctx, conv);
        return;
    }
    if (title_n) session_set_title(ag->sess, (Str){title, title_n});
    ctx_sync(&g_ctx, conv);
    tui_notice(STR("compacted: a new session continues from the summary"));
}

static void say_compaction(Str text) {
    if (g_turn.one_shot)
        one_shot_diag("context", (Str){0}, text);
    else
        tui_notice(text);
}

/* Compaction that keeps the thread: the older turns become one checkpoint
 * message and the newest rounds that fit the tail budget are replayed
 * exactly as they stand, so the model keeps the detail it is working from.
 * Safe between rounds of a turn, since the kept tail begins at a round
 * boundary and no assistant call is separated from the results that answer
 * it.
 *
 * A session file is only appended to, so the compacted conversation
 * continues in a new one the way /compact does and the old file keeps every
 * turn it recorded. Nothing is given back to the persistent arena: the kept
 * tail still points into it, and only /clear rewinds that far.
 *
 * `keep` is the first slot of that tail, which the caller has already
 * measured. `interrupted` reports a Ctrl-C that landed in the request, which
 * is consumed there and left for the turn this runs inside to act on. */
static b8 compact_auto(Agent *ag, size_t keep, b8 *interrupted) {
    Conv *conv = ag->conv;
    *interrupted = false;
    if (no_provider(ag->cfg)) return false;
    if (keep < 2 || keep > conv->n) return false;

    Str built = {0};
    char err[256] = {0};
    CompactOutcome rc = compact_summarize(ag, keep, &built, err, sizeof err);
    if (rc == COMPACT_SUM_INTERRUPTED) {
        *interrupted = true;
        return false;
    }
    if (rc == COMPACT_SUM_ERROR) {
        notice_fmt("could not compact the context: %s", err);
        return false;
    }
    if (rc != COMPACT_SUM_OK) {
        say_compaction(STR("could not compact the context: this session "
                           "continues whole"));
        return false;
    }

    Str stored = str_dup(ag->persist, built);
    if (!stored.p || !conv_compact_head(conv, keep, stored)) {
        say_compaction(STR("could not compact the context: this session "
                           "continues whole"));
        return false;
    }
    cache_guard_cause(&g_cache, CACHE_CAUSE_COMPACT, 0);

    char title[AGENT_MAX_TITLE + 1];
    size_t title_n = ag->sess->title.n < sizeof title ? ag->sess->title.n : 0;
    if (title_n) memcpy(title, ag->sess->title.p, title_n);
    session_begin(ag->sess);
    b8 saved = save_session(ag);
    if (saved && title_n) session_set_title(ag->sess, (Str){title, title_n});

    if (!g_turn.one_shot)
        rerender_conv(conv, ag->cfg, ag->show_instructions, ag->scratch, 0);
    say_compaction(
        saved
            ? STR("context compacted: the older work is now a summary")
            : STR("context compacted in memory but the new session was not saved"));
    ctx_sync(&g_ctx, conv);
    return true;
}

/* Move the elision boundary up, once the conversation is heavy enough for the
 * rewrite to pay for the cached prefix it discards. Checked between rounds
 * beside the compaction check, and before it: eliding is what buys room while
 * a compaction is still avoidable.
 *
 * The gain guard keeps a conversation of prose from paying a full rebuild for
 * a few hundred tokens; there compaction is the cheaper answer, and it runs
 * one threshold later. */
static void elide_if_needed(Agent *ag) {
    const Config *cfg = ag->cfg;
    if (!cfg->elide_at || !ctx_over(&g_ctx, ag->conv, cfg->elide_at)) return;
    size_t gain = ctx_elide_gain(&g_ctx, ag->conv);
    if (gain < g_ctx.window * AGENT_ELIDE_MIN_GAIN_PCT / 100) return;
    if (!conv_elide_advance(ag->conv)) return;
    cache_guard_cause(&g_cache, CACHE_CAUSE_ELIDE, gain);
    ctx_sync(&g_ctx, ag->conv);
}

/* Checked between rounds, where the conversation is consistent and the usage
 * the provider last reported measures it. Nothing here interrupts a stream:
 * a request already in flight is the provider's to accept or refuse. One
 * attempt, or one word about it, per crossing of the threshold.
 * True when Ctrl-C landed in the request, which the turn has to act on. */
static b8 compact_if_needed(Agent *ag) {
    const Config *cfg = ag->cfg;
    if (cfg->compact == COMPACT_OFF
        || !ctx_over(&g_ctx, ag->conv, cfg->compact_at)) {
        ag->compact_seen = false;
        ag->compact_short = false;
        return false;
    }
    if (ag->compact_seen) return false;
    if (cfg->compact == COMPACT_MANUAL) {
        ag->compact_seen = true;
        say_compaction(STR("the context window is nearly full: /compact to "
                           "continue from a summary"));
        return false;
    }
    /* Too little older than the tail to pay for the request: either this
     * turn's work is most of the conversation, or what fills the window is
     * the system prompt and the tool schemas. Said once, and the crossing is
     * left open: a turn still running grows a head as it goes. */
    size_t keep = ctx_compact_split(&g_ctx, ag->conv);
    if (!keep || !ctx_compact_worth(&g_ctx, ag->conv, keep)) {
        if (ag->compact_short) return false;
        ag->compact_short = true;
        say_compaction(STR("the context window is nearly full: nothing old "
                           "enough to compact yet"));
        return false;
    }
    ag->compact_seen = true;
    b8 interrupted = false;
    /* A compaction that worked earns the next one: a long turn crosses the
     * threshold again as it grows. */
    if (compact_auto(ag, keep, &interrupted)) {
        ag->compact_seen = false;
        ag->compact_short = false;
    }
    return interrupted;
}

/* Name the session from its first exchange, through the small model. Built
 * the way /compact builds its request: a conversation of its own in a part
 * of the persistent arena that is rewound either way, so a name that does
 * not arrive costs the session nothing.
 *
 * `manual` says the user asked, which is what decides whether a missing
 * piece is reported: the automatic attempt happens on its own and says
 * nothing when it cannot. `interrupted`, when given, reports a Ctrl-C that
 * landed in the errand: the flag is consumed here, and a caller running this
 * inside a turn has to act on it. */
static b8 name_session(Agent *ag, b8 manual, b8 *interrupted_out) {
    Session *sess = ag->sess;
    const Conv *conv = ag->conv;
    const Config *cfg = ag->cfg;
    Arena *persist = ag->persist;

    if (interrupted_out) *interrupted_out = false;
    Str first_user = {0}, first_reply = {0};
    for (size_t i = 0; i < conv->n && !first_reply.n; i++) {
        if (conv->role[i] == M_USER && !conv_is_shell(conv, i) && !first_user.n)
            first_user = conv->text[i];
        else if (first_user.n && conv->role[i] == M_ASSISTANT
                 && !conv_is_call(conv, i) && conv->text[i].n)
            first_reply = conv->text[i];
    }
    if (!sess->path.n || !first_user.n) {
        if (manual) tui_notice(STR("nothing to name yet"));
        return false;
    }
    if (no_provider(cfg)) {
        if (manual) tui_notice(setup_hint(cfg, ag->scratch));
        return false;
    }
    if (!cfg->small_model.n) {
        if (manual)
            tui_notice(STR("no small model is configured: pick one with "
                           "Ctrl-S in /model"));
        return false;
    }
    sess->title_tried = true;

    size_t mark = persist->off;
    Conv tmp;
    Buf ask;
    buf_init(&ask, persist, TITLE_EXCERPT_BYTES * 2 + 128);
    buf_puts(&ask, prompt_title_ask());
    buf_puts(&ask, STR("\n\nUser: "));
    buf_puts(&ask, str_clip_utf8(first_user, TITLE_EXCERPT_BYTES));

    if (first_reply.n) {
        buf_puts(&ask, STR("\n\nAssistant: "));
        buf_puts(&ask, str_clip_utf8(first_reply, TITLE_EXCERPT_BYTES));
    }
    if (!conv_init(&tmp, persist, 4) || !buf_ok(&ask)
        || conv_add(&tmp, M_SYSTEM, prompt_title()) == CONV_NONE
        || conv_add(&tmp, M_USER, buf_finish(&ask)) == CONV_NONE) {
        persist->off = mark;
        if (manual) tui_notice(STR("out of memory naming this session"));
        return false;
    }

    /* Static: a Config carries kilobytes of owned buffers, and this runs
     * from the command loop rather than from a frame with room for them. The
     * reasoning controls describe the main model, so the small one is asked
     * without them. */
    static Config small;
    if (!small_config(&small, cfg, ag->scratch, manual)) {
        persist->off = mark;
        return false;
    }
    if (small.max_tokens > TITLE_MAX_TOKENS)
        small.max_tokens = TITLE_MAX_TOKENS;

    say_busy("naming");

    Provider p = {
        .cfg = &small,
        .tools = NULL,
        .conv = &tmp,
        .persist = persist,
        .scratch = ag->scratch,
        .on_idle = on_idle,
        .idle_fd = tui_input_fd(),
        .interrupt_flag = &g_got_sigint,
    };
    char err[256] = {0};
    f64 started = agent_now_seconds();
    arena_reset(ag->scratch);
    i32 rc = provider_run(&p, err, sizeof err);

    if (manual) {
        tui_activity_end();
        tui_set_status("ready");
    }
    /* An interrupt skips the naming, and the flag is consumed here so it
     * cannot leak into the turn the user types next. A caller that runs this
     * in the middle of a turn is told instead, through `interrupted_out`. */
    b8 interrupted = g_got_sigint != 0;
    if (interrupted) g_got_sigint = 0;
    if (interrupted_out) *interrupted_out = interrupted;

    Str title = {0};
    if (rc >= 0 && !interrupted) {
        for (size_t i = tmp.n; i-- > 0;) {
            if (tmp.role[i] != M_ASSISTANT || conv_is_call(&tmp, i)) continue;
            if (tmp.text[i].n) {
                title = str_trim(tmp.text[i]);
                break;
            }
        }
    }
    b8 named = title.n && session_set_title(sess, title);

    TelEvent e;
    tel_open(&e, "title");
    tel_bool(&e, "auto", !manual);
    tel_int(&e, "rc", rc);
    tel_int(&e, "ms", (i64)((agent_now_seconds() - started) * 1000.0));
    tel_shape(&e, "title", title);
    tel_send(&e);

    persist->off = mark;
    if (named && sess->title.n) {
        notice_fmt("session named: %.*s", (i32)sess->title.n, sess->title.p);
        return true;
    }
    if (!manual) return false;
    if (interrupted)
        tui_notice(STR("naming interrupted"));
    else if (rc < 0)
        notice_fmt("could not name this session: %s", err);
    else
        tui_notice(STR("the model sent no name for this session"));
    return false;
}

/* The conversation that produced the plan is dropped whole and the plan
 * becomes the first message of the next one. It still lives in the scratch
 * arena, which the persistent rewind below does not touch. */
static b8 agent_handoff(Agent *ag) {
    Str plan = ag->handoff;
    ag->handoff = (Str){0};
    conv_truncate(ag->conv, 1);
    cache_guard_begin(&g_cache);
    ag->pending_n = 0;
    ag->persist->off = ag->mark;
    plan = str_dup(ag->persist, plan);
    if (!plan.p) {
        if (g_turn.one_shot)
            one_shot_diag("error", (Str){0}, STR("out of memory keeping plan"));
        else
            tui_notice(STR("out of memory keeping the approved plan"));
        return false;
    }
    agent_set_mode(ag, MODE_BUILD);

    char title[AGENT_MAX_TITLE + 1];
    size_t title_n = ag->sess->title.n < sizeof title ? ag->sess->title.n : 0;
    if (title_n) memcpy(title, ag->sess->title.p, title_n);
    session_begin(ag->sess);
    if (title_n) session_set_title(ag->sess, (Str){title, title_n});
    tui_clear();
    return agent_turn(ag, plan);
}


static Str last_reply(const Conv *conv) {
    for (size_t i = conv->n; i-- > 0;) {
        if (conv->role[i] != M_ASSISTANT || conv_is_call(conv, i)) continue;
        Str reply = conv->text[i];
        while (
            reply.n
            && (reply.p[reply.n - 1] == '\n' || reply.p[reply.n - 1] == '\r'))
            reply.n--;
        return reply;
    }
    return (Str){0};
}


static void announce_interrupt(void) {
    if (g_turn.one_shot)
        one_shot_diag("error", (Str){0}, STR("interrupted"));
    else {
        tui_block();
        tui_write(STR("[interrupted]\n"));
    }
    tui_set_status("ready");
    g_got_sigint = 0;
}

/* The automatic naming, made from inside the turn that earns it: as soon as
 * one response is whole, rather than once every tool the turn asked for has
 * run, because that can be many minutes away and an interrupted turn would
 * never get there. One attempt per session, which `title_tried` records.
 * True when Ctrl-C landed in the errand, which the turn has to act on. */
static b8 name_session_now(Agent *ag) {
    if (g_turn.one_shot || !ag->cfg->auto_title || !ag->cfg->small_model.n
        || ag->sess->title.n || ag->sess->title_tried)
        return false;
    b8 interrupted = false;
    name_session(ag, false, &interrupted);
    return interrupted;
}

static b8 agent_turn(Agent *ag, Str text) {
    Conv *conv = ag->conv;

    size_t media_off = 0, media_n = 0;
    Str user_text = turn_bind_images(ag, text, &media_off, &media_n);
    ag->pending_n = 0;
    if (text.n && !user_text.p) {
        if (g_turn.one_shot)
            one_shot_diag("error", (Str){0}, STR("out of memory"));
        else {
            tui_block();
            tui_write(STR("[out of memory: /clear to start a new session]\n"));
        }
        return false;
    }
    if (conv_add(conv, M_USER, user_text) == CONV_NONE) {
        say_conv_full();
        return false;
    }
    if (media_n) conv_attach_media(conv, conv->n - 1, media_off, media_n);
    /* A new turn is a new crossing: an attempt that found nothing older than
     * the turns it keeps is worth making again once the conversation has
     * grown one. */
    ag->compact_seen = false;
    ag->compact_short = false;
    if (ag->echo) {
        tui_scroll_to_bottom();
        render_user_message(conv, conv->n - 1);
    }
    save_session(ag);

    ctx_sync(&g_ctx, conv);

    TelEvent te;
    tel_open(&te, "turn_start");
    tel_shape(&te, "prompt", text);
    tel_int(&te, "messages", (i64)conv->n);
    tel_int(&te, "images", (i64)media_n);
    tel_str(&te, "mode", mode_name(ag->cfg->mode));
    tel_send(&te);
    f64 turn_started = agent_now_seconds();
    i32 rounds = 0;

    b8 ok = false;
    NotifyKind ending = NOTIFY_TURN_FAILED;
    Str ending_text = {0};
    char ending_buf[256] = {0};
    g_got_sigint = 0;
    tui_set_busy(true);

    for (;;) {
        rounds++;
        if (g_got_sigint) {
            announce_interrupt();
            ending = NOTIFY_INTERRUPTED;
            break;
        }
        /* Before the request rather than during it: here the conversation is
         * consistent, and a summary that arrives is what the next request is
         * built from. */
        elide_if_needed(ag);
        if (compact_if_needed(ag)) {
            announce_interrupt();
            ending = NOTIFY_INTERRUPTED;
            break;
        }
        tui_set_status("thinking");
        tui_activity(STR("thinking"));
        g_turn.reasoning = false;
        g_turn.replying = false;
        Provider p = {
            .cfg = ag->cfg,
            .tools = ag->tools,
            .conv = conv,
            .persist = ag->persist,
            .scratch = ag->scratch,
            .on_text = on_text,
            .on_reason = on_reason,
            .on_tool_call = on_tool_call,
            .on_usage = on_usage,
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

        md_end();
        md_set_muted(false);

        save_session(ag);
        if (g_got_sigint) {
            announce_interrupt();
            ending = NOTIFY_INTERRUPTED;
            break;
        }
        if (rc < 0) {
            TelEvent ee;
            tel_open(&ee, "error");
            tel_str(&ee, "where", STR("provider"));
            tel_str(&ee, "detail", str_c(err));
            tel_send(&ee);
            if (g_turn.one_shot)
                one_shot_diag("provider error", (Str){0}, str_c(err));
            else {
                tui_block();
                tui_printf("[provider error: %s]\n", err);
            }
            tui_set_status("ready");
            snprintf(ending_buf, sizeof ending_buf, "%s", err);
            ending_text = str_c(ending_buf);
            break;
        }

        /* Read before the round ends, so a rebuild nothing asked for is said
         * next to the request that paid for it. */
        b8 cache_bad =
            cache_guard_observe(&g_cache, p.prompt_tokens, p.cache_read_tokens,
                                p.cache_creation_tokens);
        if (name_session_now(ag)) {
            announce_interrupt();
            ending = NOTIFY_INTERRUPTED;
            break;
        }
        if (rc == 0) {
            tui_set_status("ready");
            ok = true;
            ending = NOTIFY_TURN_DONE;
            break;
        }

        size_t tail = conv->n;
        TurnAction act = run_tool_calls(ag, before, tail);

        ctx_sync(&g_ctx, conv);
        todo_sync(conv, ag->scratch);
        if (act == TURN_FULL) {
            tui_set_status("ready");
            ending_text = STR("the conversation is full");
            break;
        }
        if (act == TURN_DENIED) {
            tui_set_status("ready");
            ending_text = STR("approval was required for a guarded tool call");
            break;
        }
        if (act == TURN_HANDOFF) {
            ok = true;
            ending = NOTIFY_TURN_DONE;
            break;
        }
        if (act == TURN_DONE) {
            tui_set_status("ready");
            ok = true;
            ending = NOTIFY_TURN_DONE;
            break;
        }
        /* The round the model asked for still runs: its results belong to the
         * calls already in the conversation, and leaving a call unanswered
         * would cost the next request more than the miss did. The loop stops
         * once they are in, the way an interrupt does. */
        if (cache_bad) {
            tui_set_status("ready");
            ending = NOTIFY_INTERRUPTED;
            ending_text = STR("an unexplained cache miss stopped the turn");
            break;
        }
        /* A follow-up cannot precede results for the tool calls it observed:
         * that would split the assistant call from its required result. Once
         * every result is appended, ending this turn lets the queued message
         * become the next user turn instead of asking the model to continue
         * along the old direction. */
        if (tui_queued_pending()) {
            tui_set_status("ready");
            ok = true;
            ending = NOTIFY_TURN_DONE;
            break;
        }
    }
    tui_set_busy(false);
    tui_activity_end();
    save_session(ag);
    tel_open(&te, "turn_end");
    tel_bool(&te, "ok", ok);
    tel_int(&te, "rounds", rounds);
    tel_int(&te, "ms", (i64)((agent_now_seconds() - turn_started) * 1000.0));
    tel_int(&te, "messages", (i64)conv->n);
    /* The budget a long session runs down; nothing on screen says where the
     * arenas stand. */
    tel_int(&te, "persist_used", (i64)arena_used(ag->persist));
    tel_int(&te, "scratch_used", (i64)arena_used(ag->scratch));
    todo_telemetry(&te);
    tel_send(&te);

    if (!ag->handoff.n && !tui_queued_pending()) {
        if (ending == NOTIFY_TURN_DONE) ending_text = last_reply(conv);
        notify_event(ending, ending_text,
                     (agent_now_seconds() - turn_started) * 1000.0);
    }
    if (ag->handoff.n) return agent_handoff(ag);
    /* The plan a handoff carries lives in the scratch arena this resets, so
     * it runs only on the path that keeps the conversation. */
    if (g_turn.rerender_pending) {
        g_turn.rerender_pending = false;
        rerender_conv(conv, ag->cfg, ag->show_instructions, ag->scratch, 0);
    }
    return ok;
}

/* Run submitted follow-ups without reopening the prompt between them. The
 * TUI owns their bytes until they are taken; agent_turn duplicates each one
 * into persist before polling can accept another. A submitted follow-up also
 * runs after an error or Ctrl-C: Esc is the operation that cancels the queue,
 * while Ctrl-C only stops the operation currently on screen. */
static b8 agent_turn_interactive(Agent *ag, Str text) {
    b8 ok = agent_turn(ag, text);
    while (tui_queued_pending()) ok = agent_turn(ag, tui_queued_take());
    return ok;
}

static void write_final_reply(const Conv *conv) {
    for (size_t i = conv->n; i-- > 0;) {
        if (conv->role[i] != M_ASSISTANT || conv_is_call(conv, i)) continue;
        Str reply = last_reply(conv);
        if (reply.n) fwrite(reply.p, 1, reply.n, stdout);
        fputc('\n', stdout);
        return;
    }
}

/* The image a restart runs. /proc/self/exe names it even where argv[0] was
 * replaced or is a bare command name; without it, only a command line
 * carrying a path can be checked, and a bare name is left to the PATH search
 * execvp does. */
static b8 restart_exe(char *out, size_t cap, const char *argv0) {
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", out, cap - 1); // flawfinder: ignore
    if (n > 0 && (size_t)n < cap - 1) {
        out[(size_t)n] = '\0';
        if (access(out, X_OK) == 0) return true;
    }
#endif
    size_t len = argv0 ? strlen(argv0) : 0;
    if (!len || len >= cap || !strchr(argv0, '/')) return false;
    memcpy(out, argv0, len + 1);
    return access(out, X_OK) == 0;
}

/* Hand the terminal to a fresh process with the same command line, which
 * reads the settings again: it reopens this session or greets with the
 * welcome screen exactly as `resume_last` says. The conversation is already
 * on disk, since a session is saved as it happens. exec keeps the pty and
 * runs no atexit handler, so what those release is released here and the
 * terminal is put back before the image is replaced. */
static void restart_agent(char **argv) {
    char exe[AGENT_MAX_PATH];
    b8 have = restart_exe(exe, sizeof exe, argv[0]);
    if (!have && (!argv[0] || !*argv[0])) {
        tui_notice(STR("could not find the " AGENT_NAME " to restart"));
        return;
    }
    jobs_stop();
    highlight_close();
    telemetry_close();
    tui_stop();
    if (have)
        execv(exe, argv);
    else
        execvp(argv[0], argv);
    fprintf(stderr, AGENT_NAME ": could not restart: %s\n", strerror(errno));
    exit(1);
}

i32 main(i32 argc, char **argv) {
#ifdef PR_SET_THP_DISABLE
    /* The large static arenas and TUI buffers are sparse. Under an "always"
     * THP policy a few writes would otherwise make their unused capacity
     * resident in 2 MiB increments. */
    (void)prctl(PR_SET_THP_DISABLE, 1L, 0L, 0L, 0L);
#endif
    CliOpts opts;
    switch (cli_parse(argc, argv, &opts)) {
        case CLI_DONE: return 0;
        case CLI_ERROR: return 2;
        case CLI_RUN: break;
    }
    if (opts.have_prompt && !opts.prompt.n) {
        fprintf(stderr, AGENT_NAME ": the prompt is empty\n");
        return 2;
    }

    Arena persist, scratch;
    arena_init(&persist, g_persist, sizeof g_persist);
    arena_init(&scratch, g_scratch, sizeof g_scratch);

    Config cfg;
    Conf conf;
    conf_resolve(&conf, &persist, &scratch);
    config_load(&cfg, &conf, &persist);
    cli_apply(&opts, &cfg);
    if (cfg.provider.n && !apply_model_profile(&cfg, &scratch)) {
        fprintf(stderr, AGENT_NAME ": cannot store model settings\n");
        return 1;
    }
    UiPrefs prefs;
    ui_prefs_load(&prefs, &conf);
    notify_init(&conf, &persist);
    web_search_init(&conf, &persist);
    arena_reset(&scratch);

    ToolRegistry tools;
    tools_init(&tools, &persist, cfg.shell_timeout_ms);
    b8 interactive =
        !opts.have_prompt && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    tools_set_interactive(interactive);

    char tools_err[128] = {0};
    if (cfg.disable_tools.n
        && !tools_disable_list(&tools, cfg.disable_tools, tools_err,
                               sizeof tools_err)) {
        fprintf(stderr, AGENT_NAME ": %s\n", tools_err);
        return 2;
    }
    char prompt_err[AGENT_MAX_PATH + 128] = {0};
    cfg.system_prompt =
        prompt_build(&tools, cfg.system_prompt, &persist, &scratch,
                     &cfg.system_sources, prompt_err, sizeof prompt_err);
    if (!cfg.system_prompt.n) {
        fprintf(stderr, AGENT_NAME ": %s\n", prompt_err);
        return 2;
    }
    arena_reset(&scratch);
    /* Built up front, so switching mode later is an assignment rather than a
     * file read mid-turn. */
    cfg.plan_prompt =
        prompt_build_plan(&tools, &persist, &scratch, &cfg.plan_sources,
                          prompt_err, sizeof prompt_err);
    if (!cfg.plan_prompt.n) {
        fprintf(stderr, AGENT_NAME ": %s\n", prompt_err);
        return 2;
    }
    arena_reset(&scratch);
    tools_set_mode(cfg.mode);

    History hist = {0};
    Arena hist_arena = {0};
    void *hist_mem = arena_alloc(&persist, AGENT_HISTORY_BYTES, 64);
    if (hist_mem) {
        arena_init(&hist_arena, hist_mem, AGENT_HISTORY_BYTES);
        if (history_init(&hist, &hist_arena, AGENT_MAX_HISTORY))
            history_load(&hist, history_path(&persist, &scratch), &scratch);
    }
    arena_reset(&scratch);

    Conv conv;
    if (!conv_init(&conv, &persist, cfg.max_messages)) {
        fprintf(stderr, AGENT_NAME ": cannot reserve %zu conversation slots\n",
                cfg.max_messages);
        return 1;
    }

    if (cfg.images && media_init(&g_media, &persist, AGENT_MAX_MEDIA))
        conv_set_media(&conv, &g_media);

    conv_add(&conv, M_SYSTEM,
             cfg.mode == MODE_PLAN ? cfg.plan_prompt : cfg.system_prompt);
    size_t session_mark = persist.off;

    // Without a resolvable data dir the conversation is not persisted.
    static Session sess;
    session_init(&sess, &scratch);
    arena_reset(&scratch);
    /* Reopening is for a session someone is about to sit in front of: a
     * one-shot run and a piped one each answer one request, so both start
     * clean whatever the setting says. */
    b8 truncated = false;
    b8 resumed = interactive && cfg.resume_last
                 && resume_latest(&sess, &conv, &persist, &scratch, &truncated);
    if (!resumed) session_begin(&sess);

    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    setvbuf(stdout, NULL, _IONBF, 0);
    g_turn.one_shot = opts.have_prompt;
    ctx_init(&g_ctx);
    ctx_set_window(&g_ctx, cfg.context_window);
    ctx_set_tools(&g_ctx, &tools);
    render_set_verbose(prefs.verbose_tools);
    md_set_raw(prefs.raw_markdown);
    b8 setup = no_provider(&cfg);
    tui_start(cfg.model, cfg.base_url, !cfg.api_key.p, setup, tools.n,
              prefs.show_ignored, prefs.justify, prefs.status_fields, cfg.mode,
              opts.have_prompt);
    tui_set_permissions(cfg.permissions);
    if (cfg.provider.n) tui_set_provider(cfg.provider);
    tui_set_reasoning(cfg.reasoning_effort, cfg.thinking_budget);
    if (resumed) {
        tui_batch_begin();
        render_conv(&conv, &cfg, prefs.show_instructions, &scratch);
        tui_batch_end();
        arena_reset(&scratch);
    } else if (prefs.show_instructions && !opts.have_prompt) {
        render_instructions(&cfg);
    }
    tui_set_commands(g_commands.v, commands_init(cfg.images));
    tui_set_aliases(k_aliases, ALIAS_N);
    tui_set_history(&hist);
    tui_set_interrupt_flag(&g_got_sigint);

    shell_set_idle(on_idle, NULL);
    shell_set_interrupt_flag(&g_got_sigint);
    shell_set_timeout(cfg.shell_timeout_ms);

    atexit(jobs_stop);
    web_set_idle(on_idle, NULL, tui_input_fd(), &g_got_sigint);
    atexit(tui_stop);
    highlight_init(argv[0]);
    atexit(highlight_close);

    telemetry_init(&scratch, prefs.telemetry);
    static TelHead head;
    head.cfg = &cfg;
    head.tools = &tools;
    telemetry_set_header(telemetry_header, &head);
    atexit(telemetry_close);

    Agent agent = {
        .cfg = &cfg,
        .tools = &tools,
        .conv = &conv,
        .persist = &persist,
        .scratch = &scratch,
        .sess = &sess,
        .mark = session_mark,
        .echo = !opts.have_prompt,
        .show_instructions = prefs.show_instructions,
    };
    tui_set_busy_command(on_busy_command, &agent);
    if (!render_verbose()) tui_set_find_expand(find_expand, &agent);
    b8 resumed_saved = !resumed || save_session(&agent);


    if (opts.have_prompt) {
        if (no_provider(&cfg)) {
            Str hint = setup_hint(&cfg, &scratch);
            tui_stop();
            fprintf(stderr,
                    AGENT_NAME ": nothing to talk to; run " AGENT_NAME
                               " without -p, then %.*s\n",
                    (i32)hint.n, hint.p);
            return 1;
        }
        b8 ok = agent_turn(&agent, opts.prompt);
        if (ok) write_final_reply(&conv);
        tui_stop();
        return ok ? 0 : 1;
    }

    /* The welcome screen names the command instead of a form opening unasked
     * over an empty screen. */
    if (tui_is_fullscreen()) tui_set_setup_hint(setup_hint(&cfg, &scratch));
    if (truncated && resumed_saved)
        tui_notice(STR("session truncated: the conversation is full"));


    static char line[AGENT_LINE_BUF];
    for (;;) {
        size_t ln = 0;
        /* Every command that touches the conversation or the model lands
         * back here, so the field is restated once rather than at each of
         * them. */
        ctx_sync(&g_ctx, &conv);
        todo_sync(&conv, &scratch);
        if (!tui_readline("> ", line, sizeof line, &ln)) break;
        if (ln == 0) {
            g_got_sigint = 0;
            continue;
        }

        /* The markers a command was typed behind leave the composer with it,
         * so an attachment reads them from here rather than from the box the
         * submission emptied. */
        char shed[AGENT_MAX_MEDIA_PER_TURN * 16];
        size_t shed_n = 0;
        if (agent.pending_n) {
            size_t skip = pending_prefix(&agent, line, ln);
            if (skip) {
                if (skip <= sizeof shed) {
                    memcpy(shed, line, skip);
                    shed_n = skip;
                }
                memmove(line, line + skip, ln - skip + 1);
                ln -= skip;
            }
        }
        b8 escaped =
            ln >= 2 && line[0] == '\\' && (line[1] == '/' || line[1] == '!');
        if (escaped) {
            memmove(line, line + 1, ln);
            ln--;
            goto send_message;
        } else if (line[0] == '!') {
            run_shell(&agent, (Str){line + 1, ln - 1});
            continue;
        }
        if (!escaped && line[0] == '/') {
            ln = resolve_alias(line, ln, sizeof line);
            telemetry_command((Str){line, ln});
        }
        if (!strcmp(line, "/exit")) break;
        if (!strcmp(line, "/clear")) {
            conv_truncate(&conv, 1);
            cache_guard_begin(&g_cache);
            agent.pending_n = 0;
            persist.off = session_mark;
            arena_reset(&scratch);
            session_begin(&sess);

            tui_batch_begin();
            tui_clear();
            ctx_sync(&g_ctx, &conv);
            tui_batch_end();
            continue;
        }
        if (!strcmp(line, "/mode")) {
            agent_set_mode(&agent,
                           cfg.mode == MODE_PLAN ? MODE_BUILD : MODE_PLAN);
            tui_notice(
                cfg.mode == MODE_PLAN
                    ? STR("plan mode: read-only, and it ends with a plan "
                          "to approve")
                    : STR("build mode: the agent edits files and runs "
                          "commands"));
            continue;
        }
        if (!strcmp(line, "/fork")) {
            fork_session(&agent);
            continue;
        }
        if (!strcmp(line, "/compact")) {
            compact_session(&agent);
            continue;
        }
        if (!strcmp(line, "/find")) {
            tui_find_open();
            continue;
        }
        if (!strcmp(line, "/rewind")) {
            rewind_conversation(&agent);
            continue;
        }
        if (!strcmp(line, "/copy")) {
            copy_last_reply(&conv);
            continue;
        }
        if (!strncmp(line, "/attach", 7) && (ln == 7 || line[7] == ' ')) {
            attach_image(&agent, (Str){line + 7, ln - 7}, (Str){shed, shed_n});
            continue;
        }
        if (!strcmp(line, "/export") || !strncmp(line, "/export ", 8)) {
            export_session(&conv, (Str){line + 7, ln - 7});
            continue;
        }
        if (!strcmp(line, "/title") || !strncmp(line, "/title ", 7)) {
            title_command(&agent, (Str){line + 6, ln - 6});
            continue;
        }
        if (!strcmp(line, "/settings")) {
            choose_settings(&agent);
            continue;
        }
        if (!strcmp(line, "/statusline")) {
            choose_statusline(&scratch);
            continue;
        }
        if (!strcmp(line, "/about")) {
            tui_info(STR("about " AGENT_NAME), k_about, ABOUT_N);
            continue;
        }
        if (!strcmp(line, "/keys")) {
            tui_info(STR("keyboard shortcuts"), g_keys, keys_rows());
            continue;
        }
        if (!strcmp(line, "/todo")) {
            show_todo();
            continue;
        }
        if (!strcmp(line, "/help")) {
            start_help_session(&agent);
            continue;
        }
        if (!strncmp(line, "/expand ", 8)) {
            unsigned long id = strtoul(line + 8, NULL, 10);
            if (id && id <= conv.n) {
                conv.expanded[id - 1] = !conv.expanded[id - 1];
                rerender_conv(&conv, &cfg, agent.show_instructions, &scratch,
                              (u32)id);
            }
            continue;
        }
        if (!strcmp(line, "/model")) {
            choose_model(&cfg, &scratch);
            continue;
        }
        if (!strcmp(line, "/provider")) {
            manage_providers(&cfg, &persist, &scratch);
            continue;
        }
        if (!strcmp(line, "/resume")) {
            resume_session(&agent);
            continue;
        }
        if (!strcmp(line, "/restart")) {
            restart_agent(argv);
            continue;
        }
        if (!escaped && line[0] == '/') {
            notice_fmt("unknown command: %.*s; type / to see commands", (i32)ln,
                       line);
            continue;
        }
    send_message:
        if (no_provider(&cfg)) {
            tui_notice(setup_hint(&cfg, &scratch));
            continue;
        }
        agent_turn_interactive(&agent, (Str){line, ln});
    }

    tui_stop();
    return 0;
}
