"""/settings: the toggles and values of a session, in one screen."""


def test_settings_lists_the_toggles_and_the_values(ctx):
    """Every row says what it is and what it currently says."""
    s = ctx.spawn()
    s.open_settings()
    text = s.text()
    for row in ("[ ] Verbose tool output", "[ ] Raw Markdown",
                "[x] Stream replies", "[ ] Ignored files", "[ ] Telemetry",
                "Mode", "Model", "Provider"):
        assert row in text, text
    assert "Space changes the selected row" in text, text
    ctx.check_screen(s)
    # More rows than the popup holds: the last is reached by moving down.
    s.key(*(["down"] * 8)).sync()
    assert "Max tokens" in s.text(), s.text()


def test_space_toggles_the_selected_row_only(ctx):
    """The checkbox is the answer: it flips, and its neighbours do not."""
    s = ctx.spawn()
    s.open_settings().settings_select("Raw Markdown")
    s.key("space").sync()
    text = s.text()
    assert "[x] Raw Markdown" in text, text
    assert "[ ] Verbose tool output" in text, text


def test_the_screen_stays_open_across_a_toggle(ctx):
    """Changing one setting is not a reason to ask for the screen again."""
    s = ctx.spawn()
    s.open_settings().settings_select("Verbose tool output")
    s.key("space").sync()
    s.key("space").sync()
    assert "[ ] Verbose tool output" in s.text(), s.text()


def test_enter_and_escape_close_it(ctx):
    """Both keys close; neither submits the composed line."""
    for key in ("enter", "esc"):
        s = ctx.spawn()
        s.open_settings()
        s.key(key)
        s.wait_gone("Verbose tool output")
        assert s.composer_text() == "", s.composer_lines()
        assert s.proc.poll() is None, f"{key} must not end the session"
        s.close()


def test_the_toggles_are_not_commands_of_their_own(ctx):
    """They live here now, so the composer offers no '/raw' to type."""
    s = ctx.spawn()
    s.type("/").sync()
    assert "/settings" not in s.text(), "it is below the eight visible rows"
    s.type("r").sync()
    text = s.text()
    assert "/raw" not in text and "/verbose" not in text, text
    assert "/rewind" in text, text


def test_the_mode_row_switches_mode(ctx):
    """The same switch Shift+Tab makes, named on the status line."""
    s = ctx.spawn()
    assert s.status_field(2) == "build", s.status_line()
    s.settings_act("Mode")
    s.wait_for(lambda t: s.status_field(2) == "plan", "plan mode")
    assert "Plan: read-only" in s.text(), s.text()


def test_max_tokens_is_asked_for_and_sent(ctx):
    """A value row opens the composer for it, and the next turn carries it."""
    ctx.scenario("text=short+answer")
    s = ctx.spawn()
    s.settings_act("Max tokens")
    s.wait_text("max tokens for one reply")
    s.type("128")
    s.key("enter")
    s.wait_text("    Max tokens")
    assert "128" in s.text(), s.text()
    s.key("esc")
    s.wait_gone("Max tokens")

    s.submit("say something")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["max_tokens"] == 128, ctx.mock.requests[-1]


def test_an_unreadable_max_tokens_leaves_it_alone(ctx):
    """A guess at what 'lots' means is worse than keeping the setting."""
    s = ctx.spawn()
    s.settings_act("Max tokens")
    s.wait_text("max tokens for one reply")
    s.type("lots")
    s.key("enter")
    s.wait_text("    Max tokens")
    assert "32768" in s.text(), s.text()


def test_streaming_off_sends_one_document(ctx):
    """The reply arrives whole, and the request says it asked for that."""
    ctx.scenario("text=all+at+once,usage=200/12")
    s = ctx.spawn()
    s.settings_toggle("Stream replies")
    s.submit("say it")
    s.wait_text("all at once")
    s.wait_turn_done()

    assert ctx.mock.requests[-1]["stream"] is False, ctx.mock.requests[-1]


def test_streaming_off_still_runs_tool_calls(ctx):
    """A tool call is a message field rather than a delta; the loop is one."""
    ctx.write_file("notes.txt", "written down\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    s = ctx.spawn()
    s.settings_toggle("Stream replies")
    s.submit("read the notes")
    s.wait_text("I read it")
    s.wait_turn_done()

    text = s.text()
    assert "written down" in text, text
    assert ctx.mock.tool_results()[0].strip() == "written down", (
        ctx.mock.tool_results()
    )


def test_streaming_back_on_keeps_working(ctx):
    """The toggle is per turn, not per session: both paths stay live."""
    ctx.scenario("text=first+one")
    s = ctx.spawn()
    s.settings_toggle("Stream replies")
    s.submit("one")
    s.wait_text("first one")
    s.wait_turn_done()

    ctx.scenario("text=second+one")
    s.settings_toggle("Stream replies")
    s.submit("two")
    s.wait_text("second one")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["stream"] is True, ctx.mock.requests[-1]
