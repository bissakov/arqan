/* provider.c: chat completions with tool calls, in either wire format.
 *
 * Builds the request JSON from the conversation, POSTs it, and dispatches
 * text and tool-call deltas to the caller's sinks. With Config.stream off the
 * reply is one document instead of a sequence of events and reaches the same
 * sinks in one piece. Either way the assistant message and its tool calls are
 * appended to the conversation at the end.
 *
 * Config.api picks between OpenAI chat completions and the Anthropic messages
 * API. They differ in the shape of a request, of an event and of a reply, and
 * nowhere else: both are read into the same slots and pushed through the same
 * callbacks, so nothing above this file knows which one answered.
 */
#include "yoke.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ---- conversation SoA ---------------------------------------------------
 * The parallel arrays are allocated once at their final capacity, so a full
 * conversation is reported to the caller rather than written past.
 */
b8 conv_init(Conv *c, Arena *persist, size_t cap) {
    c->role           = arena_new(persist, MRole, cap);
    c->text           = arena_new(persist, Str,   cap);
    c->tool_name      = arena_new(persist, Str,   cap);
    c->tool_call_id   = arena_new(persist, Str,   cap);
    c->shell_out      = arena_new(persist, Str,   cap);
    c->has_tool_call  = arena_new(persist, b8,  cap);
    c->expanded       = arena_new(persist, b8,  cap);
    c->ms             = arena_new(persist, u32, cap);
    c->n = 0;
    c->cap = cap;
    if (!c->role || !c->text || !c->tool_name || !c->tool_call_id
        || !c->shell_out || !c->has_tool_call || !c->expanded || !c->ms) {
        c->cap = 0;
        return false;
    }
    return true;
}

size_t conv_room(const Conv *c) { return c->cap - c->n; }

/* CONV_NONE when the conversation is full. */
static size_t conv_push(Conv *c, MRole role, Str text, Str id, Str name,
                        b8 has_call) {
    if (c->n >= c->cap) return CONV_NONE;
    size_t i = c->n++;
    c->role[i] = role;
    c->text[i] = text;
    c->tool_call_id[i] = id;
    c->tool_name[i] = name;
    c->shell_out[i] = (Str){0};
    c->has_tool_call[i] = has_call;
    c->expanded[i] = false;
    c->ms[i] = 0;
    return i;
}

size_t conv_add(Conv *c, MRole role, Str text) {
    return conv_push(c, role, text, (Str){0}, (Str){0}, false);
}
/* Head slot of an assistant turn that calls tools: prose only. The calls
 * themselves follow as carrier slots, each with its own id. */
size_t conv_add_assistant_calls(Conv *c, Str content) {
    return conv_push(c, M_ASSISTANT, content, (Str){0}, (Str){0}, true);
}
size_t conv_add_call(Conv *c, Str id, Str name, Str args) {
    return conv_push(c, M_ASSISTANT, args, id, name, true);
}
size_t conv_add_tool(Conv *c, Str tool_call_id, Str text) {
    return conv_push(c, M_TOOL, text, tool_call_id, (Str){0}, false);
}
size_t conv_add_shell(Conv *c, Str cmd, Str out) {
    size_t i = conv_push(c, M_USER, cmd, (Str){0}, STR("shell"), false);
    if (i != CONV_NONE) c->shell_out[i] = out;
    return i;
}
b8 conv_is_shell(const Conv *c, size_t i) {
    return i < c->n && c->role[i] == M_USER
        && str_eq(c->tool_name[i], STR("shell"));
}
/* A carrier holds one call: an assistant slot flagged with a tool call and
 * naming the tool. The head slot names nothing. */
b8 conv_is_call(const Conv *c, size_t i) {
    return i < c->n && c->role[i] == M_ASSISTANT && c->has_tool_call[i]
        && c->tool_name[i].p != NULL;
}

/* First slot of the last `turns` user turns, or 0 when there are fewer. */
static size_t conv_recent_start(const Conv *c, size_t turns) {
    size_t seen = 0;
    for (size_t i = c->n; i-- > 0;)
        if (c->role[i] == M_USER && ++seen == turns) return i;
    return 0;
}

/* The tool a result answers, for the line that replaces an elided one. */
static Str conv_call_name(const Conv *c, size_t result) {
    for (size_t i = result; i-- > 0;)
        if (conv_is_call(c, i)
            && str_eq(c->tool_call_id[i], c->tool_call_id[result]))
            return c->tool_name[i];
    return STR("tool");
}

/* A tool result is charged again on every later turn, so one older than
 * YOKE_ELIDE_TURNS user turns goes out as a line naming what it was: a file
 * read four turns ago is either reflected in the work or worth reading again.
 * The transcript keeps the text either way. */
static void write_tool_result(Buf *b, const Conv *c, size_t i, size_t recent) {
    if (i < recent && c->text[i].n > YOKE_ELIDE_BYTES) {
        Str name = conv_call_name(c, i);
        buf_puts(b, STR("\"["));
        buf_json_chars(b, name);
        buf_putf(b, " result elided after %u turns: %zu bytes. "
                 "Call it again if you still need it.]\"",
                 (unsigned)YOKE_ELIDE_TURNS, c->text[i].n);
    } else {
        buf_json_str(b, c->text[i]);
    }
}

