/* The event log a task worker writes and its parent reads.
 *
 * One JSON object per line, appended with a single write each. The worker is
 * a separate process, so this is the whole protocol between the two: what
 * the delegate said, what it called, what came back, and one terminal event
 * carrying the report the parent hands to the model.
 *
 * Everything but that terminal event is view material. A line that does not
 * parse, a line longer than the buffer and a log cut short by its cap are all
 * skipped rather than reported: the answer comes from `end`, and the rest is
 * what the task view shows while the work runs.
 */

#include "agent.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const char *const k_task_ev[] = {
    "", "start", "round", "msg", "call", "result", "usage", "end",
};

/* ---- writing ------------------------------------------------------------ */

void tasklog_init(TaskLog *l, i32 fd) {
    memset(l, 0, sizeof *l);
    l->fd = fd;
}

static void ev_str(Buf *b, const char *key, Str v) {
    if (!v.n) return;
    buf_putf(b, ",\"%s\":", key);
    buf_json_str(b, v);
}

static void ev_num(Buf *b, const char *key, u64 v) {
    if (!v) return;
    buf_putf(b, ",\"%s\":%llu", key, (unsigned long long)v);
}

static void ev_bool(Buf *b, const char *key, b8 v) {
    if (v) buf_putf(b, ",\"%s\":true", key);
}

static void ev_body(Buf *b, const TaskEvent *e) {
    switch (e->kind) {
        case TASK_EV_START:
            ev_num(b, "id", e->id);
            ev_str(b, "label", e->label);
            ev_str(b, "model", e->model);
            ev_str(b, "provider", e->provider);
            ev_bool(b, "small", e->small);
            ev_str(b, "task", e->task);
            return;
        case TASK_EV_ROUND: ev_num(b, "n", e->n); return;
        case TASK_EV_MSG:
            ev_bool(b, "assistant", e->assistant);
            ev_str(b, "text", e->text);
            return;
        case TASK_EV_CALL:
            ev_num(b, "slot", e->slot);
            ev_str(b, "name", e->name);
            ev_str(b, "args", e->args);
            return;
        case TASK_EV_RESULT:
            ev_num(b, "slot", e->slot);
            ev_num(b, "ms", e->ms);
            ev_str(b, "text", e->text);
            return;
        case TASK_EV_USAGE:
            ev_num(b, "rounds", e->rounds);
            ev_num(b, "tool_calls", e->tool_calls);
            ev_num(b, "prompt", e->prompt_tokens);
            ev_num(b, "completion", e->completion_tokens);
            ev_str(b, "name", e->name);
            return;
        case TASK_EV_END:
            ev_str(b, "outcome", e->outcome);
            ev_num(b, "rounds", e->rounds);
            ev_num(b, "tool_calls", e->tool_calls);
            ev_num(b, "prompt", e->prompt_tokens);
            ev_num(b, "completion", e->completion_tokens);
            ev_str(b, "text", e->text);
            return;
        case TASK_EV_NONE: return;
    }
}

void tasklog_write(TaskLog *l, const TaskEvent *e, Arena *scratch) {
    if (l->fd < 0 || e->kind <= TASK_EV_NONE || e->kind > TASK_EV_END) return;
    if (l->full && e->kind != TASK_EV_END) return;

    size_t mark = scratch->off;
    Buf b;
    buf_init(&b, scratch, 4096);
    buf_putf(&b, "{\"e\":\"%s\"", k_task_ev[e->kind]);
    ev_body(&b, e);
    buf_puts(&b, STR("}\n"));
    Str line = buf_finish(&b);

    if (buf_ok(&b) && line.n <= AGENT_TASK_LINE_MAX) {
        const char *p = line.p;
        size_t left = line.n;
        while (left) {
            ssize_t w = write(l->fd, p, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                l->full = true;
                break;
            }
            p += w;
            left -= (size_t)w;
            l->written += (size_t)w;
        }
    }
    if (l->written >= AGENT_TASK_LOG_BYTES) l->full = true;
    scratch->off = mark;
}

