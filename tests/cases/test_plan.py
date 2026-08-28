"""Plan mode: a read-only agent that ends on a plan the user approves."""

import json
import time

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


def test_build_mode_offers_questions_but_withholds_plan_submission(ctx):
    """Build can ask for a decision but cannot submit a plan for approval."""
    ctx.scenario("text=on+it")
    s = ctx.spawn()
    s.submit("do the work")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "write" in names and "patch" in names, names
    assert "ask_user" in names and "submit_plan" not in names, names


def test_build_mode_asks_and_continues_the_same_turn(ctx):
    """A build agent receives the decision as a tool result and carries on."""
    args = json.dumps({
        "question": "Which database should I configure?",
        "options": [
            {"label": "sqlite", "recommended": True},
            {"label": "postgres"},
        ],
    })
    ctx.scenario(f"tool=ask_user:{args},final_text=configuring+it+now")
    s = ctx.spawn()
    s.submit("set up the application")
    s.wait_status("pick an answer")
    s.key("enter")
    s.wait_text("configuring it now")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["sqlite"], ctx.mock.tool_results()
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_build_mode_refuses_an_unavailable_plan_submission(ctx):
    """Special UI tools still obey the registry mode when called unasked."""
    ctx.scenario(submit_plan(ctx) + ",final_text=understood")
    s = ctx.spawn()
    s.submit("do the work")
    s.wait_text("understood")
    s.wait_turn_done()

    assert "continue?" not in s.text(), s.text()
    assert ctx.mock.tool_results() == [
        "ERROR: submit_plan is not available in build mode"
    ], ctx.mock.tool_results()


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


def answer_rows(s, answer):
    """The result row holding `answer` and the rows under it."""
    rows = s.screen.lines()
    for i, row in enumerate(rows):
        if row.lstrip().startswith("\u2514\u2500") and answer in row:
            return [r.strip() for r in rows[i:]]
    raise AssertionError(f"no result row holding {answer!r}\n{s.text()}")


def test_ask_user_keeps_the_detail_of_the_answer(ctx):
    """The answer alone is a label, so the transcript keeps what it meant."""
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
    s.key("up").sync()
    s.key("enter")
    s.wait_text("noted")
    s.wait_turn_done()

    rows = answer_rows(s, "sqlite")
    assert rows[1] == "one file", rows[:3]
    assert ctx.mock.tool_results() == ["sqlite"], ctx.mock.tool_results()
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn()
    again.submit("/resume")
    again.wait_text("plan the storage")
    again.key("enter")
    again.wait_text("noted")
    assert answer_rows(again, "sqlite")[1] == "one file", again.text()


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


def test_ask_user_wraps_option_values_to_the_picker_width(ctx):
    """A value too wide for its column continues instead of clipping."""
    label = "use the local embedded database with project scoped storage"
    ctx.scenario(ask("Which storage?", [{"label": label, "detail": "one file"}]))
    s = ctx.spawn(cols=48, rows=20)
    to_plan(s)
    s.submit("plan the storage")
    s.wait_status("pick an answer")

    lines = s.screen.lines()
    assert any("use the local" in line for line in lines), s.text()
    assert any("project scoped" in line for line in lines), s.text()
    assert any("storage" in line for line in lines), s.text()
    assert not any(label in line for line in lines), "the narrow value should wrap"


def test_ask_user_keeps_the_question_above_a_tall_picker(ctx):
    """The question stays in an overlay when the options cover its transcript."""
    question = "Which deployment target should this plan use?"
    options = [
        {"label": f"target-{i}", "detail": f"deployment option {i}"}
        for i in range(12)
    ]
    ctx.scenario(ask(question, options))
    s = ctx.spawn(cols=80, rows=16)
    to_plan(s)
    s.submit("plan the deployment")
    s.wait_status("pick an answer")

    rows = s.screen.lines()
    question_rows = [i for i, row in enumerate(rows) if question in row]
    assert len(question_rows) == 1, "\n".join(rows)
    row = question_rows[0]
    assert s.screen.attr_at(row, 2).fg == 221, "the question is a notice"
    assert row < next(i for i, text in enumerate(rows) if "target-" in text), rows


def test_ask_user_grows_the_picker_on_a_tall_terminal(ctx):
    """A screen with room shows the options at once instead of scrolling."""
    options = [
        {"label": f"target-{i:02d}", "detail": f"deployment option {i}"}
        for i in range(12)
    ]
    ctx.scenario(ask("Which deployment target?", options))
    s = ctx.spawn(cols=80, rows=40)
    to_plan(s)
    s.submit("plan the deployment")
    s.wait_status("pick an answer")

    rows = s.screen.lines()
    shown = [i for i in range(12) if any(f"target-{i:02d}" in r for r in rows)]
    assert len(shown) == 12, "\n".join(rows)
    assert any("+ something else" in r for r in rows), "\n".join(rows)


