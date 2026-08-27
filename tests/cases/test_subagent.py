"""The task tool: a read-only subagent that runs in a worker process.

A task call starts a nested agent with its own conversation in a child
process and answers at once with an id. The delegate only reads, searches
and fetches, and the parent goes on working while it runs; a later
task(id=N) collects the written report.

The parent turn and the subagent are told apart by the model each request
names: `subagent_model = small` with a `small_model` of `mock:<scenario>`
gives the delegate a scenario of its own, so one case drives both sides.
"""

import json
import signal
import time

READ_ONLY = ["find", "grep", "internet_search", "page_fetch", "read"]

# Long enough that a loaded machine still finishes the delegate's rounds,
# short enough that a case which never finishes fails rather than hangs.
WAIT = 30000


def a_small_tree(ctx):
    ctx.write_file("src/one.c", "int alpha(void);\n")
    ctx.write_file("notes.txt", "alpha is a letter\n")


def spawn(ctx, sub="text=the+cat+is+in+src/one.c", **env):
    """A session whose small model is the subagent, on its own scenario."""
    env.setdefault("ARQAN_SUBAGENT_MODEL", "small")
    env.setdefault("ARQAN_SMALL_MODEL", f"mock:{sub}")
    # Naming the session would go to the small model too, and its request is
    # not the one under test.
    env.setdefault("ARQAN_AUTO_TITLE", "false")
    return ctx.spawn(**env)


def delegate(ctx, prompt="find the cat", label="cats", **args):
    """The parent turn: one task call, then a final answer."""
    call = dict(args)
    if prompt is not None:
        call["prompt"] = prompt
    if label is not None:
        call["label"] = label
    return f"tool=task:{json.dumps(call)},final_text=done"


def collect(ctx, prompt="find the cat", label="cats", wait_ms=WAIT):
    """Start a task and collect it in the same turn, which is what a caller
    with nothing else to do would write."""
    start = {"prompt": prompt, "label": label}
    poll = {"id": 1, "wait_ms": wait_ms}
    return (f"tool=task:{json.dumps(start)}"
            f",tool=task:{json.dumps(poll)},final_text=done")


def sub_requests(ctx):
    """Requests the subagent made, which are the ones on the small model."""
    return [r for r in ctx.mock.requests
            if str(r.get("model", "")).startswith("mock:")]


def parent_requests(ctx):
    return [r for r in ctx.mock.requests
            if not str(r.get("model", "")).startswith("mock:")]


def tool_names(request):
    tools = request.get("tools") or []
    names = [t.get("function", {}).get("name") or t.get("name") for t in tools]
    return sorted(n for n in names if n)


def parent_results(ctx):
    """Tool results the parent read, which excludes the delegate's own."""
    out = []
    for req in parent_requests(ctx):
        for m in req.get("messages", []):
            if m.get("role") == "tool":
                out.append(m.get("content", ""))
                continue
            content = m.get("content")
            if not isinstance(content, list):
                continue
            for block in content:
                if block.get("type") == "tool_result":
                    out.append(block.get("content", ""))
    return out


def last_result(ctx):
    return parent_results(ctx)[-1]


# ---- the delegation itself ------------------------------------------------


def test_a_task_call_reports_back_as_the_tool_result(ctx):
    """The subagent answers, and its answer is what the parent reads."""
    a_small_tree(ctx)
    ctx.scenario(collect(ctx))
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    assert sub_requests(ctx), [r.get("model") for r in ctx.mock.requests]
    assert "the cat is in src/one.c" in last_result(ctx), last_result(ctx)


def test_the_task_becomes_the_subagents_first_user_message(ctx):
    """The delegate is seeded with its own prompt and the task, and nothing
    of the parent's conversation."""
    ctx.scenario(delegate(ctx, prompt="count the vowels"))
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_turn_done()

    first = sub_requests(ctx)[0]["messages"]
    assert [m["role"] for m in first] == ["system", "user"], first
    assert first[1]["content"] == "count the vowels", first
    assert "delegate it" not in json.dumps(first), first


