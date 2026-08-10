"""Composer editing: text entry, motion, kill keys, wrapping, unicode.

Every keystroke repaints, so `session.sync()` (wait for that repaint, then for
quiet) is what keeps these assertions off a stale screen.
"""


def test_types_text(ctx):
    """Typed bytes land in the composer and push the placeholder out."""
    s = ctx.spawn()
    s.type("hello world").sync()
    assert s.composer_text() == "hello world", s.composer_lines()
    assert s.PLACEHOLDER not in s.text()
    ctx.check_screen(s)


def test_cursor_follows_text(ctx):
    """The terminal cursor sits after the last typed glyph."""
    s = ctx.spawn()
    s.type("abc").sync()
    # gutter (2) + "› " (2) + 3 glyphs, 1-based
    assert s.cursor == (s.screen.rows - 3, 8), s.cursor
    assert s.screen.cursor_visible


def test_backspace_and_motion(ctx):
    """Backspace, arrows and home/end move and edit in place."""
    s = ctx.spawn()
    s.type("hello world").sync()
    s.key(*["backspace"] * 5).sync()
    assert s.composer_text() == "hello", s.composer_lines()   # "hello " padded
    s.type("there").sync()
    s.key("home").sync()
    s.type(">").sync()
    s.key("end").sync()
    s.type("!").sync()
    assert s.composer_text() == ">hello there!", s.composer_lines()
    s.key("left", "left").sync()
    s.type("_").sync()
    assert s.composer_text() == ">hello ther_e!", s.composer_lines()


def test_emacs_editing_keys(ctx):
    """Ctrl-A/E/B/F/K/U/W behave like readline."""
    s = ctx.spawn()
    s.type("alpha beta gamma").sync()

    s.key("ctrl-w").sync()                          # kill the last word
    assert s.composer_text() == "alpha beta", s.composer_lines()

    s.key("ctrl-a").sync()
    s.type("X ").sync()
    s.key("ctrl-e").sync()
    s.type("end").sync()
    assert s.composer_text() == "X alpha beta end", s.composer_lines()

    s.key("ctrl-a", "ctrl-f", "ctrl-f", "ctrl-k").sync()
    # ctrl-k keeps "X ", but the panel pads the row so the space is invisible
    assert s.composer_text() == "X", s.composer_lines()

    s.type("tail").sync()
    s.key("ctrl-b", "ctrl-b").sync()
    s.key("ctrl-u").sync()                          # kill back to the start
    assert s.composer_text() == "il", s.composer_lines()


def test_line_keys_are_line_scoped(ctx):
    """Ctrl-A/E/K act on the composer line the cursor is on, not the buffer."""
    s = ctx.spawn()
    s.type("first line").sync()
    s.key("newline").sync()
    s.type("second line").sync()

    s.key("ctrl-a").sync()
    s.type(">").sync()
    assert s.composer_body(2) == ["first line", ">second line"], s.composer_lines(2)

    s.key("ctrl-e").sync()
    s.type("!").sync()
    assert s.composer_body(2) == ["first line", ">second line!"], s.composer_lines(2)

    s.key("ctrl-a", "ctrl-k").sync()
    assert s.composer_body(2) == ["first line", ""], s.composer_lines(2)
    assert ctx.mock.requests == []


def test_ctrl_k_on_empty_tail_joins_the_next_line(ctx):
    """With nothing left on the line, Ctrl-K eats the break like readline."""
    s = ctx.spawn()
    s.type("one").sync()
    s.key("newline").sync()
    s.type("two").sync()
    s.key("ctrl-a").sync()          # start of "two"
    s.key("ctrl-b").sync()          # onto the end of "one"
    s.key("ctrl-k").sync()
    assert s.composer_text() == "onetwo", s.composer_lines()


def test_word_motion(ctx):
    """Ctrl-Left / Ctrl-Right jump whole words."""
    s = ctx.spawn()
    s.type("one two three").sync()
    s.key("ctrl-left", "ctrl-left").sync()          # before "two"
    s.type("<").sync()
    assert s.composer_text() == "one <two three", s.composer_lines()
    s.key("ctrl-right").sync()                      # end of "two"
    s.type(">").sync()
    assert s.composer_text() == "one <two> three", s.composer_lines()


def test_forward_delete(ctx):
    """Ctrl-D deletes forward while there is text to delete."""
    s = ctx.spawn()
    s.type("abcdef").sync()
    s.key("ctrl-a", "delete", "delete").sync()
    assert s.composer_text() == "cdef", s.composer_lines()


