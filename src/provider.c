/* provider.c: OpenAI-compatible chat-completions streaming with tool calls.
 *
 * Builds the request JSON from the conversation, POSTs with SSE, and dispatches
 * text deltas and tool-call deltas to the provided sinks. On stream end it
 * appends the assistant message and tool-call messages to the conversation
 * (living in the persistent arena).
 */
#include "yoke.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- conversation SoA ---------------------------------------------------
 * Every append is bounded: the parallel arrays are allocated once at their
 * final capacity, so a full conversation has to be reported to the caller
 * rather than written past.
 */
b8 conv_init(Conv *c, Arena *persist, size_t cap) {
    c->role           = arena_new(persist, MRole, cap);
    c->text           = arena_new(persist, Str,   cap);
    c->tool_name      = arena_new(persist, Str,   cap);
    c->tool_call_id   = arena_new(persist, Str,   cap);
    c->has_tool_call  = arena_new(persist, b8,  cap);
    c->n = 0;
    c->cap = cap;
    if (!c->role || !c->text || !c->tool_name || !c->tool_call_id
        || !c->has_tool_call) {
        c->cap = 0;
        return false;
    }
    return true;
}

size_t conv_room(const Conv *c) { return c->cap - c->n; }

/* Claim one slot, or CONV_NONE when the conversation is full. */
static size_t conv_push(Conv *c, MRole role, Str text, Str id, Str name,
                        b8 has_call) {
    if (c->n >= c->cap) return CONV_NONE;
    size_t i = c->n++;
    c->role[i] = role;
    c->text[i] = text;
    c->tool_call_id[i] = id;
    c->tool_name[i] = name;
    c->has_tool_call[i] = has_call;
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
/* A carrier is the slot that holds one call: an assistant slot flagged with a
 * tool call *and* naming the tool. The head slot names nothing. */
b8 conv_is_call(const Conv *c, size_t i) {
    return i < c->n && c->role[i] == M_ASSISTANT && c->has_tool_call[i]
        && c->tool_name[i].p != NULL;
}

/* Serialize messages to OpenAI chat format. Assistant tool calls are emitted
 * as a single message with a "tool_calls" array; the paired args slot is
 * consumed here and skipped in the main loop. */
void conv_write_json(Buf *b, const Conv *c, const ToolRegistry *reg) {
    (void)reg;
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
        if (c->role[i] == M_TOOL) {
            buf_putf(b, ",\"tool_call_id\":");
            buf_json_str(b, c->tool_call_id[i]);
            buf_putf(b, ",\"content\":");
            buf_json_str(b, c->text[i]);
            buf_putc(b, '}');
            continue;
        }
        if (c->role[i] == M_ASSISTANT && c->has_tool_call[i]) {
            /* head slot: prose + the tool_calls array built from the carrier
             * slots that follow it, each keeping its own id */
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
            i = j - 1; /* skip consumed slots */
            continue;
        }
        buf_putf(b, ",\"content\":");
        buf_json_str(b, c->text[i]);
        buf_putc(b, '}');
    }
    buf_putc(b, ']');
}

