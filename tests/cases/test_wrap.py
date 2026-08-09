"""How a row too long for the width is broken, and how it fills it."""

import json

# Words of five different lengths on a 76-column body, so no row fills it
# exactly and a character wrap has to split one of them.
WORDS = [f"w{i:02d}" + "x" * (i % 5) for i in range(40)]
REPLY = "+".join(WORDS)


def transcript_rows(s):
    """The rows holding the reply, indented by the transcript's gutter."""
    return [r for r in s.text().splitlines() if r.startswith("  w0")
            or r.startswith("  w1") or r.startswith("  w2")
            or r.startswith("  w3")]


def test_a_row_breaks_between_words(ctx):
    """Every word arrives whole: the break goes in the gap before it."""
    ctx.scenario(f"text={REPLY}")
    s = ctx.spawn()
    s.submit("say it")
    s.wait_text(WORDS[-1])
    s.wait_turn_done()
    rows = transcript_rows(s)
    assert len(rows) > 1, rows
    for word in WORDS:
        assert any(word in row for row in rows), (word, rows)


def test_the_break_costs_no_leading_space(ctx):
    """The space a row broke at belongs to neither row."""
    ctx.scenario(f"text={REPLY}")
    s = ctx.spawn()
    s.submit("say it")
    s.wait_text(WORDS[-1])
    s.wait_turn_done()
    for row in transcript_rows(s):
        assert not row.startswith("   "), row


def test_word_wrap_leaves_the_rows_ragged(ctx):
    """The default ends a row where a word does, short of the edge."""
    ctx.scenario(f"text={REPLY}")
    s = ctx.spawn()
    s.submit("say it")
    s.wait_text(WORDS[-1])
    s.wait_turn_done()
    rows = transcript_rows(s)
    assert any(len(row.rstrip()) < 78 for row in rows[:-1]), rows


def test_justification_fills_the_row(ctx):
    """Every wrapped row reaches the right edge, the last one excepted."""
    ctx.scenario(f"text={REPLY}")
    s = ctx.spawn()
    s.submit("say it")
    s.wait_text(WORDS[-1])
    s.wait_turn_done()
    s.settings_act("Text wrap")
    s.wait_for(lambda t: s.settings_option("Text wrap") == "Justified",
               "justified wrapping")
    s.key("esc")
    s.wait_gone("Text wrap")
    rows = transcript_rows(s)
    assert len(rows) > 1, rows
    for row in rows[:-1]:
        assert len(row.rstrip()) == 78, (len(row.rstrip()), row)
    # The words are still all there, and still whole.
    for word in WORDS:
        assert any(word in row for row in rows), (word, rows)
    ctx.check_screen(s)


def test_justification_is_a_setting_that_flips_back(ctx):
    """Turning it off repaints what is already on screen, ragged again."""
    ctx.scenario(f"text={REPLY}")
    s = ctx.spawn()
    s.submit("say it")
    s.wait_text(WORDS[-1])
    s.wait_turn_done()
    s.settings_act("Text wrap")
    s.wait_for(lambda t: s.settings_option("Text wrap") == "Justified",
               "justified wrapping")
    s.key("space")
    s.wait_for(lambda t: s.settings_option("Text wrap") == "Word",
               "word wrapping")
    s.key("esc")
    s.wait_gone("Text wrap")
    rows = transcript_rows(s)
    assert any(len(row.rstrip()) < 78 for row in rows[:-1]), rows


def test_the_composer_wraps_between_words_too(ctx):
    """A draft is text: it breaks in the gaps, and the cursor follows it."""
    s = ctx.spawn(cols=40, rows=20)
    s.type("alpha bravo charlie delta echo foxtrot golf").sync()
    first, second = s.composer_body(2)
    assert first == "alpha bravo charlie delta echo", (first, second)
    assert second == "foxtrot golf", (first, second)
    # The cursor sits one cell past the last typed one, on the second row.
    row, col = s.cursor
    assert col == s.gutter() + len(second) + 1, (row, col)


def test_a_tool_result_is_not_justified(ctx):
    """Widening the gaps of output a reader lines up would move its columns."""
    args = json.dumps({"command": f"echo {' '.join(WORDS)}"})
    ctx.scenario(f"tool=bash:{args},final_text=ran+it")
    s = ctx.spawn()
    s.submit("run echo")
    s.wait_text("ran it")
    s.wait_turn_done()
    before = [r for r in s.text().splitlines() if "w2" in r]
    assert len(before) > 1, s.text()
    s.settings_act("Text wrap")
    s.wait_for(lambda t: s.settings_option("Text wrap") == "Justified",
               "justified wrapping")
    s.key("esc")
    s.wait_gone("Text wrap")
    after = [r for r in s.text().splitlines() if "w2" in r]
    assert after == before, (before, after)


def test_a_short_last_row_is_left_alone(ctx):
    """A row its author ended is as long as it was meant to be."""
    ctx.scenario("text=short+line\\nand+another")
    s = ctx.spawn()
    s.submit("say it")
    s.wait_text("and another")
    s.wait_turn_done()
    s.settings_act("Text wrap")
    s.wait_for(lambda t: s.settings_option("Text wrap") == "Justified",
               "justified wrapping")
    s.key("esc")
    s.wait_gone("Text wrap")
    text = s.text()
    assert "  short line" in text, text
    assert "  and another" in text, text
