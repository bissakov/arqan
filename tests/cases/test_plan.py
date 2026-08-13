"""Plan mode: a read-only agent that ends on a plan the user approves."""

import json

PLAN = "## Steps\n\n1. Read the file\n2. Change the line"


def tool_names(request) -> list[str]:
    return [t["function"]["name"] for t in request.get("tools", [])]


def wait_mode(s, mode: str):
    """The status line names the mode after the model."""
    return s.wait_for(lambda t: s.status_field(2) == mode, f"{mode} mode")


def to_plan(s):
    """Switch the composer into Plan mode and wait for the status line."""
    s.key("shift-tab")
    wait_mode(s, "plan")
    return s


def submit_plan(ctx, plan: str = PLAN) -> str:
    return f"tool=submit_plan:{json.dumps({'plan': plan})}"


def test_shift_tab_switches_mode(ctx):
    """Shift+Tab names the new mode on the status line and in the notice."""
    s = ctx.spawn()
    assert s.status_field(2) == "build", s.status_line()
    to_plan(s)
    s.wait_text("plan mode: read-only")
    s.key("shift-tab")
    wait_mode(s, "build")
    s.wait_text("build mode: the agent edits")


def test_shift_tab_keeps_the_draft(ctx):
    """The mode is not text: the composed line survives the switch."""
    s = ctx.spawn()
    s.type("half a thought").sync()
    to_plan(s)
    assert s.composer_text() == "half a thought", s.composer_lines()


def test_plan_mode_withholds_the_writing_tools(ctx):
    """The provider is offered reads, never bash, write, or patch."""
    ctx.scenario("text=here+is+what+I+see")
    s = ctx.spawn()
    to_plan(s)
    s.submit("how does this work?")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "read" in names, names
    assert "bash" not in names and "write" not in names and "patch" not in names, names
    assert "submit_plan" in names and "ask_user" in names, names


def test_build_mode_withholds_the_plan_tools(ctx):
    """submit_plan and ask_user exist only where a plan is being made."""
    ctx.scenario("text=on+it")
    s = ctx.spawn()
    s.submit("do the work")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "write" in names and "patch" in names, names
    assert "submit_plan" not in names and "ask_user" not in names, names


def test_a_write_in_plan_mode_is_refused(ctx):
    """A tool withheld from the mode is refused even when it is called."""
    args = json.dumps({"path": "out.txt", "content": "nope"})
    ctx.scenario(f"tool=write:{args},final_text=understood")
    s = ctx.spawn()
    to_plan(s)
    s.submit("write a file")
    s.wait_text("understood")
    s.wait_turn_done()

    assert "write is not available in plan mode" in s.text(), s.text()
    assert not (ctx.work / "out.txt").exists(), "plan mode wrote a file"


def test_the_plan_is_rendered_with_its_options(ctx):
    """submit_plan shows the plan as Markdown and asks how to continue."""
    ctx.scenario(submit_plan(ctx))
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the change")
    s.wait_status("continue?")

    text = s.text()
    assert "Steps" in text and "Read the file" in text, text
    assert "{" not in text, text
    assert "Yes, but from a new session" in text, text
    assert "No" in text, text
    ctx.check_screen(s)


def test_an_incomplete_plan_call_is_rejected_before_review(ctx):
    """Malformed plan arguments cannot be approved as an empty handoff."""
    ctx.scenario('tool=submit_plan:{"plan":"unfinished')
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the change")
    s.wait_turn_done()

    assert "continue?" not in s.text(), s.text()
    assert ctx.mock.tool_results() == [
        "ERROR: submit_plan requires a non-empty string plan in complete valid JSON. "
        "Call submit_plan again with the complete plan."
    ], ctx.mock.tool_results()


def test_a_long_markdown_plan_survives_a_new_session_handoff(ctx):
    """The reviewed Markdown is the complete first prompt of the new session."""
    plan = "# Permission model\n\n" + "\n".join(
        f"{i}. Change component {i} without a shortcut" for i in range(1, 301)
    )
    assert len(plan) > 10_000
    ctx.scenario(submit_plan(ctx, plan))
    s = ctx.spawn(rows=40)
    to_plan(s)
    s.submit("plan the change")
    s.wait_status("continue?")
    ctx.scenario("text=started+the+complete+plan")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("started the complete plan")
    s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["system", "user"], messages
    assert messages[1]["content"] == plan, (
        len(messages[1]["content"]),
        len(plan),
        messages[1]["content"][-100:],
    )


def test_yes_switches_to_build_and_continues(ctx):
    """Approving the plan flips the mode and carries the same turn on."""
    ctx.scenario(submit_plan(ctx) + ",final_text=starting+now")
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the change")
    s.wait_status("continue?")
    s.key("enter")
    s.wait_text("starting now")
    s.wait_turn_done()

    assert s.status_field(2) == "build", s.status_line()
    messages = ctx.mock.requests[-1]["messages"]
    assert messages[-1]["role"] == "tool", messages
    assert "approved the plan" in messages[-1]["content"], messages[-1]
    assert "write" in tool_names(ctx.mock.requests[-1]), "build tools are back"


