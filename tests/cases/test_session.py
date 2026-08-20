"""Session-level behaviour: slash commands, exit paths and terminal restore."""

import json
import signal


def test_a_failed_session_save_is_reported_and_retried(ctx):
    """A transient data-directory failure leaves every message pending."""
    blocked = ctx.home / "blocked-data"
    blocked.write_text("not a directory\n")

    ctx.scenario("text=first+reply")
    s = ctx.spawn(XDG_DATA_HOME=str(blocked))
    s.submit("first prompt")
    s.wait_text("session was not saved")
    s.wait_turn_done()

    blocked.unlink()
    blocked.mkdir()
    ctx.scenario("text=second+reply")
    s.submit("second prompt")
    s.wait_text("session saving recovered")
    s.wait_turn_done()

    files = sorted(blocked.rglob("*.jsonl"))
    assert len(files) == 1, files
    lines = [json.loads(line) for line in files[0].read_text().splitlines()]
    assert [(line["role"], line["content"]) for line in lines] == [
        ("user", "first prompt"),
        ("assistant", "first reply"),
        ("user", "second prompt"),
        ("assistant", "second reply"),
    ], lines


def test_exit_command_quits_cleanly(ctx):
    """/exit ends the process with status 0."""
    s = ctx.spawn()
    s.submit("/exit")
    assert s.wait_exit() == 0


def test_exit_restores_the_terminal(ctx):
    """Leaving puts back the primary screen, the cursor and the mouse modes."""
    s = ctx.spawn()
    s.submit("/exit")
    s.wait_exit()
    t = s.screen
    assert not t.alt_active, "should be back on the primary screen"
    assert t.cursor_visible, "cursor must be visible again"
    assert t.modes.get(1003) is False, "mouse tracking must be released"
    assert t.modes.get(1006) is False
    assert t.modes.get(7) is True, "autowrap must be restored"


def test_ctrl_d_on_empty_composer_exits(ctx):
    """Ctrl-D at an empty prompt is end-of-input."""
    s = ctx.spawn()
    s.key("ctrl-d")
    assert s.wait_exit() == 0
    assert not s.screen.alt_active


def test_clear_command_clears_the_transcript(ctx):
    """/clear wipes the visible history, and the token counter falls back to
    what a request carries with no conversation in it: the system prompt and
    the tool schemas are still sent."""
    ctx.scenario("text=remember+this,usage=500/10")
    s = ctx.spawn()
    s.submit("first message")
    s.wait_text("remember this")
    s.wait_turn_done()
    counted = s.status_field(5)
    assert counted.startswith("~"), f"the reply is estimated: {counted}"
    before = int(counted.lstrip("~"))
    assert before >= 500, s.status_line()

    s.submit("/clear")
    s.wait_for(lambda t: "remember this" not in t.text(), "transcript to clear")
    assert "first message" not in s.text()
    emptied = s.status_field(5)
    assert emptied.startswith("~"), f"nothing has measured it: {emptied}"
    assert int(emptied.lstrip("~")) <= before, s.status_line()
    assert s.PLACEHOLDER in s.text()
    ctx.check_screen(s)


def test_clear_restores_the_welcome_screen(ctx):
    """An emptied transcript is a fresh start, welcome art included."""
    ctx.scenario("text=hi")
    s = ctx.spawn()
    s.submit("hello")
    s.wait_turn_done()
    assert "| (_| | | | (_| |" not in s.text()

    s.submit("/clear")
    s.wait_text("| (_| | | | (_| |")


def test_clear_command_resets_the_conversation(ctx):
    """After /clear the provider sees a fresh conversation, system prompt kept."""
    ctx.scenario("text=one")
    s = ctx.spawn()
    s.submit("first")
    s.wait_text("one")
    s.wait_turn_done()

    s.submit("/clear")
    s.wait_for(lambda t: "one" not in t.text(), "transcript to clear")

    ctx.scenario("text=two")
    s.submit("second")
    s.wait_text("two")
    s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    roles = [m["role"] for m in messages]
    assert roles == ["system", "user"], roles
    assert messages[0]["content"] == "You are a test fixture."
    assert messages[1]["content"] == "second"


def test_sigwinch_during_a_turn_is_handled(ctx):
    """A resize mid-stream repaints without disturbing the running turn."""
    ctx.scenario("words=40,chunk=2,delay=0.05")
    s = ctx.spawn()
    s.submit("keep talking")
    s.wait_turn_underway()
    s.resize(64, 20)
    s.wait_turn_done()
    assert s.screen.cols == 64
    assert s.status_kind() == "ready"


def test_sigint_outside_a_turn_clears_the_composer(ctx):
    """SIGINT at the prompt abandons the line rather than quitting."""
    s = ctx.spawn()
    s.type("half typed").sync()
    s.signal(signal.SIGINT)
    s.wait_for(lambda t: s.composer_text() == "", "the composer to clear")
    assert "^C" not in s.text(), "nothing is written to the transcript"
    assert s.proc.poll() is None, "SIGINT must not kill the session"

    # the cancellation must not leak into the turn that follows it
    ctx.scenario("text=next+turn+runs")
    s.submit("and now this")
    s.wait_text("next turn runs")
    s.wait_turn_done()
    assert "[interrupted]" not in s.text()


def test_unknown_slash_command_is_rejected_locally(ctx):
    """A reserved slash typo is answered without a provider request."""
    s = ctx.spawn()
    s.submit("/unknown thing")
    s.wait_text("unknown command: /unknown thing")
    assert ctx.mock.requests == [], ctx.mock.requests
