/* provider.c — OpenAI-compatible chat-completions streaming with tool calls.
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

/* ---- conversation SoA --------------------------------------------------- */
void conv_init(Conv *c, Arena *persist, size_t cap) {
    c->role           = arena_new(persist, MRole, cap);
    c->text           = arena_new(persist, Str,   cap);
    c->tool_name      = arena_new(persist, Str,   cap);
    c->tool_call_id   = arena_new(persist, Str,   cap);
    c->has_tool_call  = arena_new(persist, b8,  cap);
    c->n = 0; c->cap = cap;
}
size_t conv_add(Conv *c, MRole role, Str text) {
    size_t i = c->n++;
    c->role[i] = role;
    c->text[i] = text;
    c->tool_name[i] = (Str){0};
    c->tool_call_id[i] = (Str){0};
    c->has_tool_call[i] = false;
    return i;
}
size_t conv_add_assistant_toolcall(Conv *c, Str content, Str id, Str name, Str args) {
    size_t i = c->n++;
    c->role[i] = M_ASSISTANT;
    c->text[i] = content;
    c->tool_call_id[i] = id;
    c->tool_name[i] = name;
    /* stash args in a parallel message slot encoded as a tool_call carrier:
     * we store args into the *next* assistant slot with has_tool_call and the
     * same id; simplest: store args on the same slot's tool_name reuse is
     * ambiguous. Instead we encode args into text of a hidden second slot. */
    c->has_tool_call[i] = true;
    /* second slot carries the args so we can serialize tool_calls properly */
    size_t j = c->n++;
    c->role[j] = M_ASSISTANT;     /* sentinel: has_tool_call + carries args */
    c->text[j] = args;
    c->tool_call_id[j] = id;
    c->tool_name[j] = name;
    c->has_tool_call[j] = true;
    return i;
}
size_t conv_add_tool(Conv *c, Str tool_call_id, Str text) {
    size_t i = c->n++;
    c->role[i] = M_TOOL;
    c->text[i] = text;
    c->tool_call_id[i] = tool_call_id;
    c->tool_name[i] = (Str){0};
    c->has_tool_call[i] = false;
    return i;
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
            /* first slot: prose + tool_calls array (one entry per pair) */
            buf_putf(b, ",\"content\":");
            buf_json_str(b, c->text[i]);
            buf_puts(b, STR(",\"tool_calls\":["));
            /* consume paired args slots that share this message's run */
            size_t j = i + 1;
            i32 first = 1;
            while (j < c->n && c->role[j] == M_ASSISTANT && c->has_tool_call[j]
                   && str_eq(c->tool_call_id[j], c->tool_call_id[i])) {
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
    Str  id[16];
    Str  name[16];
    Buf  args[16];
    b8 used[16];
    i32  count;
    Buf  text;
    b8   text_started;
} StreamState;

static i32 slot(StreamState *s, i32 idx) {
    if (idx < 0 || idx >= 16) return -1;
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
        JVal *ev = json_parse(s->scratch, payload);
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
                    i32 idx = idxv ? (i32)idxv->u.n : 0;
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
    memset(s, 0, sizeof *s);
    s->scratch = scratch;
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

    Str text = buf_finish(&s->text);
    Str text_dup = str_dup(p->persist, text);

    i32 emitted = 0;
    i32 first_tc = -1;
    for (i32 i = 0; i < s->count; i++) {
        if (!s->used[i] || !s->name[i].p) continue;
        Str id = str_dup(p->persist, s->id[i]);
        Str nm = str_dup(p->persist, s->name[i]);
        Str ag = str_dup(p->persist, buf_finish(&s->args[i]));
        if (first_tc < 0) {
            /* create the assistant prose + first tool_call carrier */
            first_tc = (i32)conv_add_assistant_toolcall(p->conv, text_dup, id, nm, ag);
        } else {
            /* additional paired args slot sharing the same tool_call_id group:
             * we just append another assistant toolcall slot with empty prose */
            size_t j = p->conv->n;
            p->conv->role[j] = M_ASSISTANT;
            p->conv->text[j] = (Str){0};
            p->conv->tool_call_id[j] = id;
            p->conv->tool_name[j] = nm;
            p->conv->text[j] = ag;
            p->conv->has_tool_call[j] = true;
            p->conv->n++;
            /* link under the same group id: set its id to the first's id */
            p->conv->tool_call_id[j] = p->conv->tool_call_id[first_tc];
        }
        emitted++;
    }
    if (emitted == 0) {
        conv_add(p->conv, M_ASSISTANT, text_dup);
    }
    return emitted;
}