def test_the_transcript_keeps_one_call_and_one_result(ctx):
    """Nothing of the nested loop reaches the screen: the sub rounds are not
    a transcript, they are one tool call that took a while."""
    a_small_tree(ctx)
    ctx.scenario(collect(ctx))
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},final_text=one+hit')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()
    ctx.check_screen(s)


# ---- the audience ---------------------------------------------------------


def test_the_subagent_is_offered_the_read_only_tools_only(ctx):
    """No bash, no patch, no write, and no task: a delegate cannot change
    the repository, and cannot delegate again."""
    ctx.scenario(delegate(ctx))
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_turn_done()

    names = tool_names(sub_requests(ctx)[0])
    assert names == READ_ONLY, names


def test_the_parent_still_carries_its_own_tools(ctx):
    """The audience narrows the sub request, not the one that made it."""
    ctx.scenario(delegate(ctx))
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_turn_done()

    names = tool_names(parent_requests(ctx)[0])
    assert "task" in names, names
    assert "bash" in names and "write" in names, names


def test_a_disabled_tool_is_gone_from_the_subagent_too(ctx):
    """One registry: what /tools and disable_tools turn off is off for the
    delegate as well, not merely hidden from the parent."""
    ctx.scenario(delegate(ctx))
    s = spawn(ctx, args=["--disable-tools", "grep"])
    s.submit("delegate it")
    s.wait_turn_done()

    names = tool_names(sub_requests(ctx)[0])
    assert "grep" not in names, names
    assert "read" in names, names


def test_plan_mode_keeps_the_task_tool_and_the_same_audience(ctx):
    """Plan mode reads the repository, so delegating a read is still allowed
    and the delegate's own tools do not change."""
    ctx.scenario(delegate(ctx))
    s = spawn(ctx, ARQAN_MODE="plan")
    s.submit("delegate it")
    s.wait_turn_done()

    assert "task" in tool_names(parent_requests(ctx)[0]), \
        tool_names(parent_requests(ctx)[0])
    assert tool_names(sub_requests(ctx)[0]) == READ_ONLY, \
        tool_names(sub_requests(ctx)[0])


def test_the_setting_off_puts_no_task_in_the_schemas(ctx):
    """A session that disabled subagents pays none of the schema bytes."""
    ctx.scenario("text=fine")
    s = ctx.spawn(ARQAN_SUBAGENTS="false")
    s.submit("say something")
    s.wait_turn_done()

    assert "task" not in tool_names(ctx.mock.requests[-1]), \
        tool_names(ctx.mock.requests[-1])


# ---- running in the background --------------------------------------------


def test_a_start_answers_before_the_delegate_has_finished(ctx):
    """The reason this design exists: the call comes back with an id while
    the delegate is still on its first round, so the parent turn is free."""
    a_small_tree(ctx)
    ctx.scenario(delegate(ctx))
    s = spawn(ctx, sub="hold=1,text=too+slow")
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    started = parent_results(ctx)[0]
    assert "Task 1" in started, started
    assert "background" in started, started
    assert "task(id=1)" in started, started
    assert "too slow" not in started, started
    ctx.mock.release()


def test_the_parent_runs_another_tool_while_the_delegate_works(ctx):
    """Delegating no longer blocks the turn: the parent's own tool result
    lands while the delegate's round is still held open."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"find the cat","label":"cats"}'
        ',tool=read:{"path":"notes.txt"},final_text=done'
    )
    s = spawn(ctx, sub="hold=1,text=too+slow")
    s.submit("delegate it")
    s.wait_for(lambda _: len(parent_results(ctx)) >= 2,
               "the parent's own tool result")

    assert "alpha is a letter" in parent_results(ctx)[1], parent_results(ctx)
    ctx.mock.release()
    s.wait_turn_done()


def test_a_poll_before_it_finishes_answers_with_progress(ctx):
    """Progress, not a report: what it has run so far and how to collect."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"find the cat","label":"cats"}'
        ',tool=task:{"id":1},final_text=done'
    )
    s = spawn(ctx, sub="hold=1,text=the+cat+is+in+src/one.c")
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    progress = parent_results(ctx)[1]
    assert "running" in progress, progress
    assert "task(id=1)" in progress, progress
    assert "the cat is in src/one.c" not in progress, progress
    ctx.mock.release()


