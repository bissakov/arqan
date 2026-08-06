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
    """Ctrl-C abandons the line and notes it in the transcript."""
    s = ctx.spawn()
    s.type("throw this away").sync()
    s.key("ctrl-c").sync()
    s.wait_text("^C")
    assert s.composer_text() == "", s.composer_lines()
    assert s.PLACEHOLDER in s.text(), "placeholder should be back"
    assert ctx.mock.requests == []


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
