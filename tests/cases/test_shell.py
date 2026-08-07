"""Shell mode: a composed line starting with '!' runs locally, not remotely."""


def composer_marker(s):
    """The composer's prompt marker and its colour, as painted."""
    row = s.term.rows - 4
    col = s.gutter()
    return s.term.row_text(row)[col], s.term.attr_at(row, col).fg


def test_marker_switches_to_shell(ctx):
    """A leading '!' turns the blue prompt into a red one, and back."""
    s = ctx.spawn()
    assert composer_marker(s) == ("\u203a", 81)      # S_CYAN

    s.type("!").sync()
    assert composer_marker(s) == ("!", 203)          # S_RED
    assert "Run a shell command..." in s.composer_lines()[0]

    s.type("echo hi").sync()
    # the '!' is the marker, not part of the text
    assert s.composer_text() == "echo hi", s.composer_lines()
    ctx.check_screen(s)

    s.key("home", "delete").sync()
    assert composer_marker(s) == ("\u203a", 81)
    assert s.composer_text() == "echo hi", s.composer_lines()


def test_command_and_output_reach_the_transcript(ctx):
    """The command runs and both it and its output are written to the view."""
    s = ctx.spawn()
    s.submit("!echo hello-from-shell")
    s.wait_text("\u2514\u2500 exit 0")
    s.wait_status("ready")

    text = s.text()
    assert "\u25c6  shell echo hello-from-shell" in text, text
    assert "\u2514\u2500 exit 0" in text, text
    ctx.check_screen(s)

    # Nothing was asked of the model: the command is the user's, not a turn.
    assert ctx.mock.requests == [], ctx.mock.requests


def test_failing_command_reports_its_status(ctx):
    """A nonzero exit is the result's summary, output and all."""
    s = ctx.spawn()
    s.submit("!echo oops >&2; exit 3")
    s.wait_text("\u2514\u2500 exit 3")
    s.wait_status("ready")
    # stderr is captured, not painted over the frame it would otherwise corrupt
    assert "   oops" in s.text(), s.text()


def test_empty_command_answers_in_the_notice_row(ctx):
    """'!' alone runs nothing and never touches the transcript."""
    s = ctx.spawn()
    s.submit("!   ")
    s.wait_text("no command to run")
    assert "\u25c6  shell" not in s.text(), s.text()


def test_a_run_survives_a_rerender(ctx):
    """A replay of the transcript is a replay of the run: a setting keeps it."""
    s = ctx.spawn()
    s.submit("!echo still-here")
    s.wait_text("\u2514\u2500 exit 0")

    s.settings_toggle("Verbose tool output")
    text = s.text()
    assert "\u25c6  shell echo still-here" in text, text
    assert "still-here" in text, text
    assert "| |_| | (_) |" not in text, "the welcome screen is not the answer"

    s.settings_toggle("Raw Markdown")
    assert "\u25c6  shell echo still-here" in s.text(), s.text()


def test_a_run_is_saved_and_resumes(ctx):
    """The run is part of the session, command and output alike."""
    s = ctx.spawn()
    s.submit("!echo saved-output")
    s.wait_text("\u2514\u2500 exit 0")
    s.submit("/exit")
    s.wait_exit()

    root = ctx.home / ".local" / "share" / "yoke" / "sessions"
    files = [f for d in root.iterdir() for f in d.iterdir()]
    assert len(files) == 1, files
    line = files[0].read_text().splitlines()[0]
    assert '"name":"shell"' in line and '"content":"echo saved-output"' in line
    assert "saved-output\\n\\n[exit 0]" in line, line

    s2 = ctx.spawn()
    s2.submit("/resume")
    s2.wait_status("pick a session")
    s2.key("enter")
    s2.wait_text("\u25c6  shell echo saved-output")
    assert "saved-output" in s2.text(), s2.text()


def test_a_run_reaches_the_next_request(ctx):
    """The model is told what the user ran and what it printed."""
    ctx.scenario("text=noted")
    s = ctx.spawn()
    s.submit("!echo context-line")
    s.wait_text("\u2514\u2500 exit 0")
    s.submit("what did that print?")
    s.wait_text("noted")
    s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    roles = [m["role"] for m in messages]
    assert roles == ["system", "user", "user"], roles
    assert messages[1]["content"] == "!echo context-line\ncontext-line\n\n[exit 0]"