/* Assistant tool calls are emitted as one message with a "tool_calls" array,
 * the carrier slots consumed here and skipped in the main loop. */
void conv_write_json(Buf *b, const Conv *c, const ToolRegistry *reg) {
    (void)reg;
    size_t recent = conv_recent_start(c, YOKE_ELIDE_TURNS);
    buf_putc(b, '[');
    for (size_t i = 0; i < c->n; i++) {
        if (i) buf_putc(b, ',');
        const char *role = "user";
        switch (c->role[i]) {
            case M_SYSTEM:    role = "system"; break;
            case M_USER:      role = "user"; break;
            case M_ASSISTANT: role = "assistant"; break;
            case M_TOOL:      role = "tool"; break;
        }
        buf_putc(b, '{');
        buf_putf(b, "\"role\":\"%s\"", role);
        if (conv_is_shell(c, i)) {
            /* A '!' run reads on the wire the way it was typed: the command,
             * then what it printed. */
            buf_puts(b, STR(",\"content\":\"!"));
            buf_json_chars(b, c->text[i]);
            buf_json_chars(b, STR("\n"));
            buf_json_chars(b, c->shell_out[i]);
            buf_puts(b, STR("\"}"));
            continue;
        }
        if (c->role[i] == M_TOOL) {
            buf_putf(b, ",\"tool_call_id\":");
            buf_json_str(b, c->tool_call_id[i]);
            buf_putf(b, ",\"content\":");
            write_tool_result(b, c, i, recent);
            buf_putc(b, '}');
            continue;
        }
        if (c->role[i] == M_ASSISTANT && c->has_tool_call[i]) {
            /* Prose, then the tool_calls array built from the carriers. */
            buf_putf(b, ",\"content\":");
            buf_json_str(b, c->text[i]);
            buf_puts(b, STR(",\"tool_calls\":["));
            size_t j = i + 1;
            i32 first = 1;
            while (conv_is_call(c, j)) {
                if (!first) buf_putc(b, ',');
                first = 0;
                buf_putc(b, '{');
                buf_putf(b, "\"id\":");
                buf_json_str(b, c->tool_call_id[j]);
                buf_putf(b, ",\"type\":\"function\",\"function\":{\"name\":");
                buf_json_str(b, c->tool_name[j]);
                buf_putf(b, ",\"arguments\":");
                buf_json_str(b, c->text[j]);
                buf_puts(b, STR("}}"));
                j++;
            }
            buf_putc(b, ']');
            buf_putc(b, '}');
            i = j - 1;   /* the carriers are consumed */
            continue;
        }
        buf_putf(b, ",\"content\":");
        buf_json_str(b, c->text[i]);
        buf_putc(b, '}');
    }
    buf_putc(b, ']');
}

/* ---- the same conversation as Anthropic messages -------------------------
 * Content blocks rather than flat text, a tool result carried by the user
 * turn that answers the call, and consecutive slots of one role merged into a
 * single message, which is the only shape that API accepts.
 */

/* A slot with nothing to say contributes no block, and a message with no
 * blocks is refused rather than read as an empty turn. */
static b8 anth_has_block(const Conv *c, size_t i) {
    if (c->role[i] == M_SYSTEM) return false;
    if (c->role[i] == M_TOOL || conv_is_call(c, i) || conv_is_shell(c, i))
        return true;
    return c->text[i].n > 0;
}

static void anth_write_block(Buf *b, const Conv *c, size_t i, size_t recent) {
    if (conv_is_shell(c, i)) {
        buf_puts(b, STR("{\"type\":\"text\",\"text\":\"!"));
        buf_json_chars(b, c->text[i]);
        buf_json_chars(b, STR("\n"));
        buf_json_chars(b, c->shell_out[i]);
        buf_puts(b, STR("\"}"));
        return;
    }
    if (c->role[i] == M_TOOL) {
        buf_puts(b, STR("{\"type\":\"tool_result\",\"tool_use_id\":"));
        buf_json_str(b, c->tool_call_id[i]);
        buf_puts(b, STR(",\"content\":"));
        write_tool_result(b, c, i, recent);
        buf_putc(b, '}');
        return;
    }
    if (conv_is_call(c, i)) {
        buf_puts(b, STR("{\"type\":\"tool_use\",\"id\":"));
        buf_json_str(b, c->tool_call_id[i]);
        buf_puts(b, STR(",\"name\":"));
        buf_json_str(b, c->tool_name[i]);
        buf_puts(b, STR(",\"input\":"));
        /* The arguments as the model wrote them. A call that carried none is
         * still an object here, since the field is not optional. */
        Str args = str_trim(c->text[i]);
        if (args.n && args.p[0] == '{') buf_put(b, args.p, args.n);
        else buf_puts(b, STR("{}"));
        buf_putc(b, '}');
        return;
    }
    buf_puts(b, STR("{\"type\":\"text\",\"text\":"));
    buf_json_str(b, c->text[i]);
    buf_putc(b, '}');
}

