"""Mouse selection: highlighting, OSC 52 copy and what invalidates a range."""


def transcript_turn(ctx, s, text="alpha beta gamma delta"):
    ctx.scenario(f"text={text.replace(' ', '+')}")
    s.submit("say something")
    s.wait_text(text)
    s.wait_turn_done()
    return s


def row_of(s, needle):
    """1-based screen row containing `needle`."""
    r = s.screen.find_row(needle)
    assert r >= 0, f"{needle!r} not on screen\n{s.text()}"
    return r + 1


def drag(s, row, col_from, col_to):
    s.mouse("down", row, col_from)
    s.mouse("drag", row, col_to)
    s.mouse("up", row, col_to)
    return s.sync()


def test_drag_copies_to_the_clipboard(ctx):
    """A drag over transcript text copies exactly those cells via OSC 52."""
    s = ctx.spawn()
    transcript_turn(ctx, s)
    row = row_of(s, "alpha beta gamma delta")
    # the body starts at column 3 (two-cell gutter)
    drag(s, row, 3, 3 + len("alpha") - 1)
    assert s.screen.clipboard == "alpha", repr(s.screen.clipboard)


def test_selection_is_highlighted(ctx):
    """Selected cells are painted in reverse video."""
    s = ctx.spawn()
    transcript_turn(ctx, s)
    row = row_of(s, "alpha beta gamma delta")
    s.mouse("down", row, 3)
    s.mouse("drag", row, 7).sync()
    attrs = [s.screen.attr_at(row - 1, c) for c in range(2, 7)]
    assert all(a.reverse for a in attrs), attrs
    assert not s.screen.attr_at(row - 1, 8).reverse


def test_welcome_art_highlights_in_one_colour(ctx):
    """Selecting across the welcome art keeps a uniform highlight.

    The centering padding is styled like the art it precedes, so reverse
    video shows one colour across the row instead of splitting where the
    glyphs start.
    """
    s = ctx.spawn()
    row = row_of(s, "_   _  ___ | | ___")   # spans padding on the left, art on the right
    s.mouse("down", row, 10)
    s.mouse("drag", row, 45).sync()
    attrs = [s.screen.attr_at(row - 1, c) for c in range(9, 45)]
    assert all(a.reverse for a in attrs), attrs
    fgs = {a.fg for a in attrs}
    assert fgs == {81}, f"highlight is not uniformly S_CYAN: {fgs}"


def test_copy_shows_a_status_notice(ctx):
    """The status line acknowledges the copy."""
    s = ctx.spawn()
    transcript_turn(ctx, s)
    row = row_of(s, "alpha beta gamma delta")
    drag(s, row, 3, 12)
    assert "copied" in s.status_line(), s.status_line()


def test_multi_row_selection_joins_with_newlines(ctx):
    """Dragging across rows copies them separated by line breaks.

    Selection is linear, as in xterm: rows after the first are taken from
    column one, so the body gutter comes along. Trailing padding is trimmed,
    since that is background the painter added rather than content.
    """
    s = ctx.spawn()
    ctx.scenario("text=first+row\\nsecond+row")
    s.submit("two rows please")
    s.wait_text("second row")
    s.wait_turn_done()

    first = row_of(s, "first row")
    second = row_of(s, "second row")
    assert second == first + 1, (first, second)
    s.mouse("down", first, 3)
    s.mouse("drag", second, 3 + len("second row") - 1)
    s.mouse("up", second, 3 + len("second row") - 1).sync()
    assert s.screen.clipboard == "first row\n  second row", repr(s.screen.clipboard)


def test_chrome_is_selectable_too(ctx):
    """Selection works over any painted cell, including the status line."""
    s = ctx.spawn()
    row = s.screen.rows            # status line, 1-based
    drag(s, row, 3, 3 + len("\u25cf ready") - 1)
    assert s.screen.clipboard == "\u25cf ready", repr(s.screen.clipboard)


def test_composer_text_is_selectable(ctx):
    """The composer is painted like everything else, so it can be copied."""
    s = ctx.spawn()
    s.type("copy this").sync()
    row = row_of(s, "copy this")
    drag(s, row, 5, 5 + len("copy") - 1)
    assert s.screen.clipboard == "copy", repr(s.screen.clipboard)


def test_click_without_drag_copies_nothing(ctx):
    """A plain click drops the old range and copies nothing."""
    s = ctx.spawn()
    transcript_turn(ctx, s)
    row = row_of(s, "alpha beta gamma delta")
    drag(s, row, 3, 7)
    assert s.screen.clipboard == "alpha"

    s.mouse("down", row, 5)
    s.mouse("up", row, 5).sync()
    assert len(s.screen.clipboard_writes) == 1, s.screen.clipboard_writes
    assert not any(
        s.screen.attr_at(row - 1, c).reverse for c in range(0, s.screen.cols)
    ), "the old highlight should be gone"


def test_typing_clears_the_selection(ctx):
    """A keystroke drops the highlight, like a terminal's own selection."""
    s = ctx.spawn()
    transcript_turn(ctx, s)
    row = row_of(s, "alpha beta gamma delta")
    s.mouse("down", row, 3)
    s.mouse("drag", row, 7).sync()
    assert s.screen.attr_at(row - 1, 3).reverse

    s.type("x").sync()
    assert not s.screen.attr_at(row - 1, 3).reverse


def test_new_output_clears_the_selection(ctx):
    """Streaming shifts the rows a highlight covered, so it is dropped."""
    s = ctx.spawn()
    transcript_turn(ctx, s)
    row = row_of(s, "alpha beta gamma delta")
    s.mouse("down", row, 3)
    s.mouse("drag", row, 7).sync()
    assert s.screen.attr_at(row - 1, 3).reverse

    ctx.scenario("text=new+output")
    s.submit("again")
    s.wait_text("new output")
    s.wait_turn_done()
    assert not any(
        s.screen.attr_at(r, c).reverse
        for r in range(s.screen.rows)
        for c in range(s.screen.cols)
    ), "no highlight should survive new output"


def test_selection_trims_row_padding(ctx):
    """Dragging past the end of a line does not copy the padding spaces."""
    s = ctx.spawn()
    transcript_turn(ctx, s, "short")
    row = row_of(s, "short")
    drag(s, row, 3, s.screen.cols)
    assert s.screen.clipboard == "short", repr(s.screen.clipboard)
