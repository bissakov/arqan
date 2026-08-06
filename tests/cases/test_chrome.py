"""Status line, colour handling and where diagnostics end up."""


def test_status_text_tracks_the_turn(ctx):
    """The state is spelled out, not only implied by the bullet's colour."""
    ctx.scenario("words=30,chunk=1,delay=0.05,first_delay=0.15")
    s = ctx.spawn()
    assert s.status_kind() == "ready", s.status_line()
    s.submit("think about it")
    s.wait_status("thinking")
    assert "thinking" in s.status_line(), s.status_line()
    s.wait_turn_done()
    assert "ready" in s.status_line(), s.status_line()


def test_status_is_readable_without_colour(ctx):
    """NO_COLOR keeps the state legible: it is text, not just a colour."""
    ctx.scenario("words=30,chunk=1,delay=0.05,first_delay=0.15")
    s = ctx.spawn(NO_COLOR="1")
    assert s.status_kind() == "ready", s.status_line()
    s.submit("think about it")
    s.wait_status("thinking")
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
    s.wait_status("thinking")
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

    assert fg_of("\u25c6  Tool") == 221         # S_YELLOW
    assert fg_of("\u2514\u2500 Result") == 114  # S_GREEN


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
    s = ctx.spawn(YOKE_BASE_URL="http://127.0.0.1:1/v1")
    s.submit("nobody is listening")
    s.wait_text("[provider error:")
    s.wait_turn_done()

    text = s.text()
    assert "[error: curl:" in text, text
    assert "[yoke ERR]" not in text, "raw stderr must not reach the screen"
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


def test_context_counter_formats_tokens(ctx):
    """The token field shows a dash until the provider reports usage."""
    ctx.scenario("text=ok,usage=900/100")
    s = ctx.spawn()
    assert s.status_field(4) == "-", s.status_line()
    s.submit("count")
    s.wait_turn_done()
    assert s.status_line().endswith("1000"), s.status_line()


def test_narrow_status_line_drops_fields_from_the_right(ctx):
    """On a narrow screen the state survives and the tail is clipped."""
    s = ctx.spawn(cols=30, rows=12)
    line = s.status_line()
    assert "ready" in line, line
    assert len(line) <= 30, line
