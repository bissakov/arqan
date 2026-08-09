"""Transcript messages are Markdown, and the composer remains literal."""

TEXT = 253      # S_TEXT
MUTED = 245     # S_MUTED
CYAN = 81       # S_CYAN, headings
BLUE = 75       # S_BLUE, bullets and other markers
MONO = 180      # S_MONO, inline code
CODE_BG = 235   # S_CODE_BG, fenced blocks
USER_BG = 238   # S_USER_BG, submitted user messages


def cell(s, needle: str, offset: int = 0):
    """The attributes of a glyph of `needle` as painted on screen."""
    for r in range(s.screen.rows):
        row = s.screen.row_text(r)
        if needle in row:
            return s.screen.attr_at(r, row.index(needle) + offset)
    raise AssertionError(f"{needle!r} is not on screen:\n{s.text()}")


def transcript(s, after: str) -> str:
    """What the agent wrote below the user's turn, chrome excluded."""
    body = "\n".join(s.screen.lines()[: s.transcript_height()])
    head, sep, tail = body.partition(after)
    return tail.strip() if sep else ""


def test_heading_loses_its_hashes(ctx):
    """'## Plans' reads as a heading, not as punctuation."""
    ctx.scenario("text=##+Plans\\ndetails")
    s = ctx.spawn()
    s.submit("write a heading")
    s.wait_text("details")
    s.wait_turn_done()
    assert "Plans" in s.text() and "## Plans" not in s.text(), s.text()
    assert cell(s, "Plans").fg == CYAN
    assert cell(s, "details").fg == TEXT
    ctx.check_screen(s)


def test_bullets_become_bullets(ctx):
    """A list marker is drawn, and the item text stays plain."""
    ctx.scenario("text=-+one\\n-+two\\n\\n1.+first")
    s = ctx.spawn()
    s.submit("list it")
    s.wait_text("first")
    s.wait_turn_done()
    text = s.text()
    assert "\u2022 one" in text and "\u2022 two" in text, text
    assert "- one" not in text, text
    assert cell(s, "\u2022 one").fg == BLUE
    assert cell(s, "\u2022 one", 2).fg == TEXT
    assert "1. first" in text, text


def test_emphasis_is_styled_not_spelled(ctx):
    """'**loud**' is bold text, '`ls`' is monospace colour, markers are gone."""
    ctx.scenario("text=say+**loud**+and+`ls`+and+_soft_+here")
    s = ctx.spawn()
    s.submit("emphasise")
    s.wait_text("here")
    s.wait_turn_done()
    text = s.text()
    assert "say loud and ls and soft here" in text, text
    assert "*" not in text and "`" not in text, text
    assert cell(s, "loud").bold, "emphasis should be bold"
    assert cell(s, "ls ").fg == MONO
    assert cell(s, "soft").fg == MUTED


def test_underscores_inside_words_survive(ctx):
    """snake_case is a name, not emphasis, and arithmetic is not either."""
    ctx.scenario("text=call+md_line_end+when+2+*+3+*+4+is+done")
    s = ctx.spawn()
    s.submit("careful now")
    s.wait_text("is done")
    s.wait_turn_done()
    assert "call md_line_end when 2 * 3 * 4 is done" in s.text(), s.text()


def test_fenced_code_becomes_a_block(ctx):
    """The fences themselves go; what they held gets a block of its own."""
    ctx.scenario("text=here:\\n```c\\nint+main(void)\\n```\\ndone")
    s = ctx.spawn()
    s.submit("show me code")
    s.wait_text("done")
    s.wait_turn_done()
    text = s.text()
    assert "int main(void)" in text, text
    assert "```" not in text and "```c" not in text, text
    assert cell(s, "int main").bg == CODE_BG, "the code row carries a panel"
    assert cell(s, "done").bg is None
    ctx.check_screen(s)