void conv_write_json_anthropic(Buf *b, const Conv *c) {
    size_t recent = conv_recent_start(c, YOKE_ELIDE_TURNS);
    buf_putc(b, '[');
    b8 first_msg = true;
    size_t i = 0;
    while (i < c->n) {
        if (!anth_has_block(c, i)) { i++; continue; }
        b8 assistant = c->role[i] == M_ASSISTANT;
        if (!first_msg) buf_putc(b, ',');
        first_msg = false;
        buf_putf(b, "{\"role\":\"%s\",\"content\":[",
                 assistant ? "assistant" : "user");
        b8 first_block = true;
        for (; i < c->n && c->role[i] != M_SYSTEM
               && (c->role[i] == M_ASSISTANT) == assistant; i++) {
            if (!anth_has_block(c, i)) continue;
            if (!first_block) buf_putc(b, ',');
            first_block = false;
            anth_write_block(b, c, i, recent);
        }
        buf_puts(b, STR("]}"));
    }
    buf_putc(b, ']');
}

/* ---- streaming state (in scratch arena) --------------------------------- */
typedef struct {
    Arena *scratch;
    /* Each SSE event is parsed into its own region and thrown away, so a
     * turn's scratch use follows the size of the reply rather than the number
     * of events. */
    Arena  ev;
    Str  id[YOKE_MAX_TOOL_CALLS];
    Str  name[YOKE_MAX_TOOL_CALLS];
    Buf  args[YOKE_MAX_TOOL_CALLS];
    b8   used[YOKE_MAX_TOOL_CALLS];
    i32  count;
    i32  dropped;      /* calls past the per-turn cap */
    Buf  text;
    b8   text_started;
    b8   reason_started;
    /* Anthropic streams one content block at a time, so the open block is
     * enough to route a delta: the tool slot it fills, or -1 for prose. */
    i32  open_slot;
    i32  blocks;        /* tool_use blocks seen, which is the next slot   */
    size_t events;       /* SSE data lines parsed, for telemetry           */
    size_t bad_events;   /* data lines that were not JSON yoke could read   */
    size_t reason_bytes; /* thinking trace streamed, which Conv never keeps */
} StreamState;

static i32 slot(StreamState *s, i32 idx) {
    if (idx < 0 || idx >= YOKE_MAX_TOOL_CALLS) { s->dropped++; return -1; }
    if (!s->used[idx]) {
        s->used[idx] = true;
        buf_init(&s->args[idx], s->scratch, 256);
        if (idx >= s->count) s->count = idx + 1;
    }
    return idx;
}

/* With stream_options.include_usage the final event has no choices and
 * carries the request's token counts; a non-streamed reply carries the same
 * object at its top level. */
static void read_usage(Provider *p, const JVal *root) {
    const JVal *usage = json_get(root, STR("usage"));
    if (!usage || usage->type != J_OBJ) return;
    const JVal *prompt = json_get(usage, STR("prompt_tokens"));
    const JVal *completion = json_get(usage, STR("completion_tokens"));
    const JVal *total = json_get(usage, STR("total_tokens"));
    if (!prompt || prompt->type != J_NUM
        || !completion || completion->type != J_NUM) return;
    p->prompt_tokens = (size_t)prompt->u.n;
    p->completion_tokens = (size_t)completion->u.n;
    p->total_tokens = total && total->type == J_NUM
                    ? (size_t)total->u.n
                    : p->prompt_tokens + p->completion_tokens;
    p->usage_valid = true;
    /* Fired wherever it is heard, so the caller's context counter is kept
     * current even when the turn is interrupted before it ends. */
    if (p->on_usage) p->on_usage(p->total_tokens, p->ud);
}

/* The composer has already advanced past the submitted line, so a provider
 * that leads with a line break would open the reply on a blank row. */
static Str skip_leading_breaks(Str s, b8 started) {
    size_t skip = 0;
    if (!started)
        while (skip < s.n && (s.p[skip] == '\r' || s.p[skip] == '\n')) skip++;
    return str_drop(s, skip);
}

/* "reasoning_content" is what DeepSeek-style endpoints send, "reasoning" what
 * OpenRouter does; both carry plain text. */
static Str reasoning_of(const JVal *v) {
    Str r = json_str(v, STR("reasoning_content"));
    return r.n ? r : json_str(v, STR("reasoning"));
}

/* The three things a reply carries, taken the same way whether they arrived a
 * delta at a time or whole: a thinking trace the conversation never keeps,
 * the reply itself, and one tool call per slot. */
static void take_reason(Provider *p, StreamState *s, Str raw) {
    Str rt = skip_leading_breaks(raw, s->reason_started);
    if (!rt.n) return;
    s->reason_started = true;
    s->reason_bytes += rt.n;
    if (p->on_reason) p->on_reason(rt, p->ud);
}

static void take_text(Provider *p, StreamState *s, Str raw) {
    Str text = skip_leading_breaks(raw, s->text_started);
    if (!text.n) return;
    s->text_started = true;
    buf_puts(&s->text, text);
    if (p->on_text) p->on_text(text, p->ud);
}

/* Fields are copied out of the event they came in, since a delta's arena is
 * reset before the next one arrives. */
