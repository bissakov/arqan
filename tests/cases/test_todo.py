"""The todo tool: the step list the model keeps for work of several rounds."""

import json

from .test_budget import windowed
from .test_telemetry import body, events


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


def test_the_count_outranks_the_ambient_fields_on_a_narrow_screen(ctx):
    """Progress through the work survives where the session-long facts do not."""
    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            ("add a regression case", "pending"),
            final="all+set",
        )
    )
    s = ctx.spawn(cols=60, rows=14)
    s.submit("do the long thing")
    s.wait_turn_done()

    line = s.status_line()
    assert "todo 1/3" in line, line
    assert "~/work" not in line, line          # cwd yields the columns first
    fields = [f.strip() for f in line.split("\u00b7")]
    assert fields.index("todo 1/3") < len(fields), line


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


def test_rewinding_past_the_call_drops_the_list(ctx):
    """The call is the state, so trimming it away leaves no list behind."""
    ctx.scenario("text=hello+there")
    s = ctx.spawn()
    s.submit("say hi")
    s.wait_turn_done()

    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            final="all+set",
        )
    )
    s.submit("do the long thing")
    s.wait_turn_done()
    assert "todo 1/2" in s.status_line(), s.status_line()

    s.submit("/rewind")
    s.wait_status("rewind to a message")
    s.key("enter")
    s.wait_gone("wire the parser")
    s.wait_for(
        lambda _t: "todo" not in s.status_line(),
        "the todo field to leave the status line",
    )
    s.key("end", "ctrl-u")            # rewind reloaded the message it dropped
    s.submit("/todo")
    s.wait_text("no step list yet")


def test_the_record_keeps_the_shape_of_the_list_and_none_of_its_text(ctx):
    """Telemetry counts steps and progress; the steps themselves are content."""
    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            ("add a regression case", "pending"),
            final="all+set",
        )
    )
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("do the long thing")
    s.wait_turn_done()

    turn = [e for e in events(ctx) if e["ev"] == "turn_end"][-1]
    assert turn["todo_calls"] == 1, turn
    assert turn["todos"] == 2, turn            # the bucket below three items
    assert turn["todo_done_pct"] == 33, turn

    text = body(ctx)
    for leaked in ("read the decoder", "wire the parser", "regression"):
        assert leaked not in text, text


def test_a_session_without_a_list_records_nothing_of_one(ctx):
    """The fields are absent, not zero, where the tool never ran."""
    ctx.scenario("text=hello+there")
    s = ctx.spawn()
    s.settings_toggle("Telemetry")
    s.submit("say hi")
    s.wait_turn_done()

    turn = [e for e in events(ctx) if e["ev"] == "turn_end"][-1]
    assert "todo_calls" not in turn, turn
    assert "todos" not in turn, turn


def todo_args_on_the_wire(ctx):
    """Every todo call's arguments in the last request, oldest first."""
    return [
        c["function"]["arguments"]
        for m in ctx.mock.requests[-1]["messages"]
        for c in (m.get("tool_calls") or [])
        if c["function"]["name"] == "todo"
    ]


def test_eliding_never_takes_the_live_list_off_the_wire(ctx):
    """The boundary measures age, and the current list is not history.

    A list of a few items passes AGENT_ELIDE_BYTES, so once the elide
    boundary moves past the call that carried it the arguments would be
    stubbed like any other tool's. That leaves the model working without the
    plan it wrote, in exactly the long session the list exists for, while the
    status line still counts it.
    """
    items = [
        {"text": f"step number {i:02d} of the long piece of work",
         "status": "in_progress" if i == 0 else "pending"}
        for i in range(8)
    ]
    args = json.dumps({"items": items})
    assert len(args) > 512, "the case needs a list over AGENT_ELIDE_BYTES"
    bash = json.dumps({"command": "seq 1 300"})

    # The list is written once, then left behind: the boundary reaches back
    # two user turns, so the turns of ordinary work after it are what put the
    # call that carried it under the boundary.
    ctx.scenario(f"tool=todo:{args},final_text=planned")
    s = windowed(ctx, ARQAN_PERMISSIONS="free")
    s.submit("plan the long thing")
    s.wait_text("planned")
    s.wait_turn_done()

    for i in range(3):
        ctx.scenario(f"tool=bash:{bash},tool_rounds=2,text=ok,final_text=step+{i}")
        s.submit(f"carry on {i}")
        s.wait_text(f"step {i}")
        s.wait_turn_done()

    sent = todo_args_on_the_wire(ctx)
    assert sent, "the run made no todo call"
    assert "elided" not in sent[-1], sent[-1]
    assert "step number 00 of the long piece of work" in sent[-1], sent[-1]
    # The work after it did go, or the boundary never moved and the case
    # would pass without testing anything.
    results = [m["content"] for m in ctx.mock.requests[-1]["messages"]
               if m.get("role") == "tool"]
    assert any(r.startswith("[older bash result elided:") for r in results), results


def test_an_update_draws_only_the_steps_that_moved(ctx):
    """A second list prints its changes, not the steps already seen."""
    s = ctx.spawn()
    ctx.scenario(
        todo(
            ("read the decoder", "in_progress"),
            ("wire the parser", "pending"),
            ("add a regression case", "pending"),
            final="planned",
        )
    )
    s.submit("start")
    s.wait_text("planned")
    s.wait_turn_done()

    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("wire the parser", "in_progress"),
            ("add a regression case", "pending"),
            final="moved+on",
        )
    )
    s.submit("next")
    s.wait_text("moved on")
    s.wait_turn_done()

    screen = s.text()
    assert "todo 1/3" in screen, screen
    assert screen.count("read the decoder") == 2, screen
    assert screen.count("wire the parser") == 2, screen
    assert screen.count("add a regression case") == 1, screen


def test_a_restructured_list_is_drawn_whole(ctx):
    """A delta against different items would name a step never shown."""
    s = ctx.spawn()
    ctx.scenario(
        todo(
            ("read the decoder", "in_progress"),
            ("wire the parser", "pending"),
            final="planned",
        )
    )
    s.submit("start")
    s.wait_text("planned")
    s.wait_turn_done()

    ctx.scenario(
        todo(
            ("read the decoder", "done"),
            ("rewrite the lexer", "in_progress"),
            ("wire the parser", "pending"),
            final="regrouped",
        )
    )
    s.submit("next")
    s.wait_text("regrouped")
    s.wait_turn_done()

    screen = s.text()
    assert screen.count("wire the parser") == 2, screen
    assert "rewrite the lexer" in screen, screen


def test_the_call_opens_a_block_of_its_own(ctx):
    """The header starts a block: reasoning or a call above it keeps its air."""
    items = json.dumps(
        {"items": [{"text": "wire the parser", "status": "in_progress"}]}
    )
    ctx.write_file("notes.txt", "result bytes\n")
    ctx.scenario(
        "reasoning=planning+the+work,"
        f"tool=todo:{items},"
        'tool=read:{"path":"notes.txt"},'
        "tool_rounds=2,final_text=all+set"
    )
    s = ctx.spawn(rows=34)
    s.submit("do the long thing")
    s.wait_text("all set")
    s.wait_turn_done()

    lines = s.text().splitlines()
    head = [
        i
        for i, line in enumerate(lines)
        if line.strip().startswith("\u25c6  todo")
    ]
    assert len(head) == 2, s.text()      # after reasoning, then after a block
    for i in head:
        assert lines[i - 1].strip() == "", s.text()