/* ---- streaming state (in scratch arena) --------------------------------- */
typedef struct {
    Arena *scratch;
    /* Each SSE event is parsed into its own region and thrown away again: a
     * DOM per delta would otherwise make a turn's scratch use grow with the
     * number of events rather than with the size of the reply. */
    Arena  ev;
    Str  id[YOKE_MAX_TOOL_CALLS];
    Str  name[YOKE_MAX_TOOL_CALLS];
    Buf  args[YOKE_MAX_TOOL_CALLS];
    b8   used[YOKE_MAX_TOOL_CALLS];
    i32  count;
    i32  dropped;      /* calls past the per-turn cap */
    Buf  text;
    b8   text_started;
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

static b8 on_line(Str line, void *ud) {
    Provider *p = (Provider *)ud;
    StreamState *s = p->ud;
    if (line.n >= 6 && !memcmp(line.p, "data:", 5)) {
        Str payload = str_trim(str_drop(line, 5));
        if (str_eq(payload, STR("[DONE]"))) return true;
        arena_reset(&s->ev);
        JVal *ev = json_parse(&s->ev, payload);
        if (!ev) return true;

        /* With stream_options.include_usage, the final event has no choices
         * and carries authoritative token counts for the completed request. */
        const JVal *usage = json_get(ev, STR("usage"));
        if (usage && usage->type == J_OBJ) {
            const JVal *prompt = json_get(usage, STR("prompt_tokens"));
            const JVal *completion = json_get(usage, STR("completion_tokens"));
            const JVal *total = json_get(usage, STR("total_tokens"));
            if (prompt && prompt->type == J_NUM
                && completion && completion->type == J_NUM) {
                p->prompt_tokens = (size_t)prompt->u.n;
                p->completion_tokens = (size_t)completion->u.n;
                p->total_tokens = total && total->type == J_NUM
                                ? (size_t)total->u.n
                                : p->prompt_tokens + p->completion_tokens;
                p->usage_valid = true;
            }
        }

        const JVal *choices = json_get(ev, STR("choices"));
        const JVal *ch0 = json_at(choices, 0);
        if (!ch0) return true;
        const JVal *delta = json_get(ch0, STR("delta"));
        if (delta) {
            const JVal *content = json_get(delta, STR("content"));
            if (content && content->type == J_STR && content->u.s.n) {
                Str text = content->u.s;
                /* Some OpenAI-compatible providers begin assistant content
                 * with a line break.  The editor has already advanced after
                 * submit, so forwarding it creates an unwanted blank line. */
                if (!s->text_started) {
                    size_t skip = 0;
                    while (skip < text.n &&
                           (text.p[skip] == '\r' || text.p[skip] == '\n'))
                        skip++;
                    text = str_drop(text, skip);
                }
                if (text.n) {
                    s->text_started = true;
                    buf_puts(&s->text, text);
                    if (p->on_text) p->on_text(text, p->ud);
                }
            }
            const JVal *tcs = json_get(delta, STR("tool_calls"));
            if (tcs && tcs->type == J_ARR) {
                for (size_t i = 0; i < tcs->u.arr.n; i++) {
                    const JVal *tc = &tcs->u.arr.items[i];
                    const JVal *idxv = json_get(tc, STR("index"));
                    /* A non-numeric "index" would read the union as a double;
                     * treat anything but a number as "the first call". */
                    i32 idx = idxv && idxv->type == J_NUM
                            && idxv->u.n >= 0 && idxv->u.n < (f64)YOKE_MAX_TOOL_CALLS
                            ? (i32)idxv->u.n : 0;
                    i32 sl = slot(s, idx);
                    if (sl < 0) continue;
                    const JVal *idv = json_get(tc, STR("id"));
                    const JVal *fn  = json_get(tc, STR("function"));
                    if (idv && idv->type == J_STR && idv->u.s.n)
                        s->id[sl] = str_dup(s->scratch, idv->u.s);
                    if (fn) {
                        const JVal *nm = json_get(fn, STR("name"));
                        const JVal *ag = json_get(fn, STR("arguments"));
                        if (nm && nm->type == J_STR && nm->u.s.n)
                            s->name[sl] = str_dup(s->scratch, nm->u.s);
                        if (ag && ag->type == J_STR && ag->u.s.n)
                            buf_puts(&s->args[sl], ag->u.s);
                    }
                    if (p->on_tool_call && s->name[sl].p) {
                        Str ad = { s->args[sl].p, s->args[sl].n };
                        p->on_tool_call(sl, s->id[sl], s->name[sl], ad, p->ud);
                    }
                }
            }
        }
    }
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
    /* One event is at most an SSE line, so this is generous by design. */
    enum { EVENT_ARENA_BYTES = 4u << 20 };
    void *ev_mem = arena_alloc(scratch, EVENT_ARENA_BYTES, 16);
    if (!ev_mem) { snprintf(err, err_cap, "out of memory starting a turn"); return -1; }
    arena_init(&s->ev, ev_mem, EVENT_ARENA_BYTES);
    buf_init(&s->text, scratch, 1024);

    void *saved_ud = p->ud;
    p->ud = s;

    Buf body; buf_init(&body, scratch, 4096);
    buf_puts(&body, STR("{\"model\":"));
    buf_json_str(&body, p->cfg->model);
    buf_puts(&body, STR(",\"messages\":"));
    conv_write_json(&body, p->conv, p->tools);
    if (p->tools && p->tools->n) {
        buf_puts(&body, STR(",\"tools\":"));
        tools_write_schemas(&body, p->tools);
    }
    buf_putf(&body, ",\"max_tokens\":%d,\"stream\":true,"
                   "\"stream_options\":{\"include_usage\":true}}",
             p->cfg->max_tokens);
    Str bstr = buf_finish(&body);
    if (!buf_ok(&body)) {
        snprintf(err, err_cap, "request too large for the scratch arena");
        p->ud = saved_ud;
        return -1;
    }

    HttpReq r = {
        .base_url = p->cfg->base_url.p,
        .api_key  = p->cfg->api_key.p,
        .on_line  = on_line,
        .ud       = p,
        .body     = bstr.p,
        .interrupt_flag = p->interrupt_flag,
        .idle_fd  = p->on_idle ? p->idle_fd : -1,
        .on_idle  = p->on_idle,
        .idle_ud  = saved_ud,
    };
    i32 rc = http_sse_post(&r);
    p->ud = saved_ud;
    if (rc != 0) {
        if (rc < 0) snprintf(err, err_cap, "HTTP %d", -rc);
        else snprintf(err, err_cap, "request failed (%d)", rc);
        return -1;
    }

    if (s->dropped)
        yoke_log(YOKE_LOG_WARN, "dropped %d tool call(s) past the per-turn cap of %d",
                 s->dropped, (i32)YOKE_MAX_TOOL_CALLS);

    Str text = buf_finish(&s->text);
    Str text_dup = str_dup(p->persist, text);
    if (text.n && !text_dup.p) {
        snprintf(err, err_cap, "out of memory storing the reply");
        return -1;
    }

    /* Count first: a turn is appended whole or not at all, so a conversation
     * that cannot hold every call never ends up half-written. */
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