def test_a_poll_after_it_finishes_carries_the_report_and_the_cost(ctx):
    """The report is the tool result, with one line naming what it cost."""
    a_small_tree(ctx)
    ctx.scenario(collect(ctx))
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=1,'
                       'final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    report = last_result(ctx)
    assert "found it" in report, report
    assert "[task 1" in report, report
    assert "tool call" in report, report


def test_the_delegate_keeps_one_conversation_across_polls(ctx):
    """The worker runs the rounds itself, so nothing is ever re-run: the
    delegate's later requests carry the rounds already behind them."""
    a_small_tree(ctx)
    ctx.scenario(collect(ctx))
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=2,'
                       'final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    subs = sub_requests(ctx)
    assert len(subs) >= 2, [len(r["messages"]) for r in subs]
    first, second = subs[0]["messages"], subs[1]["messages"]
    assert len(first) == 2, first
    assert len(second) > len(first), (len(first), len(second))
    assert second[1]["content"] == first[1]["content"], (first, second)
    assert any(m.get("role") == "tool" for m in second), second


def test_a_new_task_is_refused_while_one_is_running(ctx):
    """One slot, so serializing is the honest contract; the refusal names
    the running id and both ways out."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"one","label":"a"}'
        ',tool=task:{"prompt":"two","label":"b"},final_text=done'
    )
    s = spawn(ctx, sub="hold=1,text=too+slow")
    s.submit("delegate it")
    s.wait_turn_done()

    refusal = parent_results(ctx)[1]
    assert "ERROR" in refusal, refusal
    assert "task(id=1)" in refusal, refusal
    assert 'action="drop"' in refusal, refusal
    ctx.mock.release()


def test_drop_stops_the_worker(ctx):
    """A dropped task makes no further provider request: the child is
    signalled and reaped, not left running against the endpoint."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"find the cat","label":"cats"}'
        ',tool=task:{"id":1,"action":"drop"},final_text=done'
    )
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=8,'
                       'final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    dropped = parent_results(ctx)[1]
    assert "dropped" in dropped, dropped
    before = len(sub_requests(ctx))
    s.settle()
    assert len(sub_requests(ctx)) == before, sub_requests(ctx)


def test_a_dropped_task_cannot_be_polled_again(ctx):
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"find the cat","label":"cats"}'
        ',tool=task:{"id":1,"action":"drop"}'
        ',tool=task:{"id":1},final_text=done'
    )
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=8,'
                       'final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    assert "ERROR" in parent_results(ctx)[2], parent_results(ctx)[2]


def test_killing_the_parent_stops_the_worker(ctx):
    """The worker holds the read end of a pipe its parent keeps open, so a
    parent that dies with no chance to clean up still takes the delegate
    with it rather than leaving it running against the endpoint."""
    a_small_tree(ctx)
    ctx.scenario(delegate(ctx))
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=40,'
                       'first_delay=0.2,final_text=found+it')
    s.submit("delegate it")
    s.wait_for(lambda _: len(sub_requests(ctx)) >= 2, "the delegate at work")
    s.signal(signal.SIGKILL)
    s.wait_exit()

    settled = len(sub_requests(ctx))
    time.sleep(1.5)
    assert len(sub_requests(ctx)) <= settled + 1, sub_requests(ctx)


def test_without_a_worker_the_delegate_runs_in_the_parent(ctx):
    """fork or exec failing is not an error the model should see: the task
    runs in slices inside the turn instead, and still reports."""
    a_small_tree(ctx)
    ctx.scenario(delegate(ctx))
    s = spawn(ctx, ARQAN_TEST_NO_TASK_WORKER="1")
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    assert "the cat is in src/one.c" in last_result(ctx), last_result(ctx)