def test_block_quote_and_rule(ctx):
    """A quote keeps a bar and reads muted; a thematic break is drawn."""
    ctx.scenario("text=>+quoted+wisdom\\n\\n---\\n\\nafter")
    s = ctx.spawn()
    s.submit("quote me")
    s.wait_text("after")
    s.wait_turn_done()
    text = s.text()
    assert "\u2502 quoted wisdom" in text, text
    assert "\u2500\u2500\u2500" in text, text
    assert "---" not in text, text
    assert cell(s, "quoted wisdom").fg == MUTED


def test_link_keeps_its_target(ctx):
    """A link shows its text, with the target beside it rather than dropped."""
    ctx.scenario("text=see+[the+docs](https://example.com/x)+now")
    s = ctx.spawn()
    s.submit("link me")
    s.wait_text("now")
    s.wait_turn_done()
    text = s.text()
    assert "see the docs (https://example.com/x) now" in text, text
    assert cell(s, "the docs").fg == BLUE


def test_submitted_user_messages_are_markdown_but_the_composer_is_not(ctx):
    """Formatting starts after submission and stays inside the user panel."""
    ctx.scenario("text=done")
    s = ctx.spawn()
    s.type("## My **request** uses `code`").sync()
    assert "## My **request** uses `code`" in s.composer_text(), s.composer_lines()
    assert cell(s, "request").fg == TEXT, "composer text stays literal"
    assert cell(s, "code").fg == TEXT, "composer text is not inline code"

    s.submit()
    s.wait_text("done")
    s.wait_turn_done()
    text = s.text()
    assert "## My" not in text and "**request**" not in text, text
    assert "My request uses code" in text, text
    assert cell(s, "My request").fg == CYAN
    assert cell(s, "request").bold
    assert cell(s, "request").bg == USER_BG
    assert cell(s, "code").fg == MONO
    assert cell(s, "code").bg == USER_BG


def test_tables_are_drawn_as_aligned_terminal_tables(ctx):
    """A GFM table gets borders, a header and column alignment, not pipes."""
    ctx.scenario(
        "text=Name+|+Score+|+State\\n"
        ":---+|+---:+|+:---:\\n"
        "Ada+|+7+|+ready\\n"
        "Linus+|+42+|+busy"
    )
    s = ctx.spawn()
    s.submit("show a table")
    s.wait_text("busy")
    s.wait_turn_done()
    text = s.text()
    assert "Name | Score" not in text and ":---" not in text, text
    assert "┌" in text and "┬" in text and "┐" in text, text
    assert "├" in text and "┼" in text and "┤" in text, text
    assert "└" in text and "┴" in text and "┘" in text, text
    assert "│ Name  │ Score │ State │" in text, text
    assert "│ Ada   │     7 │ ready │" in text, text
    assert "│ Linus │    42 │ busy  │" in text, text
    assert cell(s, "Name").bold, "table headers are bold"
    assert cell(s, "┌").fg == BLUE


def test_table_alignment_uses_terminal_cells_for_emoji(ctx):
    """Wide glyphs occupy terminal cells, not one column per UTF-8 byte."""
    ctx.scenario(
        "text=ID+|+Name+|+Status+|+Priority+|+Description\\n"
        "---:+|+:---+|+:---:+|+:---:+|+:---\\n"
        "001+|+Alpha+|+✅+Open+|+High+|+First+item\\n"
        "002+|+Beta+|+⏳+Pending+|+Medium+|+Waiting+for+review\\n"
        "003+|+Gamma+|+❌+Closed+|+Low+|+Completed+item"
    )
    s = ctx.spawn()
    s.submit("show a wide table")
    s.wait_text("Completed item")
    s.wait_turn_done()

    def columns(needle, borders):
        row = next(r for r in range(s.screen.rows)
                   if needle in s.screen.row_text(r))
        return [i for i, ch in enumerate(s.screen.buf.chars[row])
                if ch in borders]

    expected = columns("┌", "┌┬┐")
    assert columns("├", "├┼┤") == expected
    assert columns("└", "└┴┘") == expected
    for value in ("ID", "001", "002", "003"):
        assert columns(value, "│") == expected, s.text()
    assert "✅ Open" in s.text() and "⏳ Pending" in s.text(), s.text()


