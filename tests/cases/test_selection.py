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
    row = row_of(s, "(_| | | | (_| |")   # spans padding on the left, art on the right
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


def test_a_drag_under_tmux_says_what_carries_the_copy_once(ctx):
    """tmux drops the sequence by default; the caveat is said on the first
    drag and not on every one after it."""
    s = ctx.spawn(TMUX="/tmp/tmux-1000/default,4242,0")
    transcript_turn(ctx, s)
    row = row_of(s, "alpha beta gamma delta")
    drag(s, row, 3, 12)
    s.wait_text("set-clipboard on")
    s.key("esc")           # Esc retires the notice
    s.wait_gone("set-clipboard on")
    drag(s, row, 3, 12)
    assert "set-clipboard on" not in s.text(), s.text()


def test_multi_row_selection_joins_with_newlines(ctx):
    """Dragging across rows copies them separated by line breaks.

    Selection is linear, as in xterm, but the body gutter is layout rather
    than text, so it stays behind. Trailing padding is trimmed, since that is
    background the painter added rather than content.
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
    assert s.screen.clipboard == "first row\nsecond row", repr(s.screen.clipboard)


# Words of five different lengths, so the rows wrap ragged and justification
# has gaps to widen.
WRAP_WORDS = [f"w{i:02d}" + "x" * (i % 5) for i in range(40)]
WRAP_TEXT = " ".join(WRAP_WORDS)


def wrapped_reply(ctx, s, text):
    ctx.scenario("text=" + text.replace(" ", "+"))
    s.submit("say it")
    s.wait_text(text.split()[-1])
    s.wait_turn_done()
    first = row_of(s, text.split()[0])
    last = row_of(s, text.split()[-1])
    assert last > first, s.text()
    return first, last


def drag_rows(s, first, last):
    s.mouse("down", first, 1)
    s.mouse("drag", last, s.screen.cols)
    s.mouse("up", last, s.screen.cols)
    return s.sync()


def test_a_wrapped_line_copies_as_one_line(ctx):
    """Rows the painter wrapped join back into the line the text had, with
    the gap they broke at and without the gutter."""
    s = ctx.spawn()
    first, last = wrapped_reply(ctx, s, WRAP_TEXT)
    drag_rows(s, first, last)
    assert s.screen.clipboard == WRAP_TEXT, repr(s.screen.clipboard)


def test_a_justified_line_copies_without_the_padding(ctx):
    """Justification widens the gaps on screen, not in the copied text."""
    s = ctx.spawn()
    first, last = wrapped_reply(ctx, s, WRAP_TEXT)
    s.settings_act("Text wrap")
    s.wait_for(lambda t: s.settings_option("Text wrap") == "Justified",
               "justified wrapping")
    s.key("esc")
    s.wait_gone("Text wrap")
    first = row_of(s, WRAP_WORDS[0])
    last = row_of(s, WRAP_WORDS[-1])
    assert "  " in s.screen.row_text(first - 1).strip(), s.text()
    drag_rows(s, first, last)
    assert s.screen.clipboard == WRAP_TEXT, repr(s.screen.clipboard)


def test_a_word_split_by_the_width_copies_whole(ctx):
    """A word wider than the row is split with no gap, so it joins with
    none."""
    s = ctx.spawn(cols=40, rows=20)
    word = "z" * 60
    first, last = wrapped_reply(ctx, s, f"head {word} tail")
    drag_rows(s, first, last)
    assert s.screen.clipboard == f"head {word} tail", repr(s.screen.clipboard)


def test_a_wrapped_draft_copies_as_typed(ctx):
    """The composer's prompt marker and wrapping are layout too."""
    s = ctx.spawn(cols=40, rows=20)
    draft = "alpha bravo charlie delta echo foxtrot golf"
    s.type(draft).sync()
    first = row_of(s, "alpha bravo")
    last = row_of(s, "foxtrot golf")
    assert last == first + 1, s.text()
    s.mouse("down", first, 1)
    s.mouse("drag", last, 5 + len("foxtrot golf") - 1)
    s.mouse("up", last, 5 + len("foxtrot golf") - 1)
    s.sync()
    assert s.screen.clipboard == draft, repr(s.screen.clipboard)


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
