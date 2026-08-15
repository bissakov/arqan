"""Glyph widths come from this program's tables, not from the C library.

wcwidth() answers from whatever Unicode version the libc carries, and mbrtowc
answers from the locale, so the same transcript framed differently depending
on the machine and on LC_CTYPE. Both now come from src/width.c, which
scripts/gen-width.py generates, and these cases pin the answers a frame is
built from.
"""

GUTTER = 4      # the composer's gutter and prompt, before any typed cell


def col(s) -> int:
    """The cursor's column counted from the first typed cell."""
    row, c = s.cursor
    assert row == s.screen.rows - 3, s.debug_dump()
    return c - 1 - GUTTER


def test_ascii_is_one_column_each(ctx):
    """The baseline the other cases are measured against."""
    s = ctx.spawn()
    s.type("hello").sync()
    assert col(s) == 5, s.debug_dump()


def test_east_asian_glyphs_take_two_columns(ctx):
    """A wide glyph claims the cell after it as well."""
    s = ctx.spawn()
    s.type("日本").sync()
    assert col(s) == 4, s.debug_dump()


def test_combining_mark_takes_no_column(ctx):
    """A mark composes onto the letter before it instead of moving the cursor."""
    s = ctx.spawn()
    s.type("e\u0301").sync()
    assert col(s) == 1, s.debug_dump()


def test_format_character_takes_no_column(ctx):
    """A zero width joiner is not a cell, however many bytes it is."""
    s = ctx.spawn()
    s.type("a\u200db").sync()
    assert col(s) == 2, s.debug_dump()


def test_emoji_takes_two_columns(ctx):
    """Wide by East Asian width, and four bytes rather than the usual three."""
    s = ctx.spawn()
    s.type("\U0001f600").sync()
    assert col(s) == 2, s.debug_dump()


def test_columns_do_not_depend_on_the_locale(ctx):
    """LC_ALL=C frames exactly as a UTF-8 locale does.

    Measuring through mbrtowc made an unset or C locale decode every byte on
    its own: "日本" became six cells, and everything after it on the row was
    painted in the wrong place.
    """
    text = "h\u00e9llo \u65e5\u672c ok"
    utf8 = ctx.spawn()
    utf8.type(text).sync()
    expected = col(utf8)
    assert expected == 6 + 4 + 3, utf8.debug_dump()

    plain = ctx.spawn(LC_ALL="C", LANG="C")
    plain.type(text).sync()
    assert plain.composer_text() == text, plain.composer_lines()
    assert col(plain) == expected, plain.debug_dump()


def test_wide_glyphs_wrap_on_the_cell_not_the_byte(ctx):
    """A reply of wide glyphs wraps where the columns run out."""
    ctx.scenario("text=" + "\u65e5" * 40)
    s = ctx.spawn(cols=40, rows=20)
    s.submit("say it")
    s.wait_turn_done()
    rows = [r for r in s.screen.lines()[: s.transcript_height()] if "\u65e5" in r]
    assert rows, s.text()
    for r in rows:
        # Two columns each, inside the transcript's own margins.
        assert len(r.replace(" ", "")) <= 20, repr(r)


def test_malformed_bytes_do_not_stall_the_composer(ctx):
    """A byte that is not a UTF-8 sequence advances one byte and no more."""
    s = ctx.spawn()
    s.send(b"\xff")
    s.type("ok").sync()
    assert "ok" in s.composer_text(), s.composer_lines()
    assert s.status_kind() == "ready", s.status_line()