def test_no_ends_the_turn_and_stays_in_plan_mode(ctx):
    """Rejecting the plan hands the composer back with the mode unchanged."""
    ctx.scenario(submit_plan(ctx))
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the change")
    s.wait_status("continue?")
    s.key("down", "down").sync()
    s.key("enter")
    s.wait_turn_done()

    assert s.status_field(2) == "plan", s.status_line()
    assert "rejected the plan" in s.text(), s.text()
    # the rejection reached the model and nothing else was asked of it
    assert len(ctx.mock.requests) == 1, ctx.mock.requests
    s.submit("/mode")  # the composer is live again
    wait_mode(s, "build")


def test_escape_out_of_the_question_is_a_no(ctx):
    """A dismissed handover is not an approval."""
    ctx.scenario(submit_plan(ctx))
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the change")
    s.wait_status("continue?")
    s.key("esc")
    s.wait_turn_done()

    assert s.status_field(2) == "plan", s.status_line()
    assert "rejected the plan" in s.text(), s.text()


def test_a_new_session_starts_from_the_plan_alone(ctx):
    """The second Yes drops the conversation and keeps the plan."""
    ctx.scenario(submit_plan(ctx))
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the change")
    s.wait_status("continue?")
    ctx.scenario("text=working+from+the+plan")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("working from the plan")
    s.wait_turn_done()

    assert s.status_field(2) == "build", s.status_line()
    text = s.text()
    assert "plan the change" not in text, "the old conversation is gone"
    assert "Read the file" in text, text

    messages = ctx.mock.requests[-1]["messages"]
    roles = [m["role"] for m in messages]
    assert roles == ["system", "user"], roles
    assert "Read the file" in messages[1]["content"], messages[1]
    assert "You are a test fixture." in messages[0]["content"], messages[0]


def test_the_new_session_is_a_file_of_its_own(ctx):
    """/resume still finds the planning session the handover left behind."""
    ctx.scenario(submit_plan(ctx))
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the change")
    s.wait_status("continue?")
    ctx.scenario("text=working+from+the+plan")
    s.key("down").sync()
    s.key("enter")
    s.wait_turn_done()

    s.submit("/resume")
    s.wait_status("pick a session")
    assert "plan the change" in s.text(), s.text()


def ask(question: str, options: list[dict]) -> str:
    args = json.dumps({"question": question, "options": options})
    return f"tool=ask_user:{args}"


def test_ask_user_offers_the_options_and_feeds_back_the_choice(ctx):
    """The picker opens on the recommended option and answers the model."""
    ctx.scenario(
        ask(
            "Which storage?",
            [
                {"label": "sqlite", "detail": "one file"},
                {"label": "postgres", "detail": "a server", "recommended": True},
            ],
        )
        + ",final_text=noted"
    )
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the storage")
    s.wait_status("pick an answer")

    text = s.text()
    assert "Which storage?" in text, text
    assert "recommended" in text, text
    assert "+ something else" in text, text
    ctx.check_screen(s)

    s.key("enter")
    s.wait_text("noted")
    s.wait_turn_done()
    assert ctx.mock.tool_results() == ["postgres"], ctx.mock.tool_results()


def test_ask_user_wraps_option_details_to_the_picker_width(ctx):
    """A picker keeps a description visible and reflows it after a resize."""
    detail = (
        "keeps local state beside the project and supports simple portable backups"
    )
    ctx.scenario(ask("Which storage?", [{"label": "sqlite", "detail": detail}]))
    s = ctx.spawn(cols=48, rows=20)
    to_plan(s)
    s.submit("plan the storage")
    s.wait_status("pick an answer")

    lines = s.screen.lines()
    assert any("keeps local state" in line for line in lines), s.text()
    assert any("portable backups" in line for line in lines), s.text()
    assert not any(detail in line for line in lines), "the narrow row should wrap"
    ctx.check_screen(s)

    s.resize(cols=100, rows=20).sync()
    assert any(detail in line for line in s.screen.lines()), s.text()
    assert all(len(line) <= 100 for line in s.screen.lines()), s.text()


def test_ask_user_takes_an_answer_of_its_own(ctx):
    """The last row hands the composer over for something not on the list."""
    ctx.scenario(
        ask("Which storage?", [{"label": "sqlite"}]) + ",final_text=noted"
    )
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the storage")
    s.wait_status("pick an answer")
    s.key("down").sync()
    s.key("enter")
    s.wait_text("your answer")
    s.type("neither, use files")
    s.key("enter")
    s.wait_text("noted")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["neither, use files"], ctx.mock.tool_results()
    assert "neither, use files" in s.text(), s.text()


def test_a_call_beside_the_plan_is_still_answered(ctx):
    """A parallel call gets its result even when the plan ends the turn."""
    ctx.write_file("notes.txt", "on disk\n")
    ctx.scenario(submit_plan(ctx) + ',tool=read:{"path":"notes.txt"}')
    s = ctx.spawn()
    to_plan(s)
    s.submit("plan the change")
    s.wait_status("continue?")
    s.key("down", "down").sync()   # No
    s.key("enter")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == [], "the rejected turn sent nothing"
    ctx.scenario("text=carrying+on")
    s.submit("go on then")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    calls = [m for m in messages if m.get("tool_calls")]
    answered = {m["tool_call_id"] for m in messages if m["role"] == "tool"}
    ids = {c["id"] for m in calls for c in m["tool_calls"]}
    assert ids and ids == answered, (ids, answered)
