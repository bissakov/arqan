"""Status line, colour handling and where diagnostics end up."""


def test_status_text_tracks_the_turn(ctx):
    """The state is spelled out, not only implied by the bullet's colour: on
    the status line when nothing runs, on the spinner row while one does."""
    ctx.scenario("words=30,chunk=1,delay=0.05,first_delay=0.15")
    s = ctx.spawn()
    assert s.status_kind() == "ready", s.status_line()
    s.submit("think about it")
    s.wait_activity("thinking")
    assert "thinking" not in s.status_line(), s.status_line()
    s.wait_turn_done()
    assert "ready" in s.status_line(), s.status_line()


def test_status_is_readable_without_colour(ctx):
    """NO_COLOR keeps the state legible: it is text, not just a colour."""
    ctx.scenario("words=30,chunk=1,delay=0.05,first_delay=0.15")
    s = ctx.spawn(NO_COLOR="1")
    assert s.status_kind() == "ready", s.status_line()
    s.submit("think about it")
    s.wait_activity("thinking")
    s.wait_turn_done()
    assert s.status_kind() == "ready"


def test_no_color_renders_the_same_glyphs(ctx):
    """Colour is decoration: the layout must not depend on it."""
    ctx.scenario("text=same+either+way")
    coloured = ctx.spawn()
    coloured.submit("hello")
    coloured.wait_text("same either way")
    coloured.wait_turn_done()

    plain = ctx.spawn(NO_COLOR="1")
    plain.submit("hello")
    plain.wait_text("same either way")
    plain.wait_turn_done()

    assert plain.snapshot() == coloured.snapshot()
    row = coloured.screen.find_row("same either way")
    assert coloured.screen.attr_at(row, 2).fg is not None, "colour run should style"
    assert plain.screen.attr_at(row, 2).fg is None, "NO_COLOR run should not"


def test_status_colour_encodes_the_state(ctx):
    """The bullet repeats the state as colour for a glance at the screen."""
    ctx.scenario("words=30,chunk=1,delay=0.05,first_delay=0.15")
    s = ctx.spawn()
    assert s.status_colour() == "ready"
    s.submit("think")
    s.wait_activity("thinking")
    assert s.status_colour() == "thinking"
    s.wait_turn_done()
    assert s.status_colour() == "ready"


def test_transcript_roles_are_styled(ctx):
    """Tool and result rows each get their own colour."""
    ctx.write_file("f.txt", "file body")
    ctx.scenario('tool=read:{"path":"f.txt"},final_text=done')
    s = ctx.spawn()
    s.submit("read it")
    s.wait_text("done")
    s.wait_turn_done()

    def fg_of(needle):
        row = s.screen.find_row(needle)
        assert row >= 0, f"{needle!r} missing\n{s.text()}"
        return s.screen.attr_at(row, 2).fg

    assert fg_of("\u25c6  read f.txt") == 221   # S_YELLOW
    assert fg_of("\u2514\u2500 1 line") == 114  # S_GREEN


def test_user_message_is_a_box(ctx):
    """A user turn is a padded block of background, with no role label."""
    ctx.scenario("text=answered")
    s = ctx.spawn()
    s.submit("my question")
    s.wait_text("answered")
    s.wait_turn_done()

    text = s.text()
    assert "You" not in text, text
    assert "Assistant" not in text, text

    row = s.screen.find_row("my question")
    assert row >= 0, text
    bg = s.screen.attr_at(row, 2).bg
    assert bg == 238, f"user row should carry the panel background: {bg}"
    # the box is padded above and below, and spans the full width
    assert s.screen.attr_at(row - 1, 2).bg == 238, "padding row above"
    assert s.screen.attr_at(row + 1, 2).bg == 238, "padding row below"
    assert s.screen.attr_at(row, s.screen.cols - 2).bg == 238, "box spans the body"
    # the agent's own output is not boxed
    reply = s.screen.find_row("answered")
    assert s.screen.attr_at(reply, 2).bg is None, "assistant text is unboxed"


def test_status_fields_use_single_spacing(ctx):
    """Status items are separated by one space either side of the dot."""
    s = ctx.spawn()
    line = s.status_line()
    assert "  \u00b7  " not in line, line
    assert line.startswith("\u25cf ready \u00b7 "), line


def test_log_output_becomes_a_transcript_notice(ctx):
    """Diagnostics go into the transcript, never raw onto the frame."""
    s = ctx.spawn(ARQAN_BASE_URL="http://127.0.0.1:1/v1")
    s.submit("nobody is listening")
    s.wait_text("[provider error:")
    s.wait_turn_done()

    text = s.text()
    assert "[error: curl:" in text, text
    assert "[arqan ERR]" not in text, "raw stderr must not reach the screen"
    # the frame is still intact: chrome in its usual places
    assert s.PLACEHOLDER in text
    assert "ready" in s.status_line()


def test_notice_rows_are_styled_as_notices(ctx):
    """Bracketed rows read as notices rather than as model output."""
    ctx.scenario("status=500")
    s = ctx.spawn()
    s.submit("fail please")
    s.wait_text("[provider error: HTTP 500]")
    s.wait_turn_done()
    row = s.screen.find_row("[provider error")
    assert s.screen.attr_at(row, 2).fg == 221, "S_YELLOW notice"


def test_context_counter_reports_the_context_not_the_bill(ctx):
    """The field shows a dash until a request is measured, and then the
    context that request carried: the reply the provider charged for is an
    estimate on top of it, not part of the measurement."""
    ctx.scenario("text=ok,usage=900/100")
    s = ctx.spawn()
    assert s.status_field(5) == "-", s.status_line()
    s.submit("count")
    s.wait_turn_done()
    field = s.status_field(5)
    assert field.startswith("~"), f"the reply is not measured: {field}"
    assert 900 <= int(field.lstrip("~")) < 1000, s.status_line()


def test_status_names_active_reasoning_controls(ctx):
    """Configured reasoning sits between the model and the agent mode."""
    config = ctx.write_config(
        f"[providers.local-chatgpt]\n"
        f"base_url = {ctx.mock.base_url}\n"
        "model = gpt-5.6-terra\n"
        '[providers.local-chatgpt.models."gpt-5.6-terra"]\n'
        "reasoning_efforts = low,xhigh\n"
        "reasoning_effort = xhigh\n"
        "thinking_budgets = 1024,2048\n"
        "thinking_budget = 1024\n"
    )
    assert config.exists()
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = local-chatgpt\n")
    s = ctx.spawn(cols=160, ARQAN_BASE_URL=None, ARQAN_API_KEY=None,
                  ARQAN_MODEL=None)
    fields = [s.status_field(i) for i in range(8)]
    assert fields[:6] == [
        "ready", "gpt-5.6-terra", "xhigh", "thinking 1024", "build",
        "local-chatgpt",
    ], s.status_line()


def test_narrow_status_line_drops_fields_from_the_right(ctx):
    """On a narrow screen the state survives and the tail is clipped."""
    s = ctx.spawn(cols=40, rows=12)
    line = s.status_line()
    assert "ready" in line, line
    assert len(line) <= 40, line