/* ---- reading ------------------------------------------------------------ */

static u32 ev_get_num(const JVal *j, Str key) {
    const JVal *v = json_get(j, key);
    if (!v || v->type != J_NUM || v->u.n < 0.0 || v->u.n > (f64)UINT32_MAX)
        return 0;
    return (u32)v->u.n;
}

static size_t ev_get_size(const JVal *j, Str key) {
    const JVal *v = json_get(j, key);
    if (!v || v->type != J_NUM || v->u.n < 0.0 || v->u.n > (f64)(1u << 30))
        return 0;
    return (size_t)v->u.n;
}

b8 tasklog_parse(Str line, Arena *scratch, TaskEvent *out) {
    memset(out, 0, sizeof *out);
    if (!line.n || line.p[0] != '{') return false;
    JVal *j = json_parse(scratch, line);
    if (!j || j->type != J_OBJ) return false;
    Str kind = json_str(j, STR("e"));
    if (!kind.n) return false;
    for (size_t i = TASK_EV_START; i <= TASK_EV_END; i++) {
        if (!str_eq(kind, str_c(k_task_ev[i]))) continue;
        out->kind = (TaskEventKind)i;
        break;
    }
    if (out->kind == TASK_EV_NONE) return false;

    out->id = ev_get_num(j, STR("id"));
    out->n = ev_get_num(j, STR("n"));
    out->slot = ev_get_num(j, STR("slot"));
    out->ms = ev_get_num(j, STR("ms"));
    out->rounds = ev_get_num(j, STR("rounds"));
    out->tool_calls = ev_get_num(j, STR("tool_calls"));
    out->prompt_tokens = ev_get_size(j, STR("prompt"));
    out->completion_tokens = ev_get_size(j, STR("completion"));
    out->small = json_bool(j, STR("small"));
    out->assistant = json_bool(j, STR("assistant"));
    out->label = json_str(j, STR("label"));
    out->model = json_str(j, STR("model"));
    out->provider = json_str(j, STR("provider"));
    out->task = json_str(j, STR("task"));
    out->name = json_str(j, STR("name"));
    out->args = json_str(j, STR("args"));
    out->text = json_str(j, STR("text"));
    out->outcome = json_str(j, STR("outcome"));
    return true;
}

void tasklog_reader_init(TaskReader *r, i32 fd) {
    r->fd = fd;
    r->line_n = 0;
    r->overflow = false;
}

static size_t reader_lines(TaskReader *r, Arena *scratch, TaskOnEvent on,
                           void *ud) {
    size_t delivered = 0, at = 0;
    for (;;) {
        const char *nl = memchr(r->line + at, '\n', r->line_n - at);
        if (!nl) break;
        size_t len = (size_t)(nl - (r->line + at));
        if (!r->overflow) {
            size_t mark = scratch->off;
            TaskEvent e;
            if (tasklog_parse((Str){r->line + at, len}, scratch, &e)) {
                on(&e, ud);
                delivered++;
            }
            scratch->off = mark;
        }
        r->overflow = false;
        at += len + 1;
    }
    if (at) {
        r->line_n -= at;
        if (r->line_n) memmove(r->line, r->line + at, r->line_n);
    }
    return delivered;
}

size_t tasklog_read(TaskReader *r, Arena *scratch, TaskOnEvent on, void *ud) {
    if (r->fd < 0 || !on) return 0;
    size_t delivered = 0;
    for (;;) {
        size_t room = sizeof r->line - r->line_n;
        if (!room) {
            /* NOTE: no newline in a full buffer means a line longer than any
             * event can be, so it is dropped and the read picks up at the
             * next one. */
            r->overflow = true;
            r->line_n = 0;
            room = sizeof r->line;
        }
        ssize_t n = read(r->fd, r->line + r->line_n, room);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        r->line_n += (size_t)n;
        delivered += reader_lines(r, scratch, on, ud);
    }
    return delivered;
}