def test_alt_enter_inserts_newline(ctx):
    """Alt-Enter grows the composer instead of submitting."""
    s = ctx.spawn()
    s.type("first line").sync()
    s.key("newline").sync()
    s.type("second line").sync()
    assert s.composer_body(2) == ["first line", "second line"], s.composer_lines(2)
    assert ctx.mock.requests == [], "nothing should have been sent"
    ctx.check_screen(s)


def test_wraps_long_input(ctx):
    """Input longer than the body wraps onto a second composer row."""
    s = ctx.spawn(cols=40, rows=20)
    body = "w" * 34            # 40 cols - 2*2 gutter - 2 prompt cells
    s.type(body + "TAIL").sync()
    assert s.composer_text(2) == body + "TAIL", s.composer_lines(2)


def test_composer_height_is_capped(ctx):
    """A tall composer stops at a third of the screen and scrolls internally."""
    s = ctx.spawn(cols=60, rows=24)
    for i in range(12):
        s.type(f"line{i}")
        s.key("newline")
    s.type("last").sync()
    text = s.text()
    assert "last" in text
    assert "line0" not in text, "oldest composer rows scroll out of view"
    assert "line11" in text, text
    ctx.check_screen(s)


def test_unicode_input(ctx):
    """Multi-byte and double-width glyphs are inserted and measured correctly."""
    s = ctx.spawn()
    s.type("héllo 日本 ok").sync()
    assert s.composer_text() == "héllo 日本 ok", s.composer_lines()
    # gutter+prompt (4) + 6 ASCII cells + 4 cells of CJK + 3 for " ok", 1-based
    assert s.cursor == (s.screen.rows - 3, 4 + 6 + 4 + 3 + 1), s.cursor


def test_unicode_backspace_deletes_whole_glyph(ctx):
    """Backspace removes a full UTF-8 glyph, not one byte."""
    s = ctx.spawn()
    s.type("日本語").sync()
    s.key("backspace").sync()
    assert s.composer_text() == "日本", s.composer_lines()


def test_ctrl_c_clears_composer(ctx):
    """Ctrl-C abandons the line without writing anything to the transcript."""
    ctx.scenario("text=kept")
    s = ctx.spawn()
    s.submit("first message")
    s.wait_text("kept")
    s.wait_turn_done()
    before = s.text()

    s.type("throw this away").sync()
    s.key("ctrl-c").sync()
    s.wait_for(lambda t: s.composer_text() == "", "the composer to clear")
    assert s.PLACEHOLDER in s.text(), "placeholder should be back"
    assert "^C" not in s.text(), "an abandoned draft leaves no trace"
    assert "throw this away" not in s.text()
    assert s.text() == before, "the transcript is untouched"
    assert len(ctx.mock.requests) == 1


def test_ctrl_l_redraws(ctx):
    """Ctrl-L forces a full repaint without disturbing the composer."""
    s = ctx.spawn()
    s.type("keep me").sync()
    before = s.snapshot()
    s.key("ctrl-l").sync()
    assert s.snapshot() == before, "redraw must be idempotent"


def test_empty_enter_does_not_call_provider(ctx):
    """Enter on an empty composer is a no-op."""
    s = ctx.spawn()
    s.key("enter").sync()
    assert ctx.mock.requests == []
    assert s.PLACEHOLDER in s.text()


def test_application_cursor_keys_are_understood(ctx):
    """A terminal in DECCKM sends SS3 arrows; they edit like the CSI ones."""
    s = ctx.spawn()
    s.type("abc").sync()
    s.key("ss3-left", "ss3-left").sync()
    s.type("X").sync()
    assert s.composer_text() == "aXbc", s.composer_lines()
    s.key("ss3-right").sync()
    s.type("Y").sync()
    assert s.composer_text() == "aXbYc", s.composer_lines()


def test_application_cursor_keys_move_the_popup(ctx):
    """And SS3 Down moves a popup selection the way CSI Down does."""
    s = ctx.spawn()
    s.type("/").sync()
    s.key("ss3-down").sync()
    s.key("tab").sync()
    assert s.composer_text() == "/resume", s.composer_lines()


def test_bracketed_paste_mode_is_enabled(ctx):
    """The terminal is told to bracket a paste, which is what makes it text."""
    s = ctx.spawn()
    s.type("x").sync()
    assert s.term.modes.get(2004) is True, s.term.modes