static void take_call(Provider *p, StreamState *s, const JVal *tc, i32 idx) {
    i32 sl = slot(s, idx);
    if (sl < 0) return;
    const JVal *fn = json_get(tc, STR("function"));
    Str id = json_str(tc, STR("id"));
    Str name = json_str(fn, STR("name"));
    Str args = json_str(fn, STR("arguments"));
    if (id.n) s->id[sl] = str_dup(s->scratch, id);
    if (name.n) s->name[sl] = str_dup(s->scratch, name);
    if (args.n) buf_puts(&s->args[sl], args);
    if (p->on_tool_call && s->name[sl].p)
        p->on_tool_call(sl, s->id[sl], s->name[sl],
                        (Str){ s->args[sl].p, s->args[sl].n }, p->ud);
}

static void openai_event(Provider *p, StreamState *s, const JVal *ev) {
    read_usage(p, ev);

    const JVal *choices = json_get(ev, STR("choices"));
    const JVal *ch0 = json_at(choices, 0);
    if (!ch0) return;
    const JVal *delta = json_get(ch0, STR("delta"));
    if (!delta) return;
    take_reason(p, s, reasoning_of(delta));
    take_text(p, s, json_str(delta, STR("content")));
    const JVal *tcs = json_get(delta, STR("tool_calls"));
    if (tcs && tcs->type == J_ARR) {
        for (size_t i = 0; i < tcs->u.arr.n; i++) {
            const JVal *tc = &tcs->u.arr.items[i];
            const JVal *idxv = json_get(tc, STR("index"));
            /* A non-numeric "index" would read the union as a double, so
             * anything but a number is the first call. */
            i32 idx = idxv && idxv->type == J_NUM && idxv->u.n >= 0
                   && idxv->u.n < (f64)YOKE_MAX_TOOL_CALLS
                    ? (i32)idxv->u.n : 0;
            take_call(p, s, tc, idx);
        }
    }
}

/* Anthropic reports the prompt on message_start and the completion on
 * message_delta, so each is kept where it was heard rather than replacing the
 * pair. */
static void read_usage_anth(Provider *p, const JVal *owner) {
    const JVal *usage = json_get(owner, STR("usage"));
    if (!usage || usage->type != J_OBJ) return;
    const JVal *in = json_get(usage, STR("input_tokens"));
    const JVal *out = json_get(usage, STR("output_tokens"));
    if (in && in->type == J_NUM) p->prompt_tokens = (size_t)in->u.n;
    if (out && out->type == J_NUM) p->completion_tokens = (size_t)out->u.n;
    if (!p->prompt_tokens && !p->completion_tokens) return;
    p->total_tokens = p->prompt_tokens + p->completion_tokens;
    p->usage_valid = true;
    if (p->on_usage) p->on_usage(p->total_tokens, p->ud);
}

/* The name and id arrive whole on content_block_start; the arguments follow
 * as partial JSON, the way an OpenAI call's do. */
static void anth_open_tool(Provider *p, StreamState *s, const JVal *blk) {
    i32 sl = slot(s, s->blocks++);
    s->open_slot = sl;
    if (sl < 0) return;
    Str id = json_str(blk, STR("id"));
    Str name = json_str(blk, STR("name"));
    if (id.n) s->id[sl] = str_dup(s->scratch, id);
    if (name.n) s->name[sl] = str_dup(s->scratch, name);
    if (p->on_tool_call && s->name[sl].p)
        p->on_tool_call(sl, s->id[sl], s->name[sl],
                        (Str){ s->args[sl].p, s->args[sl].n }, p->ud);
}

static void anth_event(Provider *p, StreamState *s, const JVal *ev) {
    Str type = json_str(ev, STR("type"));
    if (str_eq(type, STR("message_start"))) {
        read_usage_anth(p, json_get(ev, STR("message")));
        return;
    }
    if (str_eq(type, STR("message_delta"))) {
        read_usage_anth(p, ev);
        return;
    }
    if (str_eq(type, STR("content_block_start"))) {
        const JVal *blk = json_get(ev, STR("content_block"));
        Str kind = json_str(blk, STR("type"));
        s->open_slot = -1;
        if (str_eq(kind, STR("tool_use"))) anth_open_tool(p, s, blk);
        else if (str_eq(kind, STR("thinking")))
            take_reason(p, s, json_str(blk, STR("thinking")));
        else take_text(p, s, json_str(blk, STR("text")));
        return;
    }
    if (str_eq(type, STR("content_block_delta"))) {
        const JVal *d = json_get(ev, STR("delta"));
        Str kind = json_str(d, STR("type"));
        if (str_eq(kind, STR("text_delta"))) {
            take_text(p, s, json_str(d, STR("text")));
        } else if (str_eq(kind, STR("thinking_delta"))) {
            take_reason(p, s, json_str(d, STR("thinking")));
        } else if (str_eq(kind, STR("input_json_delta")) && s->open_slot >= 0) {
            i32 sl = s->open_slot;
            buf_puts(&s->args[sl], json_str(d, STR("partial_json")));
            if (p->on_tool_call && s->name[sl].p)
                p->on_tool_call(sl, s->id[sl], s->name[sl],
                                (Str){ s->args[sl].p, s->args[sl].n }, p->ud);
        }
        return;
    }
    if (str_eq(type, STR("content_block_stop"))) s->open_slot = -1;
}

