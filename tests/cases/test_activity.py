"""The spinner row under the transcript: what is running and for how long."""

import re


def test_spinner_names_the_wait_and_counts_it(ctx):
    """A turn in flight shows a spinner, its label, its seconds and the key
    that ends it."""
    ctx.scenario("first_delay=3,text=finally")
    s = ctx.spawn()
    s.submit("take your time")
    s.wait_activity("thinking")
    _, elapsed = s.activity()
    assert re.fullmatch(r"\d+s", elapsed), elapsed
    assert "esc to interrupt" in s.text()
    s.wait_for(lambda t: s.activity()[1] != elapsed, "the clock to advance")
    s.wait_turn_done()


def test_status_line_does_not_repeat_the_spinner(ctx):
    """One state, one place: the word lives on the spinner row while a turn
    runs and on the status line when none does."""
    ctx.scenario("first_delay=3,text=done")
    s = ctx.spawn()
    s.submit("hi")
    s.wait_activity("thinking")
    line = s.status_line()
    assert "thinking" not in line, line
    assert line.startswith("\u25cf mock-model"), line
    assert s.status_colour() == "thinking", line
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
    assert "esc to interrupt" not in s.text()
    ctx.check_screen(s)


def test_spinner_leaves_the_composer_where_it_was(ctx):
    """It takes its rows from the transcript, so nothing below it moves."""
    ctx.scenario("first_delay=3,text=done")
    s = ctx.spawn()
    s.submit("hi")
    s.wait_activity("thinking")
    composed = [i for i in range(s.term.rows) if "Message arqan..." in s.row(i)]
    assert len(composed) == 1, s.text()
    s.wait_turn_done()
    assert [i for i in range(s.term.rows) if "Message arqan..." in s.row(i)] \
        == composed, s.text()


def test_spinner_names_a_running_tool(ctx):
    """A tool call is a long operation of its own and says which one runs."""
    ctx.scenario(
        'tool=bash:{"command":"sleep 2; echo slept"},final_text=that+took+a+while'
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
    ctx.scenario(
        'first_delay=1.5,tool=bash:{"command":"sleep 3; echo slept"},'
        "final_text=finally"
    )
    s = ctx.spawn()
    s.submit("run something slow")
    s.wait_activity("running bash")
    s.wait_text("total")
    row = next(r for r in (s.row(i) for i in range(s.term.rows)) if "total" in r)
    m = re.search(r"running bash \u00b7 (\d+)s \u00b7 (\d+)s total", row)
    assert m, row
    assert int(m.group(2)) > int(m.group(1)), row
    s.wait_turn_done()