def test_multiline_paste_keeps_every_line(ctx):
    """A pasted line break grows the composer instead of submitting."""
    s = ctx.spawn()
    s.paste("first line\nsecond line\nthird line")
    s.sync()
    assert s.composer_body(3) == ["first line", "second line", "third line"], \
        s.composer_lines(3)
    assert ctx.mock.requests == [], "nothing should have been sent"
    ctx.check_screen(s)


def test_pasted_crlf_is_one_break(ctx):
    """Windows line endings do not double the composer's rows."""
    s = ctx.spawn()
    s.paste("one\r\ntwo\rthree")
    s.sync()
    assert s.composer_body(3) == ["one", "two", "three"], s.composer_lines(3)


def test_paste_then_enter_submits_the_whole_message(ctx):
    """Enter after the paste sends every line as one message."""
    ctx.scenario("text=got+it")
    s = ctx.spawn()
    s.paste("alpha\nbeta")
    s.sync()
    s.submit()
    s.wait_turn_done()
    body = ctx.mock.requests[-1]["messages"][-1]["content"]
    assert body == "alpha\nbeta", body


def test_paste_does_not_run_a_command(ctx):
    """A pasted slash line is text: it neither completes nor submits."""
    s = ctx.spawn()
    s.paste("/clear\nrest")
    s.sync()
    assert s.composer_body(2) == ["/clear", "rest"], s.composer_lines(2)
    assert ctx.mock.requests == []


def test_paste_while_a_turn_runs_is_kept(ctx):
    """Typing stays live mid-turn, and so does a multi-line paste."""
    ctx.scenario("words=30,chunk=2,delay=0.02")
    s = ctx.spawn()
    s.submit("go")
    s.paste("queued one\nqueued two")
    s.wait_turn_done()
    assert s.composer_body(2) == ["queued one", "queued two"], s.composer_lines(2)


def test_pasted_tab_becomes_spaces(ctx):
    """A tab stop would move the cursor out from under the row snapshot."""
    s = ctx.spawn()
    s.paste("if x:\n\tpass")
    s.sync()
    assert s.composer_body(2) == ["if x:", "    pass"], s.composer_lines(2)


def test_up_moves_between_draft_lines(ctx):
    """Up walks the draft's own rows instead of leaving for history."""
    s = ctx.spawn()
    s.type("abcdef")
    s.key("newline")
    s.type("ghijkl").sync()
    s.key("up").sync()
    s.type("X").sync()
    assert s.composer_body(2) == ["abcdXef", "ghijkl"], s.composer_lines(2)


def test_down_moves_back_to_the_lower_line(ctx):
    """Down mirrors Up, landing on the column the run started from."""
    s = ctx.spawn()
    s.type("abcdef")
    s.key("newline")
    s.type("ghijkl").sync()
    s.key("up", "down").sync()
    s.type("Y").sync()
    assert s.composer_body(2) == ["abcdef", "ghijklY"], s.composer_lines(2)


def test_vertical_motion_keeps_its_goal_column(ctx):
    """Crossing a short row does not shorten the rows after it."""
    s = ctx.spawn()
    s.type("aaaaaaaa")
    s.key("newline")
    s.type("bb")
    s.key("newline")
    s.type("cccccccc").sync()
    s.key("up", "up").sync()
    s.type("X").sync()
    # The first row carries the two-cell prompt, so the same screen column is
    # two bytes earlier in its text.
    assert s.composer_body(3) == ["aaaaaaXaa", "bb", "cccccccc"], s.composer_lines(3)


def test_up_walks_wrapped_rows(ctx):
    """A soft-wrapped line is two rows to walk, not one."""
    s = ctx.spawn(cols=40, rows=20)
    body = "w" * 34            # 40 cols - 2*2 gutter - 2 prompt cells
    s.type(body + "tail").sync()
    s.key("up").sync()
    s.type("X").sync()
    assert s.composer_text(2) == body[:2] + "X" + body[2:] + "tail", s.composer_lines(2)


def test_up_pages_through_a_tall_draft(ctx):
    """Walking off the top of the composer box scrolls it, one row at a time."""
    s = ctx.spawn(cols=60, rows=24)
    for i in range(12):
        s.type(f"line{i}")
        s.key("newline")
    s.type("last").sync()
    assert "line0" not in s.text(), "the oldest rows start out of view"
    for _ in range(12):
        s.key("up")
    s.sync()
    text = s.text()
    assert "line0" in text, text
    assert "last" not in text, "the box scrolled past the bottom rows"