static b8 on_line(Str line, void *ud) {
    Provider *p = (Provider *)ud;
    StreamState *s = p->ud;
    if (line.n >= 6 && !memcmp(line.p, "data:", 5)) {
        Str payload = str_trim(str_drop(line, 5));
        /* The OpenAI sentinel ends the application stream even when a broken
         * HTTP server leaves its response connection open afterwards. */
        if (str_eq(payload, STR("[DONE]"))) return false;
        s->events++;
        arena_reset(&s->ev);
        JVal *ev = json_parse(&s->ev, payload);
        /* The per-event arena is a fixed slice, so an event larger than it
         * parses into the turn's scratch rather than being dropped. */
        if (!ev) ev = json_parse(s->scratch, payload);
        if (!ev) { s->bad_events++; return true; }
        if (p->cfg->api == API_ANTHROPIC) anth_event(p, s, ev);
        else openai_event(p, s, ev);
    }
    return true;
}

/* One chat.completion document, holding whole what the deltas would have
 * carried a piece at a time. It reaches the same sinks and the same slots, so
 * nothing downstream can tell the two apart. False with `err` filled in when
 * the document is not one. */
static b8 read_completion(Provider *p, StreamState *s, Str raw, Arena *scratch,
                          char *err, size_t err_cap) {
    JVal *doc = json_parse(scratch, raw);
    if (!doc) {
        snprintf(err, err_cap, "the reply is not JSON");
        return false;
    }
    read_usage(p, doc);
    const JVal *msg = json_get(json_at(json_get(doc, STR("choices")), 0),
                               STR("message"));
    if (!msg) {
        snprintf(err, err_cap, "the reply carries no message");
        return false;
    }

    take_reason(p, s, reasoning_of(msg));
    take_text(p, s, json_str(msg, STR("content")));
    const JVal *tcs = json_get(msg, STR("tool_calls"));
    if (!tcs || tcs->type != J_ARR) return true;
    for (size_t i = 0; i < tcs->u.arr.n; i++)
        take_call(p, s, &tcs->u.arr.items[i], (i32)i);
    return true;
}

/* One Anthropic message document: the content array holding whole what the
 * blocks would have streamed. */
static b8 read_message_anth(Provider *p, StreamState *s, Str raw,
                            Arena *scratch, char *err, size_t err_cap) {
    JVal *doc = json_parse(scratch, raw);
    if (!doc) {
        snprintf(err, err_cap, "the reply is not JSON");
        return false;
    }
    read_usage_anth(p, doc);
    const JVal *content = json_get(doc, STR("content"));
    if (!content || content->type != J_ARR) {
        snprintf(err, err_cap, "the reply carries no content");
        return false;
    }
    for (size_t i = 0; i < content->u.arr.n; i++) {
        const JVal *blk = &content->u.arr.items[i];
        Str kind = json_str(blk, STR("type"));
        if (str_eq(kind, STR("text"))) {
            take_text(p, s, json_str(blk, STR("text")));
        } else if (str_eq(kind, STR("thinking"))) {
            take_reason(p, s, json_str(blk, STR("thinking")));
        } else if (str_eq(kind, STR("tool_use"))) {
            anth_open_tool(p, s, blk);
            i32 sl = s->open_slot;
            const JVal *input = json_get(blk, STR("input"));
            /* The arguments are an object here rather than the text a stream
             * accumulates, so they are written back out to reach Conv in the
             * one form a tool is run from. */
            if (sl >= 0 && input) json_write(&s->args[sl], input);
        }
    }
    s->open_slot = -1;
    return true;
}

size_t provider_models(const Config *cfg, Arena *scratch, Str *out, size_t max,
                       char *err, size_t err_cap) {
    if (!out || !max) return 0;
    Buf body; buf_init(&body, scratch, YOKE_MAX_MODEL_BYTES);
    i32 rc = http_get(cfg->base_url.p, "/models", cfg->api_key.p, cfg->api,
                      &body);
    if (rc != 0) {
        if (rc < 0) snprintf(err, err_cap, "models: HTTP %d", -rc);
        else snprintf(err, err_cap, "models: request failed (%d)", rc);
        return 0;
    }
    Str raw = buf_finish(&body);
    if (!buf_ok(&body) || raw.n > YOKE_MAX_MODEL_BYTES) {
        snprintf(err, err_cap, "models: reply too large");
        return 0;
    }
    JVal *doc = json_parse(scratch, raw);
    const JVal *data = doc ? json_get(doc, STR("data")) : NULL;
    if (!data || data->type != J_ARR) {
        snprintf(err, err_cap, "models: unexpected reply");
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < data->u.arr.n && n < max; i++) {
        /* The DOM lives in `scratch` beside the array. */
        Str id = json_str(&data->u.arr.items[i], STR("id"));
        if (id.n) out[n++] = id;
    }
    if (!n) snprintf(err, err_cap, "the provider listed no models");
    return n;
}

