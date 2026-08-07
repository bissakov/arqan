"""Replies are Markdown, and the transcript renders them (unless '/raw')."""

TEXT = 253      # S_TEXT
MUTED = 245     # S_MUTED
CYAN = 81       # S_CYAN, headings
BLUE = 75       # S_BLUE, bullets and other markers
MONO = 180      # S_MONO, inline code
CODE_BG = 235   # S_CODE_BG, fenced blocks


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


def test_raw_toggles_formatting_off_and_back_on(ctx):
    """'/raw' shows the Markdown as written; running it again formats again."""
    ctx.scenario("text=#+Title\\n-+one")
    s = ctx.spawn()
    s.submit("/raw")
    s.wait_text("raw: replies are shown as the model wrote them")

    s.submit("first")
    s.wait_text("- one")
    s.wait_turn_done()
    assert "# Title" in s.text(), s.text()

    s.submit("/raw")
    s.wait_text("raw: off, Markdown is formatted")
    s.submit("second")
    s.wait_turn_done()
    assert "\u2022 one" in s.text(), s.text()


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
