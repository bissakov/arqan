"""The spinner row under the transcript: what is running and for how long."""

import json
import re

from tests.mockprovider.server import Scenario


def test_spinner_names_the_wait_and_counts_it(ctx):
    """A turn in flight shows a spinner, its label, its seconds and the key
    that ends it."""
    ctx.scenario("hold,text=finally")
    s = ctx.spawn(ARQAN_TEST_FREEZE_ACTIVITY_CLOCK=None)
    s.submit("take your time")
    s.wait_activity("thinking")
    _, elapsed = s.activity()
    assert re.fullmatch(r"\d+s", elapsed), elapsed
    assert "esc or ctrl-c to interrupt" in s.text()
    s.wait_for(lambda t: s.activity()[1] != elapsed, "the clock to advance")
    ctx.mock.release()
    s.wait_turn_done()


def test_frozen_activity_clock_stops_the_spinner_repainting(ctx):
    """A held turn leaves the screen quiet, so settle() never races the
    spinner. A quiet window wider than the 100ms repaint tick can only be
    found when nothing repaints."""
    ctx.scenario("hold,text=finally")
    s = ctx.spawn()
    s.submit("take your time")
    s.wait_activity("thinking")
    assert s.activity() == ("thinking", "0s"), s.activity()
    assert "\u280b thinking" in s.text(), s.text()
    s.settle(quiet=0.25, timeout=3.0)
    assert s.activity() == ("thinking", "0s"), s.activity()
    ctx.mock.release()
    s.wait_turn_done()


def test_status_line_does_not_repeat_the_spinner(ctx):
    """One state, one place: the word lives on the spinner row while a turn
    runs and on the status line when none does."""
    ctx.scenario("hold,text=done")
    s = ctx.spawn()
    s.submit("hi")
    s.wait_activity("thinking")
    line = s.status_line()
    assert "thinking" not in line, line
    assert line.startswith("\u25cf mock-model"), line
    assert s.status_colour() == "thinking", line
    ctx.mock.release()
    s.wait_turn_done()
    assert s.status_line().startswith("\u25cf ready"), s.status_line()


def test_spinner_leaves_when_the_turn_ends(ctx):
    """It is painted, not written: the transcript keeps none of it."""
    ctx.scenario("text=done")
    s = ctx.spawn()
    s.submit("hi")
    s.wait_text("done")
    s.wait_turn_done()
    assert s.activity() is None, s.text()
    assert "to interrupt" not in s.text()
    ctx.check_screen(s)


def test_spinner_leaves_the_composer_where_it_was(ctx):
    """It takes its rows from the transcript, so nothing below it moves."""
    ctx.scenario("hold,text=done")
    s = ctx.spawn()
    s.submit("hi")
    s.wait_activity("thinking")
    composed = [i for i in range(s.term.rows) if "Message arqan..." in s.row(i)]
    assert len(composed) == 1, s.text()
    ctx.mock.release()
    s.wait_turn_done()
    assert [i for i in range(s.term.rows) if "Message arqan..." in s.row(i)] \
        == composed, s.text()


def test_spinner_names_a_running_tool(ctx):
    """A tool call is a long operation of its own and says which one runs."""
    # TODO: placeholder description; write a real one
    ctx.scenario(
        'tool=bash:{"command":"sleep 2; echo slept","description":"qzx"},'
        'final_text=that+took+a+while'
    )
    s = ctx.spawn()
    s.submit("run something slow")
    s.wait_activity("running bash")
    s.wait_text("that took a while")
    s.wait_turn_done()
    assert s.activity() is None


def test_spinner_counts_a_shell_run(ctx):
    """A '!' command runs outside the model and reports the same way."""
    ctx.scenario("text=unused")
    s = ctx.spawn()
    s.submit("!sleep 2; echo local")
    s.wait_activity("running shell")
    s.wait_text("local")
    s.wait_for(lambda t: s.activity() is None, "the spinner to go")


def result_row(s, needle):
    """The '└─' summary row of the block whose result holds `needle`."""
    for i in range(s.term.rows):
        row = s.row(i)
        if row.lstrip().startswith("\u2514\u2500") and needle in row:
            return row.strip()
    raise AssertionError(f"no result row holding {needle!r}\n{s.text()}")


def test_a_tool_run_is_timed_in_the_transcript(ctx):
    """The result line says how long the call took, in the units it needs."""
    ctx.write_file("notes.txt", "written down")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=read+it')
    s = ctx.spawn()
    s.submit("read my notes")
    s.wait_text("read it")
    s.wait_turn_done()
    row = result_row(s, "1 line")
    assert re.search(r"\u00b7 (\d+ms|\d+\.\d+s)$", row), row


def test_a_shell_run_is_timed_in_seconds(ctx):
    """A command over a second reads in seconds rather than in milliseconds."""
    ctx.scenario("text=unused")
    s = ctx.spawn()
    s.submit("!sleep 1.2; echo waited")
    s.wait_text("waited")
    s.wait_for(lambda t: s.activity() is None, "the spinner to go")
    row = result_row(s, "exit 0")
    m = re.search(r"\u00b7 (\d+\.\d)s$", row)
    assert m and float(m.group(1)) >= 1.2, row


def test_the_spinner_outlives_the_run_it_reports_on(ctx):
    """It goes only once the result is on screen: a frame saying idle with
    nothing to show is a run that looks lost."""
    ctx.scenario("text=unused")
    s = ctx.spawn()
    s.submit("!sleep 0.5; echo done here")
    s.wait_activity("running shell")
    s.wait_for(lambda t: s.activity() is None, "the spinner to go")
    assert "exit 0" in s.text(), s.text()