def test_ask_user_wraps_a_long_question_over_several_rows(ctx):
    """A question too wide for one row keeps its tail instead of losing it."""
    question = (
        "Should the migration keep the existing table names, "
        "or rename every one of them to the new scheme "
        "before the first deployment runs?"
    )
    options = [
        {"label": f"target-{i}", "detail": f"deployment option {i}"}
        for i in range(12)
    ]
    ctx.scenario(ask(question, options))
    s = ctx.spawn(cols=80, rows=16)
    to_plan(s)
    s.submit("plan the migration")
    s.wait_status("pick an answer")

    rows = s.screen.lines()
    head = next(i for i, row in enumerate(rows) if "Should the migration" in row)
    tail = next(i for i, row in enumerate(rows) if "first deployment runs?" in row)
    assert tail > head, "\n".join(rows)
    assert s.screen.attr_at(tail, 4).fg == 221, "the whole question is a notice"
    assert head < next(i for i, r in enumerate(rows) if "target-" in r), rows


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


def test_ask_user_answers_itself_when_nobody_answers(ctx):
    """An unattended question takes the recommendation instead of waiting."""
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
    s = ctx.spawn(ARQAN_ASK_TIMEOUT_MS=1000)
    to_plan(s)
    s.submit("plan the storage")
    s.wait_status("pick an answer")
    assert "no answer in 1s picks the recommended option" in s.text(), s.text()

    s.wait_text("noted")
    s.wait_turn_done()
    results = ctx.mock.tool_results()
    assert len(results) == 1, results
    assert results[0].startswith("postgres"), results[0]
    assert "recommended option was taken automatically" in results[0], results[0]


def test_ask_user_without_a_recommendation_waits(ctx):
    """With nothing to fall back on the question keeps the keyboard."""
    ctx.scenario(
        ask("Which storage?", [{"label": "sqlite"}]) + ",final_text=noted"
    )
    s = ctx.spawn(ARQAN_ASK_TIMEOUT_MS=500)
    to_plan(s)
    s.submit("plan the storage")
    s.wait_status("pick an answer")
    assert "no answer in" not in s.text(), s.text()

    time.sleep(1.5)
    s.settle()
    assert s.status_kind() == "pick an answer", s.status_line()

    s.key("enter")
    s.wait_text("noted")
    s.wait_turn_done()
    assert ctx.mock.tool_results() == ["sqlite"], ctx.mock.tool_results()


def test_a_key_restarts_the_ask_user_wait(ctx):
    """A reader who is there is not hurried: browsing resets the deadline."""
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
    s = ctx.spawn(ARQAN_ASK_TIMEOUT_MS=1500)
    to_plan(s)
    s.submit("plan the storage")
    s.wait_status("pick an answer")

    for _ in range(3):
        time.sleep(0.5)
        s.key("up").sync()
        s.key("down").sync()
    assert s.status_kind() == "pick an answer", s.status_line()

    s.key("enter")
    s.wait_text("noted")
    s.wait_turn_done()
    assert ctx.mock.tool_results() == ["postgres"], ctx.mock.tool_results()


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


def report_turn(ctx, s, lines: int = 20):
    """A first turn whose prose fills the screen, so its tail can be covered."""
    ctx.scenario("text=" + "\\n".join(f"report line {i}" for i in range(lines)))
    s.submit("look around")
    s.wait_turn_done()


def test_ask_user_lifts_the_transcript_out_from_under_the_picker(ctx):
    """The question block stays readable: a picker may not cover its subject."""
    s = ctx.spawn(cols=80, rows=24)
    to_plan(s)
    report_turn(ctx, s)
    ctx.scenario(
        ask(
            "Which storage?",
            [
                {"label": "sqlite", "detail": "one file"},
                {"label": "postgres", "detail": "a server"},
            ],
        )
        + ",final_text=noted"
    )
    s.submit("plan the storage")
    s.wait_status("pick an answer")

    rows = s.screen.lines()
    asked = [i for i, row in enumerate(rows) if "Which storage?" in row]
    assert len(asked) == 2, "\n".join(rows)   # the ask block and the notice
    block, notice = asked
    assert "\u2502" in rows[block], rows[block]
    assert "ask" in rows[block - 1], rows[block - 1]
    assert notice < next(i for i, row in enumerate(rows) if "sqlite" in row)
    assert any("report line 19" in row for row in rows[:block]), "\n".join(rows)

    s.key("enter")
    s.wait_text("noted")
    s.wait_turn_done()
    text = s.text()
    assert "report line 19" in text and "sqlite" in text, text


def test_a_command_picker_leaves_the_transcript_where_it_was(ctx):
    """Only a screen asking about a block lifts it: /model covers as before."""
    s = ctx.spawn(cols=80, rows=24)
    to_plan(s)
    report_turn(ctx, s)
    assert any("report line 19" in row for row in s.screen.lines())

    s.submit("/model")
    s.wait_status("pick a model")
    rows = s.screen.lines()
    assert not any("report line 19" in row for row in rows), "\n".join(rows)
    assert any("report line 17" in row for row in rows), "\n".join(rows)

    s.key("esc")
    s.wait_status("ready")
    assert any("report line 19" in row for row in s.screen.lines())
