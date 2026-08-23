"""The todo tool: the step list the model keeps for work of several rounds."""

import json


def todo(*items, **kw):
    """Build a todo call from (text, status) pairs."""
    args = json.dumps({"items": [{"text": t, "status": s} for t, s in items]})
    spec = f"tool=todo:{args}"
    final = kw.get("final")
    return spec + f",final_text={final}" if final else spec


def tool_names(request):
    return [t["function"]["name"] for t in request.get("tools", [])]


def test_a_list_is_rendered_as_a_checklist(ctx):
    """The call shows the steps, not the JSON that carried them."""
    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            ("add a regression case", "pending"),
            final="all+set",
        )
    )
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_text("all set")
    s.wait_turn_done()

    text = s.text()
    assert "todo 1/3" in text, text
    assert "wire the parser" in text, text
    assert "{" not in text and "in_progress" not in text, text
    ctx.check_screen(s)


def test_the_result_reports_the_shape_of_the_list(ctx):
    """The model gets a terse summary; the list is already in its own call."""
    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            ("add a regression case", "pending"),
            final="all+set",
        )
    )
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == [
        "3 todos: 1 done, 1 in progress, 1 pending"
    ], ctx.mock.tool_results()


def test_a_finished_list_reports_no_work_left(ctx):
    """A list with nothing pending does not ask for another update."""
    ctx.scenario(todo(("read the decoder", "done"), final="done"))
    s = ctx.spawn()
    s.submit("do the thing")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["1 todo: 1 done"], ctx.mock.tool_results()


def test_an_oversized_list_is_refused_with_its_limit(ctx):
    """A bound is named and the call rejected, never truncated to fit."""
    items = [(f"step number {i}", "pending") for i in range(1, 22)]
    ctx.scenario(todo(*items, final="understood"))
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == [
        "ERROR: 21 items, limit 20: send fewer, larger steps"
    ], ctx.mock.tool_results()


def test_an_oversized_item_is_refused_with_its_limit(ctx):
    """Item text is bounded too, since every call is replayed."""
    ctx.scenario(todo(("x" * 101, "pending"), final="understood"))
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == [
        "ERROR: item 1 is 101 bytes, limit 100"
    ], ctx.mock.tool_results()


def test_two_items_in_progress_are_refused(ctx):
    """One item at a time is the whole point of the list."""
    ctx.scenario(
        todo(
            ("wire the parser", "in_progress"),
            ("add a regression case", "in_progress"),
            final="understood",
        )
    )
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == [
        "ERROR: 2 items are in_progress, work on one"
    ], ctx.mock.tool_results()


def test_an_unknown_status_names_the_allowed_ones(ctx):
    """A refusal tells the model what to send instead."""
    ctx.scenario(todo(("wire the parser", "blocked"), final="understood"))
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == [
        'ERROR: item 1 has status "blocked": use pending, in_progress or done'
    ], ctx.mock.tool_results()


def test_an_incomplete_call_is_refused(ctx):
    """Truncated streamed arguments cannot install half a list."""
    ctx.scenario('tool=todo:{"items":[{"text":"wire the,final_text=understood')
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert results and results[0].startswith("ERROR: "), results


def test_the_tool_is_offered_in_build_and_withheld_from_plan(ctx):
    """Plan mode states the plan; it does not track progress against one."""
    ctx.scenario("text=on+it")
    s = ctx.spawn()
    s.submit("do the work")
    s.wait_turn_done()
    assert "todo" in tool_names(ctx.mock.requests[-1]), tool_names(
        ctx.mock.requests[-1]
    )

    s.key("shift-tab").sync()
    s.submit("plan the work")
    s.wait_turn_done()
    assert "todo" not in tool_names(ctx.mock.requests[-1]), tool_names(
        ctx.mock.requests[-1]
    )


def test_the_prompt_carries_the_list_policy(ctx):
    """The builtin prompt describes the tool only when it is offered."""
    s = ctx.spawn(ARQAN_SYSTEM_PROMPT=None)
    ctx.scenario("text=on+it")
    s.submit("do the work")
    s.wait_turn_done()

    prompt = ctx.mock.requests[-1]["messages"][0]["content"]
    assert "Call todo before starting work" in prompt, prompt


def test_the_status_field_counts_the_list(ctx):
    """Progress is visible between turns, not only where the call landed."""
    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            ("add a regression case", "pending"),
            final="all+set",
        )
    )
    s = ctx.spawn()
    assert "todo" not in s.status_line(), s.status_line()
    s.submit("do the long thing")
    s.wait_turn_done()
    assert "todo 1/3" in s.status_line(), s.status_line()


def test_the_command_shows_the_current_list(ctx):
    """/todo answers from the state, not from the transcript."""
    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            final="all+set",
        )
    )
    s = ctx.spawn()
    s.submit("/todo")
    s.wait_text("no step list yet")
    s.submit("do the long thing")
    s.wait_turn_done()

    s.submit("/todo")
    s.wait_text("step list")
    text = s.text()
    assert "done" in text and "now" in text, text
    assert "wire the parser" in text, text


def test_the_list_survives_a_resumed_session(ctx):
    """The last call in the replayed conversation is the state."""
    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            final="all+set",
        )
    )
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("all set")
    assert "todo 1/2" in s.status_line(), s.status_line()
    s.submit("/todo")
    s.wait_text("step list")
    assert "wire the parser" in s.text(), s.text()


def test_clearing_the_conversation_clears_the_list(ctx):
    """The list is derived from the conversation, so it goes with it."""
    ctx.scenario(todo(("wire the parser", "in_progress"), final="all+set"))
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()
    assert "todo 0/1" in s.status_line(), s.status_line()

    s.submit("/clear")
    s.sync()
    assert "todo" not in s.status_line(), s.status_line()


def test_compaction_carries_the_list_into_the_checkpoint(ctx):
    """The call that held the list is summarized away; the list is not."""
    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            final="on+it",
        )
    )
    s = ctx.spawn()
    s.submit("do the long thing")
    s.wait_turn_done()

    ctx.scenario("text=##+Goal\\nFinish+the+decoder")
    s.submit("/compact")
    s.wait_text("compacted: a new session continues")
    assert "todo 1/2" in s.status_line(), s.status_line()

    ctx.scenario("text=carrying+on")
    s.submit("what next?")
    s.wait_turn_done()
    checkpoint = ctx.mock.requests[-1]["messages"][1]["content"]
    assert "## Step list" in checkpoint, checkpoint
    assert "- [x] read the decoder" in checkpoint, checkpoint
    assert "- [ ] wire the parser (in progress)" in checkpoint, checkpoint