def test_the_time_survives_a_resume(ctx):
    """A replayed transcript says what the live one did: the run is timed in
    the session file, not only on screen."""
    ctx.write_file("notes.txt", "written down")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=read+it')
    s = ctx.spawn()
    s.submit("read my notes")
    s.wait_text("read it")
    s.wait_turn_done()
    live = result_row(s, "1 line")
    s.submit("/exit")
    s.wait_exit()

    s2 = ctx.spawn()
    s2.submit("/resume")
    s2.wait_text("read my notes")
    s2.key("enter")
    s2.wait_text("read it")
    assert result_row(s2, "1 line") == live


def test_the_spinner_carries_the_turn_total_too(ctx):
    """A tool three seconds into a turn says how long it has run and how long
    the turn has."""
    # TODO: placeholder description; write a real one
    ctx.scenario(
        'first_delay=1.5,tool=bash:{"command":"sleep 3; echo slept","description":"qzx"},'
        "final_text=finally"
    )
    s = ctx.spawn(ARQAN_TEST_FREEZE_ACTIVITY_CLOCK=None)
    s.submit("run something slow")
    s.wait_activity("running bash")
    s.wait_text("total")
    row = next(r for r in (s.row(i) for i in range(s.term.rows)) if "total" in r)
    m = re.search(r"running bash \u00b7 (\d+)s \u00b7 (\d+)s total", row)
    assert m, row
    assert int(m.group(2)) > int(m.group(1)), row
    s.wait_turn_done()


def tool_payload(s):
    for i in range(s.term.rows):
        match = re.search(
            r"preparing tool call \u00b7 (\d+(?:\.\d+)?) (B|KiB|MiB|GiB) received",
            s.row(i),
        )
        if match:
            scale = {"B": 1, "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}
            return float(match.group(1)) * scale[match.group(2)]
    return 0


def check_streamed_tool_payload(ctx, api):
    first = json.dumps({"path": "first.txt", "content": "f" * 256})
    second = json.dumps(
        {"path": "second.txt", "content": "x" * 2200 + "\u03a9" * 120},
        ensure_ascii=False,
    )
    expected = len(first.encode()) + len(second.encode())
    ctx.scenario(Scenario(
        tools=[("write", first), ("write", second)],
        delay=0.01, hold_tool_args=True, final_text="written",
    ))
    s = ctx.spawn(cols=132, ARQAN_API=api)
    s.submit("write both files")
    s.wait_activity("preparing tool call")
    s.wait_for(lambda t: tool_payload(s) > 0, "some tool arguments")
    partial = tool_payload(s)
    assert partial < expected, s.text()
    assert not (ctx.work / "first.txt").exists()
    s.wait_for(lambda t: tool_payload(s) > partial, "more tool arguments")
    s.wait_text(f"{expected / 1024:.1f} KiB received")
    assert s.activity() == ("preparing tool call", "0s"), s.activity()
    assert "esc or ctrl-c to interrupt" in s.text(), s.text()
    assert not (ctx.work / "second.txt").exists()
    ctx.mock.release()
    s.wait_turn_done()
    assert "received" not in s.text(), s.text()
    assert (ctx.work / "second.txt").read_text() == "x" * 2200 + "\u03a9" * 120


def test_tool_payload_counts_openai_argument_chunks(ctx):
    check_streamed_tool_payload(ctx, "openai")


def test_tool_payload_counts_anthropic_argument_chunks(ctx):
    check_streamed_tool_payload(ctx, "anthropic")


def test_tool_payload_clears_between_provider_responses(ctx):
    # TODO: placeholder description; write a real one
    args = json.dumps({"command": "sleep 1; echo waited", "description": "qzx"})
    ctx.scenario(Scenario(
        tools=[("bash", args)], tool_rounds=2,
        hold_tool_args=True, final_text="finished",
    ))
    s = ctx.spawn()
    s.submit("run twice")
    s.wait_text(f"{len(args)} B received")
    ctx.mock.release()
    s.wait_activity("running bash")
    assert "received" not in s.text(), s.text()
    ctx.mock.hold()
    s.wait_activity("preparing tool call")
    s.wait_text(f"{len(args)} B received")
    assert tool_payload(s) == len(args), s.text()
    ctx.mock.release()
    s.wait_turn_done()
    assert "received" not in s.text(), s.text()


def test_tool_payload_updates_keep_the_phase_and_total_timers(ctx):
    args = json.dumps({"path": "timed.txt", "content": "x" * 2200})
    ctx.scenario(Scenario(
        tools=[("write", args)], first_delay=1.2, delay=0.025,
        hold_tool_args=True, final_text="finished",
    ))
    s = ctx.spawn(cols=132, ARQAN_TEST_FREEZE_ACTIVITY_CLOCK=None)
    s.submit("write slowly")
    s.wait_activity("preparing tool call")
    s.wait_for(
        lambda t: s.activity()[1] != "0s",
        "the preparing-tool-call clock to advance",
    )
    partial = tool_payload(s)
    s.wait_for(lambda t: tool_payload(s) > partial, "more tool arguments")
    assert s.activity()[1] != "0s", s.activity()
    s.wait_text(f"{len(args) / 1024:.1f} KiB received")
    row = next(s.row(i) for i in range(s.term.rows) if "KiB received" in s.row(i))
    match = re.search(r"received \u00b7 (\d+)s \u00b7 (\d+)s total", row)
    assert match, row
    assert int(match.group(1)) >= 2, row
    assert int(match.group(2)) > int(match.group(1)), row
    ctx.mock.release()
    s.wait_turn_done()