def test_table_syntax_inside_a_fence_stays_code(ctx):
    """Pipes and delimiter rows are data while a fenced block owns the line."""
    ctx.scenario("text=```text\\nA+|+B\\n---+|+---\\n1+|+2\\n```")
    s = ctx.spawn()
    s.submit("show literal table syntax")
    s.wait_text("1 | 2")
    s.wait_turn_done()
    text = s.text()
    assert "A | B" in text and "--- | ---" in text, text
    assert "┌" not in text and "└" not in text, text
    assert cell(s, "A | B").bg == CODE_BG


def test_common_gfm_extensions_are_formatted(ctx):
    """Task boxes, strikeout, tilde fences and autolinks have terminal shapes."""
    ctx.scenario(
        "text=-+[x]+shipped\\n-+[+]+queued\\n"
        "~~obsolete~~+and+<https://example.com>\\n"
        "~~~sh\\necho+ok\\n~~~"
    )
    s = ctx.spawn()
    s.submit("show extensions")
    s.wait_text("echo ok")
    s.wait_turn_done()
    text = s.text()
    assert "☑ shipped" in text and "☐ queued" in text, text
    assert "~~" not in text and "obsolete and https://example.com" in text, text
    assert "<https://example.com>" not in text, text
    assert "~~~" not in text, text
    assert cell(s, "echo ok").bg == CODE_BG
    assert cell(s, "https://example.com").fg == BLUE


def test_raw_toggles_formatting_off_and_back_on(ctx):
    """Raw shows the Markdown as written; toggling it back formats again."""
    ctx.scenario("text=#+Title\\n-+one")
    s = ctx.spawn()
    s.settings_toggle("Raw Markdown")

    s.submit("first")
    s.wait_text("- one")
    s.wait_turn_done()
    assert "# Title" in s.text(), s.text()

    s.settings_toggle("Raw Markdown")
    s.submit("second")
    s.wait_turn_done()
    assert "\u2022 one" in s.text(), s.text()


def test_raw_repaints_the_reply_already_on_screen(ctx):
    """Raw applies to the transcript, not only to the next reply."""
    ctx.scenario("text=#+Title\\n-+one")
    s = ctx.spawn()
    s.submit("first")
    s.wait_text("\u2022 one")
    s.wait_turn_done()
    assert "Title" in s.text() and "# Title" not in s.text(), s.text()

    s.settings_toggle("Raw Markdown")
    text = s.text()
    assert "# Title" in text and "- one" in text, text
    assert "\u2022 one" not in text, text

    s.settings_toggle("Raw Markdown")
    text = s.text()
    assert "\u2022 one" in text and "# Title" not in text, text


def test_streaming_stays_incremental(ctx):
    """Rendering is per delta: a long line shows up as it arrives."""
    ctx.scenario("words=60,chunk=2,delay=0.03")
    s = ctx.spawn()
    s.submit("stream it")
    s.wait_for(lambda t: len(transcript(s, "stream it")) > 20, "partial reply")
    mid = transcript(s, "stream it")
    s.wait_turn_done()
    assert len(transcript(s, "stream it")) > len(mid), "more should have landed"


def test_resumed_session_is_rendered_too(ctx):
    """Replaying a session paints the same shapes the live turn did."""
    ctx.scenario("text=#+Recap\\n-+point")
    s = ctx.spawn()
    s.submit("summarise")
    s.wait_text("\u2022 point")
    s.wait_turn_done()
    s.submit("/clear")
    s.wait_gone("Recap")
    s.submit("/resume")
    s.wait_text("pick a session")
    s.key("enter")
    s.wait_text("\u2022 point")
    text = s.text()
    assert "\u2022 point" in text and "# Recap" not in text, text