/* A transport failure and the statuses a server uses to say "not now" are
 * weather; every other status is an answer about the request itself, and an
 * interrupt is the user. */
static b8 retryable(i32 rc) {
    if (rc == 2) return true;
    switch (-rc) {
        case 408: case 425: case 429:
        case 500: case 502: case 503: case 504: return true;
        default: return false;
    }
}

/* Whether anything reached the screen or the state, which is what makes a
 * second attempt a retry rather than a duplicate: a stream that died after a
 * delta has been painted and cannot be taken back. */
static b8 stream_untouched(const StreamState *s, const Provider *p) {
    return s->events == 0 && s->text.n == 0 && s->reason_bytes == 0
        && s->count == 0 && s->dropped == 0 && !p->usage_valid;
}

/* Doubling from the configured base; attempt 1 waits the base. */
static i32 backoff_ms(i32 base, i32 attempt) {
    i64 ms = base;
    for (i32 i = 1; i < attempt && ms < YOKE_MAX_RETRY_DELAY_MS; i++) ms *= 2;
    return ms > YOKE_MAX_RETRY_DELAY_MS ? YOKE_MAX_RETRY_DELAY_MS : (i32)ms;
}

/* Sliced so the caller's idle hook keeps the composer painting and an
 * interrupt ends the wait at once. False when the turn was interrupted. */
static b8 retry_wait(const Provider *p, i32 delay_ms) {
    enum { SLICE_MS = 25 };
    for (i32 waited = 0; waited < delay_ms; waited += SLICE_MS) {
        if (p->interrupt_flag && *p->interrupt_flag) return false;
        if (p->on_idle) p->on_idle(p->ud);
        i32 slice = delay_ms - waited < SLICE_MS ? delay_ms - waited : SLICE_MS;
        struct timespec ts = { 0, (long)slice * 1000000L };
        nanosleep(&ts, NULL);
    }
    return !(p->interrupt_flag && *p->interrupt_flag);
}

/* The system prompt is a message to one API and a parameter to the other, so
 * this is where the two requests part. */
static b8 template_owned(Str key, const Provider *p) {
    if (str_eq(key, STR("model")) || str_eq(key, STR("messages"))
        || str_eq(key, STR("system")) || str_eq(key, STR("tools"))
        || str_eq(key, STR("max_tokens")) || str_eq(key, STR("stream"))
        || str_eq(key, STR("stream_options"))) return true;
    if (str_eq(key, STR("reasoning_effort")) && p->cfg->api == API_OPENAI
        && p->cfg->reasoning_effort.n) return true;
    return str_eq(key, STR("thinking")) && p->cfg->api == API_ANTHROPIC
        && p->cfg->thinking_budget.n;
}

static b8 write_template_value(Buf *b, const JVal *v, const Config *c,
                               char *err, size_t err_cap) {
    if (v->type == J_STR && str_eq(v->u.s, STR("$reasoning_effort"))) {
        if (!c->reasoning_effort.n) { snprintf(err, err_cap, "reasoning template references an effort that is Off"); return false; }
        buf_json_str(b, c->reasoning_effort); return true;
    }
    if (v->type == J_STR && str_eq(v->u.s, STR("$thinking_budget"))) {
        b8 ok = false; i64 n = str_int(c->thinking_budget, &ok);
        if (!c->thinking_budget.n || !ok || n <= 0) { snprintf(err, err_cap, "reasoning template references a budget that is Off"); return false; }
        buf_putf(b, "%lld", (long long)n); return true;
    }
    if (v->type == J_ARR) {
        buf_putc(b, '[');
        for (size_t i = 0; i < v->u.arr.n; i++) { if (i) buf_putc(b, ','); if (!write_template_value(b, &v->u.arr.items[i], c, err, err_cap)) return false; }
        buf_putc(b, ']'); return true;
    }
    if (v->type == J_OBJ) {
        buf_putc(b, '{'); size_t n = 0;
        for (const JVal *m = v->u.obj.head; m; m = m->next) {
            if (n++) buf_putc(b, ',');
            buf_json_str(b, m->key);
            buf_putc(b, ':');
            if (!write_template_value(b, m, c, err, err_cap)) return false;
        }
        buf_putc(b, '}'); return true;
    }
    json_write(b, v); return true;
}

