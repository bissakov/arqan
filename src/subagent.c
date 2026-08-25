/* A read-only agent nested inside one tool call.
 *
 * The parent asks a question, this runs rounds against the provider with the
 * read-only slice of the tool registry, and one written report comes back as
 * the tool result. Nothing here reaches the screen: the transcript keeps the
 * parent's call and its result, and the context gauge keeps measuring the
 * parent's conversation.
 *
 * The subagent has its own arena because it may outlive the slice that ran
 * it. A run that does not finish inside its budget is parked at a round
 * boundary with its conversation whole, and the next `task` call continues
 * from there, so no round is ever run twice.
 */

#include "agent.h"

#include <stdio.h>
#include <string.h>

void subagent_release(Subagent *s) {
    memset(s, 0, sizeof *s);
}

b8 subagent_begin(Subagent *s, void *mem, size_t cap, u32 id, Str system,
                  Str task, Str label, char *err, size_t err_cap) {
    memset(s, 0, sizeof *s);
    arena_init(&s->a, mem, cap);
    if (!conv_init(&s->conv, &s->a, AGENT_SUB_MESSAGES)) {
        snprintf(err, err_cap, "no room for a subagent conversation");
        return false;
    }
    Str sys = str_dup(&s->a, system);
    Str ask = str_dup(&s->a, task);
    if (!sys.p || !ask.p) {
        snprintf(err, err_cap, "no room for the task");
        return false;
    }
    if (conv_add(&s->conv, M_SYSTEM, sys) == CONV_NONE
        || conv_add(&s->conv, M_USER, ask) == CONV_NONE) {
        snprintf(err, err_cap, "no room for the task");
        return false;
    }
    size_t n = label.n < sizeof s->label - 1 ? label.n : sizeof s->label - 1;
    if (n) memcpy(s->label, label.p, n);
    s->label[n] = '\0';
    s->id = id;
    s->started = agent_now_seconds();
    s->live = true;
    return true;
}

static Str sub_last_reply(const Subagent *s) {
    const Conv *c = &s->conv;
    for (size_t i = c->n; i-- > 0;) {
        if (c->role[i] != M_ASSISTANT || conv_is_call(c, i)) continue;
        Str text = str_trim(c->text[i]);
        if (text.n) return text;
    }
    return (Str){0};
}

static void sub_put_report(Buf *out, Str report) {
    if (report.n <= AGENT_SUB_REPORT_BYTES) {
        buf_puts(out, report);
        return;
    }
    buf_puts(out, str_clip_utf8(report, AGENT_SUB_REPORT_BYTES));
    buf_putf(out,
             "\n\n[the report was %zu bytes and is cut here at %u; ask the "
             "subagent a narrower question to get the rest]",
             report.n, (unsigned)AGENT_SUB_REPORT_BYTES);
}

static void sub_put_cost(Buf *out, const Subagent *s) {
    buf_putf(out,
             "\n\n[task %u: %u round%s, %u tool call%s, %zu prompt and %zu "
             "completion tokens]",
             s->id, s->rounds, s->rounds == 1 ? "" : "s", s->tool_calls,
             s->tool_calls == 1 ? "" : "s", s->prompt_tokens,
             s->completion_tokens);
}

static void sub_put_progress(Buf *out, const Subagent *s) {
    if (s->last_tool.n)
        buf_putf(out, "\nLast tool: %.*s", (i32)s->last_tool.n, s->last_tool.p);
    Str said = sub_last_reply(s);
    if (!said.n) return;
    buf_puts(out, STR("\nWhat it has said so far:\n"));
    buf_puts(out, str_clip_utf8(said, AGENT_SUB_PROGRESS_BYTES));
}

static void sub_put_label(Buf *out, const Subagent *s) {
    if (s->label[0]) buf_putf(out, " (%s)", s->label);
}

/* Run every call the round asked for, appending each result to the sub
 * conversation. False means the conversation or its arena is full, which
 * ends the run with whatever the subagent has already said. */
