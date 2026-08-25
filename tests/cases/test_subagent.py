"""The task tool: a read-only subagent that runs in slices.

A task call starts a nested agent with its own conversation and hands one
written report back as the tool result. The delegate only reads, searches and
fetches, and a long investigation is parked at a slice boundary rather than
held open past the provider's prompt cache.

The parent turn and the subagent are told apart by the model each request
names: `subagent_model = small` with a `small_model` of `mock:<scenario>`
gives the delegate a scenario of its own, so one case drives both sides.
"""

import json
import signal

READ_ONLY = ["find", "grep", "internet_search", "page_fetch", "read"]


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
    ctx.scenario(delegate(ctx))
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
    ctx.scenario(delegate(ctx))
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


# ---- slices ---------------------------------------------------------------


def test_an_unfinished_task_is_parked_and_continues_where_it_stopped(ctx):
    """The regression this design exists for.

    With a one-millisecond slice the delegate parks after its first round.
    The result names the id to poll, and the poll's request carries the
    rounds already run rather than starting from the task again: a parked
    subagent is kept whole, and nothing is ever re-run.
    """
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"find the cat","label":"cats"}'
        ',tool=task:{"id":1},final_text=done'
    )
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=3,'
                       'final_text=found+it',
              ARQAN_SUBAGENT_SLICE_MS="1")
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()

    results = parent_results(ctx)
    parked = results[0]
    assert "task 1" in parked.lower(), parked
    assert 'task(id=1)' in parked, parked
    assert 'action="drop"' in parked, parked

    subs = sub_requests(ctx)
    assert len(subs) >= 2, [len(r["messages"]) for r in subs]
    first, second = subs[0]["messages"], subs[1]["messages"]
    assert len(first) == 2, first
    # The poll resumes the same conversation: the seeded task is still its
    # first user message, and the round already run is in front of it.
    assert len(second) > len(first), (len(first), len(second))
    assert second[1]["content"] == first[1]["content"], (first, second)
    assert any(m.get("role") == "tool" for m in second), second


def test_a_new_task_is_refused_while_one_is_parked(ctx):
    """One slot, so serializing is the honest contract; the refusal names
    the parked id and both ways out."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"one","label":"a"}'
        ',tool=task:{"prompt":"two","label":"b"},final_text=done'
    )
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=3,'
                       'final_text=found+it',
              ARQAN_SUBAGENT_SLICE_MS="1")
    s.submit("delegate it")
    s.wait_turn_done()

    refusal = parent_results(ctx)[1]
    assert "ERROR" in refusal, refusal
    assert "task(id=1)" in refusal, refusal


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


def test_clear_drops_a_parked_task(ctx):
    """A parked task belongs to the conversation that started it."""
    a_small_tree(ctx)
    ctx.scenario(
        'tool=task:{"prompt":"find the cat","label":"cats"},final_text=done'
    )
    s = spawn(ctx, sub='tool=grep:{"pattern":"alpha"},tool_rounds=3,'
                       'final_text=found+it',
              ARQAN_SUBAGENT_SLICE_MS="1")
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
    """Ctrl-C during a slice never leaves an unanswered tool call behind:
    the next request would be refused by the provider."""
    a_small_tree(ctx)
    ctx.scenario(delegate(ctx))
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
