"""Transcript messages are Markdown, and the composer remains literal."""

from tests.mockprovider import Scenario

TEXT = 253      # S_TEXT
MUTED = 245     # S_MUTED
CYAN = 81       # S_CYAN, headings
BLUE = 75       # S_BLUE, bullets and other markers
MONO = 180      # S_MONO, inline code
CODE_BG = 235   # S_CODE_BG, fenced blocks
USER_BG = 238   # S_USER_BG, submitted user messages
USER_CODE_BG = 236  # S_USER_CODE_BG, a fence inside a user message
RULE = "\u258c"     # the rule down the left of a user turn


def cell(s, needle: str, offset: int = 0):
    """The attributes of a glyph of `needle` as painted on screen."""
    for r in range(s.screen.rows):
        row = s.screen.row_text(r)
        if needle in row:
            return s.screen.attr_at(r, row.index(needle) + offset)
    raise AssertionError(f"{needle!r} is not on screen:\n{s.text()}")


def row_of(s, needle: str) -> int:
    """The screen row `needle` is painted on."""
    for r in range(s.screen.rows):
        if needle in s.screen.row_text(r):
            return r
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


def test_newest_emphasis_survives_style_capacity(ctx):
    """Evicting old spans keeps the newest bounded set ordered and styled."""
    text = " ".join(f"**word{i:04d}**" for i in range(4200))
    ctx.scenario(Scenario(text=text, chunk=512))
    s = ctx.spawn()
    s.submit("format many words")
    s.wait_turn_done(timeout=60.0)
    assert "word4199" in s.text(), s.text()
    assert cell(s, "word4199").bold, "the newest retained span lost its style"


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


def test_a_user_turn_is_marked_on_every_row(ctx):
    """Formatted content in a user turn still reads as the reader's, not the
    model's: the panel covers a fence, and every row carries the rule."""
    ctx.scenario("text=here:\\n```c\\nint+theirs+=+2;\\n```\\ndone")
    s = ctx.spawn()
    s.paste("mine:\n```c\nint mine = 1;\n```\n\n| a | b |\n| --- | --- |\n| 1 | 2 |")
    s.submit()
    s.wait_text("done")
    s.wait_turn_done()

    # The user's fence stays inside the panel rather than taking the slab the
    # model's fence is painted with.
    assert cell(s, "int mine").bg == USER_CODE_BG
    assert cell(s, "int theirs").bg == CODE_BG
    assert cell(s, "\u2502 1 \u2502 2 \u2502").bg == USER_BG

    first, last = row_of(s, "mine:"), row_of(s, "\u2514\u2500")
    for r in range(first, last + 1):
        row = s.screen.row_text(r)
        assert row.startswith(RULE), f"row {r} is unmarked: {row!r}"
    assert not s.screen.row_text(row_of(s, "int theirs")).startswith(RULE)
    assert not s.screen.row_text(row_of(s, "done")).startswith(RULE)
    ctx.check_screen(s)


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


def wide_table_scenario() -> str:
    return (
        "text=Lorem+ipsum+|+Dolor+sit+amet+|+Consectetur+adipiscing\\n"
        "---+|+---+|+---\\n"
        "src/lorem.c+|+Elit+sed+do+eiusmod+tempor+incididunt+ut+labore"
        "+et+dolore+magna+aliqua+|+No+wire+format\\n"
        "src/ipsum.c+|+Quis+nostrud+exercitation+ullamco+laboris+nisi"
        "+ut+aliquip+ex+ea+commodo+|+Yes\\n"
    )


def table_rows(s):
    """Screen rows carrying a table border or a table cell."""
    rows = []
    for r in range(s.transcript_height()):
        text = s.screen.row_text(r).rstrip()
        if text.lstrip().startswith(("\u250c", "\u251c", "\u2514", "\u2502")):
            rows.append(text)
    return rows


def test_a_wide_table_is_fitted_to_the_transcript(ctx):
    """Columns shrink and cell text wraps rather than running off the edge."""
    ctx.scenario(wide_table_scenario())
    s = ctx.spawn()
    s.submit("show a wide table")
    s.wait_text("Yes")
    s.wait_turn_done()
    rows = table_rows(s)
    assert rows, s.text()
    for row in rows:
        assert len(row) <= 80, f"{row!r} overflows 80 columns"
        assert row.endswith(("\u2510", "\u2502", "\u2524", "\u2518")), row
    text = "\n".join(rows)
    assert "incididunt" in text and "aliqua" in text, text
    assert "eiusmod tempor" in text, "words stay whole where they fit"
    assert len(rows) > 6, "wrapped cells take more rows than their cells"


def test_a_narrow_terminal_wraps_table_cells_further(ctx):
    """The same table keeps its frame when the terminal has fewer columns."""
    ctx.scenario(wide_table_scenario())
    s = ctx.spawn(cols=48, rows=40)
    s.submit("show a wide table")
    s.wait_text("Yes")
    s.wait_turn_done()
    rows = table_rows(s)
    assert rows, s.text()
    for row in rows:
        assert len(row) <= 48, f"{row!r} overflows 48 columns"
        assert row.endswith(("\u2510", "\u2502", "\u2524", "\u2518")), row