static b8 sub_run_calls(Subagent *s, const SubRun *r, size_t first) {
    Conv *c = &s->conv;
    size_t last = c->n;
    for (size_t i = first; i < last; i++) {
        if (!conv_is_call(c, i)) continue;
        Str name = c->tool_name[i];
        size_t id = tools_find(r->tools, name);
        s->last_tool = id == TOOL_NONE ? name : r->tools->name[id];
        s->tool_calls++;
        if (r->on_step) r->on_step(s->last_tool, s->rounds, r->ud);

        Buf out;
        buf_init(&out, r->scratch, 4096);
        char err[AGENT_TOOL_ERR] = {0};
        b8 ok = tools_run(r->tools, id, c->text[i], TOOL_AUTH_GRANTED,
                          r->scratch, &out, err, sizeof err, TOOL_FOR_SUB);
        if (!ok) {
            out.n = 0;
            buf_putf(&out, "ERROR: %s", err[0] ? err : "tool failed");
        }
        /* INVARIANT: what the sub Conv keeps has to live in the subagent's
         * own arena. `out` is in the caller's scratch, which the next round
         * resets and the parent reuses the moment this slice returns. */
        Str kept = str_dup(&s->a, buf_finish(&out));
        if (!kept.p) return false;
        if (conv_add_tool(c, c->tool_call_id[i], kept) == CONV_NONE)
            return false;
    }
    return true;
}

SubOutcome subagent_slice(Subagent *s, const SubRun *r, Buf *out, char *err,
                          size_t err_cap) {
    s->slices++;
    u32 ran = 0;
    /* Rolled back to between rounds rather than reset: `out` and whatever
     * else the caller staged for this slice, a small-model Config among
     * them, live below this mark and have to survive every round. */
    size_t mark = r->scratch->off;
    for (;;) {
        if (r->interrupt_flag && *r->interrupt_flag) {
            buf_putf(out, "Task %u", s->id);
            sub_put_label(out, s);
            buf_putf(out,
                     " was interrupted after %u rounds and has been dropped.",
                     s->rounds);
            sub_put_progress(out, s);
            return SUB_INTERRUPTED;
        }
        /* Predictive, and never before a round has run in this slice: a poll
         * that parked without working would answer the parent forever while
         * the task stood still. Once one has run, the question is whether
         * another would finish inside the budget, since it is a round
         * started in time and ended late that strands the parent past the
         * cache window. */
        if (ran && r->deadline_s > 0.0
            && agent_now_seconds() + s->slowest_round_s > r->deadline_s) {
            buf_putf(out, "Task %u", s->id);
            sub_put_label(out, s);
            buf_putf(out,
                     " is not finished. It was parked at a round boundary "
                     "after %u round%s and %u tool call%s, costing %zu prompt "
                     "and %zu completion tokens so far. Its conversation is "
                     "kept whole and nothing will be re-run. Continue it "
                     "with task(id=%u), or abandon it with task(id=%u, "
                     "action=\"drop\"). Keep polling rather than leaving it "
                     "unattended.",
                     s->rounds, s->rounds == 1 ? "" : "s", s->tool_calls,
                     s->tool_calls == 1 ? "" : "s", s->prompt_tokens,
                     s->completion_tokens, s->id, s->id);
            sub_put_progress(out, s);
            return SUB_PARKED;
        }

        r->scratch->off = mark;
        Conv *c = &s->conv;
        size_t before = c->n;
        f64 round_started = agent_now_seconds();
        Provider p = {
            .cfg = r->cfg,
            .tools = r->tools,
            .conv = c,
            .persist = &s->a,
            .scratch = r->scratch,
            .audience = TOOL_FOR_SUB,
            .on_retry = r->on_retry,
            .ud = r->ud,
            .on_idle = r->on_idle,
            .idle_fd = r->idle_fd,
            .interrupt_flag = r->interrupt_flag,
        };
        i32 rc = provider_run(&p, err, err_cap);
        s->rounds++;
        ran++;
        s->prompt_tokens += p.prompt_tokens;
        s->completion_tokens += p.completion_tokens;
        f64 round_s = agent_now_seconds() - round_started;
        if (round_s > s->slowest_round_s) s->slowest_round_s = round_s;

        if (r->interrupt_flag && *r->interrupt_flag) continue;
        if (rc < 0) return SUB_FAILED;
        if (rc == 0) {
            Str report = sub_last_reply(s);
            if (!report.n)
                report = STR("The subagent ended without reporting anything.");
            sub_put_report(out, report);
            sub_put_cost(out, s);
            return SUB_REPORTED;
        }
        if (!sub_run_calls(s, r, before)) {
            buf_putf(out, "Task %u", s->id);
            sub_put_label(out, s);
            buf_putf(out,
                     " ran out of room after %u rounds and could not finish. "
                     "Ask it a narrower question, or investigate this one "
                     "directly.",
                     s->rounds);
            sub_put_progress(out, s);
            return SUB_EXHAUSTED;
        }
    }
}
