"""Shell mode: a composed line starting with '!' runs locally, not remotely."""

import json
import time
from pathlib import Path

from tests.context import wait_until


def composer_marker(s):
    """The composer's prompt marker and its colour, as painted."""
    row = s.term.rows - 4
    col = s.gutter()
    return s.term.row_text(row)[col], s.term.attr_at(row, col).fg


def wait_for_file(path: Path):
    """Block until the running command has created `path`.

    A run's output only reaches the screen once it is over, so a case that
    has to interrupt a command already under way cannot wait on the
    transcript for it. Waiting on the spinner instead would only say the run
    was about to start: an interrupt that arrives before the fork is answered
    without running the command, which reads differently and is not what
    these cases are about.
    """
    return wait_until(path.exists, f"the run to create {path.name}")


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


def test_escaped_shell_prefix_reaches_the_model(ctx):
    ctx.scenario("text=literal")
    s = ctx.spawn()
    s.submit(r"\!important")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["messages"][-1]["content"] == "!important"
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
    assert "| (_| | | | (_| |" not in text, "the welcome screen is not the answer"

    s.settings_toggle("Display raw")
    assert "\u25c6  shell echo still-here" in s.text(), s.text()


def test_a_run_is_saved_and_resumes(ctx):
    """The run is part of the session, command and output alike."""
    s = ctx.spawn()
    s.submit("!echo saved-output")
    s.wait_text("\u2514\u2500 exit 0")
    s.submit("/exit")
    s.wait_exit()

    root = ctx.home / ".local" / "share" / "arqan" / "sessions"
    files = [f for d in root.iterdir() for f in d.iterdir()]
    assert len(files) == 1, files
    records = files[0].read_text().splitlines()
    assert json.loads(records[0]) == {"type": "session", "title": ""}
    line = records[1]
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


def test_a_run_cannot_write_to_the_terminal(ctx):
    """The child has no controlling terminal, so /dev/tty will not open.

    A command that reaches the terminal directly paints over the frame the
    TUI owns; `sudo` prompting for a password is the usual way to see it.
    """
    s = ctx.spawn()
    s.submit("!echo tty-marker > /dev/tty")
    s.wait_text("\u2514\u2500 exit")
    s.wait_status("ready")

    # the command is echoed into the transcript, so the status is the check:
    # the open failed rather than the write landing on the screen
    text = s.text()
    assert "\u2514\u2500 exit 0" not in text, text
    assert s.composer_text() == "", s.composer_lines()


def test_a_run_cannot_read_the_terminal(ctx):
    """A command reading the terminal fails instead of stealing keystrokes.

    With a controlling terminal the read stops the background child on
    SIGTTIN and the run never finishes; without one the open fails at once.
    """
    s = ctx.spawn()
    s.submit("!read reply < /dev/tty")
    s.wait_text("\u2514\u2500 exit", timeout=5.0)
    s.wait_status("ready")
    assert "\u2514\u2500 exit 0" not in s.text(), s.text()


def test_an_interrupt_ends_the_children_of_a_run(ctx):
    """Ctrl-C kills the run's whole process group, not only its shell."""
    if not Path("/proc/self/cmdline").exists():
        return                                  # not Linux; no process list
    marker = str(ctx.work / "survivor")         # unique to this case
    running = ctx.work / "running"
    s = ctx.spawn()
    s.submit(f"!touch {running}; (sleep 300; touch {marker}) & wait")
    wait_for_file(running)
    s.key("ctrl-c")
    s.wait_text("interrupted")
    s.wait_status("ready")

    # the signal goes to the group, so the backgrounded sleep dies with the
    # shell that started it; reaping is asynchronous, so give it a moment
    deadline = time.monotonic() + 5.0
    while (found := processes_matching(marker)) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert not found, f"a child outlived the interrupt: {found}"


def test_a_spent_interrupt_does_not_end_the_next_run(ctx):
    """The Ctrl-C belonged to the run it stopped, not to the one after it."""
    running = ctx.work / "running"
    s = ctx.spawn()
    s.submit(f"!touch {running}; sleep 300")
    wait_for_file(running)
    s.key("ctrl-c")
    s.wait_text("interrupted")
    s.wait_status("ready")

    s.submit("!echo second-run")
    s.wait_text("\u2514\u2500 exit 0")
    s.wait_status("ready")
    # the first run owns the only interruption on screen
    text = s.text()
    assert text.count("interrupted") == 1, text
    assert "second-run" in text, text
    assert text.count("\u2514\u2500 exit 0") == 1, text


def processes_matching(needle: str) -> list[str]:
    """Command lines of the live processes mentioning `needle`."""
    out = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            cmdline = (entry / "cmdline").read_bytes().decode("utf-8", "replace")
        except OSError:
            continue                            # exited while we looked
        if needle in cmdline:
            out.append(cmdline.replace("\0", " ").strip())
    return out