def test_a_long_word_is_split_when_no_column_can_hold_it(ctx):
    """Character wrapping is the fallback a browser falls back to."""
    ctx.scenario(
        "text=A+|+B\\n---+|+---\\n"
        "x+|+" + "z" * 120 + "\\n"
    )
    s = ctx.spawn()
    s.submit("show a table")
    s.wait_turn_done()
    rows = [r for r in table_rows(s) if "z" in r]
    assert len(rows) > 1, s.text()
    for row in rows:
        assert len(row) <= 80, f"{row!r} overflows 80 columns"
    assert sum(r.count("z") for r in rows) == 120, s.text()


def test_inline_markup_inside_a_cell_is_styled(ctx):
    """A cell is Markdown too: markers become styles and cost no columns."""
    ctx.scenario(
        "text=File+|+Ship\\n---+|+---\\n"
        "`src/lorem.c`+|+**Yes**\\n"
    )
    s = ctx.spawn()
    s.submit("show a table")
    s.wait_text("Yes")
    s.wait_turn_done()
    text = s.text()
    assert "`" not in text and "**" not in text, text
    assert "\u2502 src/lorem.c \u2502 Yes  \u2502" in text, text
    assert cell(s, "src/lorem.c").fg == MONO
    assert cell(s, "Yes").bold


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
    s.settings_toggle("Display raw")

    s.submit("first")
    s.wait_text("- one")
    s.wait_turn_done()
    assert "# Title" in s.text(), s.text()

    s.settings_toggle("Display raw")
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

    s.settings_toggle("Display raw")
    text = s.text()
    assert "# Title" in text and "- one" in text, text
    assert "\u2022 one" not in text, text

    s.settings_toggle("Display raw")
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


def long_row_scenario(lead: str) -> str:
    """A table whose body rows are longer than the line hold buffer.

    `lead` is the row prefix: a leading pipe or nothing. The trailing pipe
    follows it, so both delimited forms of a table are covered.
    """
    body = (
        "Житель+пишет+свободным+текстом+течет+батарея+и+модель+сама+ставит+"
        "категорию+сантехник+плюс+приоритет+высокий+плюс+выносит+квартиру+"
        "адрес+и+срочность+что+убирает+ошибку+выбора+категории+жителем"
    )
    tail = "+|" if lead else ""
    return (
        f"text={lead}Куда+|+Что+делает+|+Модель{tail}\\n"
        f"{lead}---+|+---+|+---{tail}\\n"
        f"{lead}Категоризация+|+{body}+|+Любая{tail}\\n"
        f"{lead}Эскалация+|+{body}+|+Любая{tail}\\n"
    )


def check_long_rows(s):
    """Every body row is drawn inside the box and none leaks out raw."""
    s.submit("show a table")
    s.wait_turn_done()
    text = s.text()
    assert "|" not in text.split("show a table", 1)[1], text
    rows = table_rows(s)
    assert any("Категоризация" in r for r in rows), text
    assert any("Эскалация" in r for r in rows), text
    assert sum("Любая" in r for r in rows) == 2, text


def test_a_row_longer_than_the_line_buffer_stays_in_the_table(ctx):
    """A wide row is held whole: it must not break the table open."""
    ctx.scenario(long_row_scenario("|+"))
    s = ctx.spawn(rows=40)
    check_long_rows(s)


def test_a_wide_row_without_a_leading_pipe_stays_in_the_table(ctx):
    """The pipe-less form of the same table renders the same way."""
    ctx.scenario(long_row_scenario(""))
    s = ctx.spawn(rows=40)
    check_long_rows(s)


def interior_rules(s):
    """Rule rows drawn between body rows, header rule excluded."""
    rows = table_rows(s)
    rules = [r for r in rows if r.lstrip().startswith("\u251c")]
    return rules[1:]


def test_wrapped_rows_are_separated_by_a_rule(ctx):
    """A row spanning several lines needs a boundary to end on."""
    ctx.scenario(wide_table_scenario())
    s = ctx.spawn(cols=48, rows=40)
    s.submit("show a wide table")
    s.wait_text("Yes")
    s.wait_turn_done()
    assert len(interior_rules(s)) == 1, s.text()


def test_single_line_rows_stay_tight(ctx):
    """No cell wraps, so the table keeps its compact frame."""
    ctx.scenario(
        "text=Name+|+Score\\n---+|+---\\n"
        "Ada+|+9\\nGrace+|+8\\nAlan+|+7\\n"
    )
    s = ctx.spawn()
    s.submit("show a table")
    s.wait_text("Alan")
    s.wait_turn_done()
    assert interior_rules(s) == [], s.text()


def test_unclosed_fence_does_not_leak_into_the_next_message(ctx):
    """A message that ends mid-fence must not make the next one code."""
    ctx.scenario("text=alpha:\\n```c\\nint+x;")
    s = ctx.spawn()
    s.submit("show me code")
    s.wait_text("int x;")
    s.wait_turn_done()
    assert cell(s, "int x;").bg == CODE_BG, "the fence opened"

    ctx.scenario("text=omega+prose")
    s.submit("now just talk")
    s.wait_text("omega prose")
    s.wait_turn_done()
    assert cell(s, "omega prose").bg is None, s.text()
