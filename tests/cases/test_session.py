"""Session-level behaviour: slash commands, exit paths and terminal restore."""

import signal


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
    assert t.modes.get(1002) is False, "mouse tracking must be released"
    assert t.modes.get(1006) is False
    assert t.modes.get(7) is True, "autowrap must be restored"


def test_ctrl_d_on_empty_composer_exits(ctx):
    """Ctrl-D at an empty prompt is end-of-input."""
    s = ctx.spawn()
    s.key("ctrl-d")
    assert s.wait_exit() == 0
    assert not s.screen.alt_active


def test_clear_command_clears_the_transcript(ctx):
    """/clear wipes the visible history and the token counter."""
    ctx.scenario("text=remember+this,usage=500/10")
    s = ctx.spawn()
    s.submit("first message")
    s.wait_text("remember this")
    s.wait_turn_done()
    assert "510" in s.status_line(), s.status_line()

    s.submit("/clear")
    s.wait_for(lambda t: "remember this" not in t.text(), "transcript to clear")
    assert "first message" not in s.text()
    assert s.status_field(4) == "-", s.status_line()
    assert s.PLACEHOLDER in s.text()
    ctx.check_screen(s)


def test_clear_restores_the_welcome_screen(ctx):
    """An emptied transcript is a fresh start, welcome art included."""
    ctx.scenario("text=hi")
    s = ctx.spawn()
    s.submit("hello")
    s.wait_turn_done()
    assert "__ _| |__" not in s.text()

    s.submit("/clear")
    s.wait_text("__ _| |__")


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
    s.wait_status("thinking")
    s.resize(64, 20)
    s.wait_turn_done()
    assert s.screen.cols == 64
    assert s.status_kind() == "ready"


def test_sigint_outside_a_turn_clears_the_composer(ctx):
    """SIGINT at the prompt abandons the line rather than quitting."""
    s = ctx.spawn()
    s.type("half typed").sync()
    s.signal(signal.SIGINT)
    s.wait_text("^C")
    assert s.composer_text() == ""
    assert s.proc.poll() is None, "SIGINT must not kill the session"


def test_unknown_slash_command_is_sent_as_text(ctx):
    """Anything that is not a known command is just a message."""
    ctx.scenario("text=not+a+command")
    s = ctx.spawn()
    s.submit("/unknown thing")
    s.wait_text("not a command")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["messages"][-1]["content"] == "/unknown thing"