static b8 build_request(Buf *b, const Provider *p, char *err, size_t err_cap) {
    b8 anth = p->cfg->api == API_ANTHROPIC;
    buf_puts(b, STR("{\"model\":"));
    buf_json_str(b, p->cfg->model);
    if (anth) {
        const Conv *c = p->conv;
        for (size_t i = 0; i < c->n; i++) {
            if (c->role[i] != M_SYSTEM) continue;
            if (c->text[i].n) {
                buf_puts(b, STR(",\"system\":"));
                buf_json_str(b, c->text[i]);
            }
            break;
        }
    }
    buf_puts(b, STR(",\"messages\":"));
    if (anth) conv_write_json_anthropic(b, p->conv);
    else conv_write_json(b, p->conv, p->tools);
    if (p->tools && p->tools->n) {
        buf_puts(b, STR(",\"tools\":"));
        tools_write_schemas(b, p->tools, p->cfg->api);
    }
    buf_putf(b, ",\"max_tokens\":%d", p->cfg->max_tokens);
    if (!anth && p->cfg->reasoning_effort.n) {
        buf_puts(b, STR(",\"reasoning_effort\":"));
        buf_json_str(b, p->cfg->reasoning_effort);
    }
    if (anth && p->cfg->thinking_budget.n)
        buf_putf(b, ",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":%.*s}",
                 (i32)p->cfg->thinking_budget.n, p->cfg->thinking_budget.p);
    if (p->cfg->reasoning_template.n) {
        JVal *root = json_parse(p->scratch, p->cfg->reasoning_template);
        if (!root || root->type != J_OBJ) { snprintf(err, err_cap, "reasoning template must be a JSON object"); return false; }
        for (const JVal *m = root->u.obj.head; m; m = m->next) {
            if (template_owned(m->key, p)) { snprintf(err, err_cap, "reasoning template conflicts with request field %.*s", (i32)m->key.n, m->key.p); return false; }
            for (const JVal *other = root->u.obj.head; other != m; other = other->next)
                if (str_eq(m->key, other->key)) { snprintf(err, err_cap, "reasoning template has duplicate field %.*s", (i32)m->key.n, m->key.p); return false; }
            buf_putc(b, ','); buf_json_str(b, m->key); buf_putc(b, ':');
            if (!write_template_value(b, m, p->cfg, err, err_cap)) return false;
        }
    }
    if (anth)
        buf_puts(b, p->cfg->stream ? STR(",\"stream\":true}")
                                   : STR(",\"stream\":false}"));
    else
        buf_puts(b, p->cfg->stream
                 ? STR(",\"stream\":true,\"stream_options\":{\"include_usage\":true}}")
                 : STR(",\"stream\":false}"));
    return true;
}