def test_without_a_worker_a_long_task_still_parks_and_continues(ctx):
    """The fallback keeps the slice contract subagent_slice_ms is for."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"find the cat","label":"cats"}'
        ',tool=task:{"id":1},final_text=done'
    )
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=3,'
                       'final_text=found+it',
              ARQAN_TEST_NO_TASK_WORKER="1",
              ARQAN_SUBAGENT_SLICE_MS="1")
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    parked = parent_results(ctx)[0]
    assert "task 1" in parked.lower(), parked
    assert "task(id=1)" in parked, parked
    subs = sub_requests(ctx)
    assert len(subs) >= 2, [len(r["messages"]) for r in subs]


def test_a_poll_for_a_task_that_is_gone_says_so(ctx):
    """A stale id answers the way a stale job id does, rather than starting
    something the model did not ask for."""
    ctx.scenario('tool=task:{"id":7},final_text=done')
    s = spawn(ctx)
    s.submit("poll it")
    s.wait_turn_done()

    result = last_result(ctx)
    assert "ERROR" in result and "7" in result, result
    assert not sub_requests(ctx), [r.get("model") for r in ctx.mock.requests]


def test_clear_drops_a_running_task(ctx):
    """A task belongs to the conversation that started it, so clearing that
    conversation stops the worker and forgets the id."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"find the cat","label":"cats"},final_text=done'
    )
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=3,'
                       'final_text=found+it')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()
    assert "task(id=1)" in parent_results(ctx)[0], parent_results(ctx)

    s.submit("/clear")
    s.settle()

    ctx.mock.reset()
    ctx.scenario('tool=task:{"id":1},final_text=done')
    s.submit("poll it")
    s.wait_turn_done()

    result = last_result(ctx)
    assert "ERROR" in result, result
    assert not sub_requests(ctx), [r.get("model") for r in ctx.mock.requests]


# ---- refusals and interrupts ----------------------------------------------


def test_an_oversized_prompt_is_refused_rather_than_truncated(ctx):
    """A refused argument is reported, never silently clipped."""
    ctx.scenario(
        'tool=task:{"prompt":"' + "x" * 9000 + '","label":"big"},'
        'final_text=done'
    )
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_turn_done()

    result = last_result(ctx)
    assert "ERROR" in result and "8192" in result, result[:200]
    assert not sub_requests(ctx), [r.get("model") for r in ctx.mock.requests]


def test_a_task_without_a_prompt_is_refused(ctx):
    ctx.scenario('tool=task:{"label":"nothing"},final_text=done')
    s = spawn(ctx)
    s.submit("delegate it")
    s.wait_turn_done()

    assert "ERROR" in last_result(ctx), last_result(ctx)


def test_an_interrupt_answers_the_call_and_ends_the_turn(ctx):
    """Ctrl-C during a wait never leaves an unanswered tool call behind:
    the next request would be refused by the provider."""
    a_small_tree(ctx)
    ctx.scenario(collect(ctx))
    s = spawn(ctx, sub="hold=1,text=too+late")
    s.submit("delegate it")
    s.wait_for(lambda _: bool(sub_requests(ctx)), "the subagent's request")
    s.signal(signal.SIGINT)
    ctx.mock.release()
    s.wait_turn_done()

    s.submit("/clear")
    s.settle()
    ctx.mock.reset()
    ctx.scenario("text=after")
    s.submit("again")
    s.wait_text("after")
    s.wait_turn_done()


def test_an_interrupt_leaves_the_worker_running(ctx):
    """Ctrl-C ends the wait, not the work: the task is still there to
    collect on the next turn."""
    a_small_tree(ctx)
    ctx.scenario(collect(ctx))
    s = spawn(ctx, sub="hold=1,text=the+cat+is+in+src/one.c")
    s.submit("delegate it")
    s.wait_for(lambda _: bool(sub_requests(ctx)), "the subagent's request")
    s.signal(signal.SIGINT)
    s.wait_turn_done()
    ctx.mock.release()

    ctx.scenario('tool=task:{"id":1,"wait_ms":%d},final_text=done' % WAIT)
    s.submit("poll it")
    s.wait_text("done")
    s.wait_turn_done()

    assert "the cat is in src/one.c" in last_result(ctx), last_result(ctx)