i32 provider_run(Provider *p, char *err, size_t err_cap) {
    Arena *scratch = p->scratch;
    arena_reset(scratch);
    p->prompt_tokens = 0;
    p->completion_tokens = 0;
    p->total_tokens = 0;
    p->usage_valid = false;

    StreamState *s = arena_new(scratch, StreamState, 1);
    if (!s) { snprintf(err, err_cap, "out of memory starting a turn"); return -1; }
    memset(s, 0, sizeof *s);
    s->scratch = scratch;
    s->open_slot = -1;
    /* One event is at most an SSE line, so this is generous. */
    enum { EVENT_ARENA_BYTES = 4u << 20 };
    void *ev_mem = arena_alloc(scratch, EVENT_ARENA_BYTES, 16);
    if (!ev_mem) { snprintf(err, err_cap, "out of memory starting a turn"); return -1; }
    arena_init(&s->ev, ev_mem, EVENT_ARENA_BYTES);
    buf_init(&s->text, scratch, 1024);

    void *saved_ud = p->ud;
    p->ud = s;

    Buf body; buf_init(&body, scratch, 4096);
    if (!build_request(&body, p, err, err_cap)) {
        p->ud = saved_ud;
        return -1;
    }
    Str bstr = buf_finish(&body);
    if (!buf_ok(&body)) {
        snprintf(err, err_cap, "request too large for the scratch arena");
        p->ud = saved_ud;
        return -1;
    }

    /* A whole reply has no length to go by until it has arrived, so it grows
     * into the scratch arena like every other buffer here. */
    Buf whole;
    if (!p->cfg->stream) buf_init(&whole, scratch, 1u << 16);
    HttpReq r = {
        .base_url = p->cfg->base_url.p,
        .api_key  = p->cfg->api_key.p,
        .api      = p->cfg->api,
        .on_line  = on_line,
        .ud       = p,
        .line_arena = scratch,
        .body_out = p->cfg->stream ? NULL : &whole,
        .body     = bstr.p,
        .interrupt_flag = p->interrupt_flag,
        .idle_fd  = p->on_idle ? p->idle_fd : -1,
        .on_idle  = p->on_idle,
        .idle_ud  = saved_ud,
        .fail_out = NULL,
        .fail_cap = 0,
    };
    char fail[128] = {0};
    r.fail_out = fail;
    r.fail_cap = sizeof fail;

    f64 started = yoke_now_seconds();
    i32 attempts = p->cfg->retries > 0 ? p->cfg->retries + 1 : 1;
    i32 attempt = 1;
    i32 rc;
    for (;;) {
        fail[0] = '\0';
        rc = http_post(&r);
        if (rc == 0 || attempt >= attempts || !retryable(rc)) break;
        if (!stream_untouched(s, p)) break;

        i32 delay = backoff_ms(p->cfg->retry_delay_ms, attempt);
        char reason[160];
        if (rc < 0) snprintf(reason, sizeof reason, "HTTP %d", -rc);
        else snprintf(reason, sizeof reason, "%s",
                      fail[0] ? fail : "the connection failed");
        TelEvent re;
        tel_open(&re, "retry");
        tel_int(&re, "attempt", attempt);
        tel_int(&re, "attempts", attempts);
        tel_int(&re, "delay_ms", delay);
        tel_int(&re, "rc", rc);
        tel_send(&re);
        if (p->on_retry)
            p->on_retry(attempt, attempts, delay, str_c(reason), saved_ud);
        if (!retry_wait(p, delay)) { rc = 3; break; }

        /* Nothing arrived, so only what a refused request left behind is
         * cleared. */
        s->events = 0;
        s->bad_events = 0;
        s->text.n = 0;
        s->text.oom = false;
        s->text_started = false;
        s->reason_started = false;
        s->open_slot = -1;
        s->blocks = 0;
        if (!p->cfg->stream) { whole.n = 0; whole.oom = false; }
        attempt++;
    }
    char parse_err[128] = {0};
    /* Read before the record below, so the cost is reported whether or not
     * the document made sense. */
    if (rc == 0 && !p->cfg->stream) {
        p->ud = s;
        if (!buf_ok(&whole))
            snprintf(parse_err, sizeof parse_err, "the reply is too large");
        else if (p->cfg->api == API_ANTHROPIC)
            read_message_anth(p, s, buf_finish(&whole), scratch, parse_err,
                              sizeof parse_err);
        else
            read_completion(p, s, buf_finish(&whole), scratch, parse_err,
                            sizeof parse_err);
    }
    p->ud = saved_ud;

    /* The request's size rather than its text, and what the stream cost,
     * reasoning bytes included. */
    TelEvent tev;
    tel_open(&tev, "request");
    tel_int(&tev, "messages", (i64)p->conv->n);
    tel_int(&tev, "body_bytes", (i64)bstr.n);
    tel_int(&tev, "tools", (i64)(p->tools ? p->tools->n : 0));
    tel_int(&tev, "ms", (i64)((yoke_now_seconds() - started) * 1000.0));
    tel_int(&tev, "rc", rc);
    tel_int(&tev, "attempts", attempt);
    tel_str(&tev, "api", api_name(p->cfg->api));
    tel_bool(&tev, "stream", p->cfg->stream);
    tel_int(&tev, "sse_events", (i64)s->events);
    tel_int(&tev, "bad_events", (i64)s->bad_events);
    tel_shape(&tev, "reply", (Str){ s->text.p, s->text.n });
    tel_int(&tev, "reason_bytes", (i64)s->reason_bytes);
    tel_int(&tev, "dropped_calls", s->dropped);
    if (p->usage_valid) {
        tel_int(&tev, "prompt_tokens", (i64)p->prompt_tokens);
        tel_int(&tev, "completion_tokens", (i64)p->completion_tokens);
        tel_int(&tev, "total_tokens", (i64)p->total_tokens);
    }
    tel_send(&tev);

    if (parse_err[0]) {
        snprintf(err, err_cap, "%s", parse_err);
        return -1;
    }
    if (rc != 0) {
        if (rc < 0) snprintf(err, err_cap, "HTTP %d", -rc);
        else if (fail[0]) snprintf(err, err_cap, "%s", fail);
        else snprintf(err, err_cap, "request failed (%d)", rc);
        return -1;
    }

    if (s->dropped)
        yoke_log(YOKE_LOG_WARN, "dropped %d tool call(s) past the per-turn cap of %d",
                 s->dropped, (i32)YOKE_MAX_TOOL_CALLS);
    /* A turn that said nothing because every event was unreadable is an error
     * rather than an empty reply, which would otherwise reach the transcript
     * as silence. */
    if (s->bad_events && !s->text.n && !s->count && !s->reason_bytes) {
        snprintf(err, err_cap, "the provider sent %zu event(s) yoke could not "
                 "read", s->bad_events);
        return -1;
    }

    Str text = buf_finish(&s->text);
    Str text_dup = str_dup(p->persist, text);
    if (text.n && !text_dup.p) {
        snprintf(err, err_cap, "out of memory storing the reply");
        return -1;
    }

    /* Count first: a turn is appended whole or not at all. */
    i32 calls = 0;
    for (i32 i = 0; i < s->count; i++)
        if (s->used[i] && s->name[i].p) calls++;

    size_t needed = calls ? (size_t)calls + 1 : 1;
    if (conv_room(p->conv) < needed) {
        snprintf(err, err_cap, "conversation is full (%zu messages)", p->conv->cap);
        return -1;
    }

    if (calls == 0) {
        conv_add(p->conv, M_ASSISTANT, text_dup);
        return 0;
    }

    conv_add_assistant_calls(p->conv, text_dup);
    i32 emitted = 0;
    for (i32 i = 0; i < s->count; i++) {
        if (!s->used[i] || !s->name[i].p) continue;
        Str id = str_dup(p->persist, s->id[i]);
        Str nm = str_dup(p->persist, s->name[i]);
        Str ag = str_dup(p->persist, buf_finish(&s->args[i]));
        if (!nm.p || (s->id[i].n && !id.p) || (s->args[i].n && !ag.p)) {
            snprintf(err, err_cap, "out of memory storing a tool call");
            return -1;
        }
        conv_add_call(p->conv, id, nm, ag);
        emitted++;
    }
    return emitted;
}
