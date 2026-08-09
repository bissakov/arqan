"""Thinking models: reasoning deltas are shown, muted, and never sent back."""

MUTED = 245  # S_MUTED
TEXT = 253   # S_TEXT
CYAN = 81    # S_CYAN
BLUE = 75    # S_BLUE
MONO = 180   # S_MONO


def row_of(s, needle: str) -> int:
    term = s.screen
    for r in range(term.rows):
        if needle in term.row_text(r):
            return r
    raise AssertionError(f"{needle!r} is not on screen:\n{s.text()}")


def fg_of(s, needle: str) -> int:
    r = row_of(s, needle)
    col = s.screen.row_text(r).index(needle[0])
    return s.screen.attr_at(r, col).fg


def attr_of(s, needle: str):
    r = row_of(s, needle)
    col = s.screen.row_text(r).index(needle)
    return s.screen.attr_at(r, col)


def test_reasoning_is_visible(ctx):
    """A reasoning trace lands in the transcript above the reply."""
    ctx.scenario("reasoning=let+me+count+the+rabbits,text=four+rabbits")
    s = ctx.spawn()
    s.submit("how many?")
    s.wait_text("four rabbits")
    s.wait_turn_done()
    assert "let me count the rabbits" in s.text()
    assert row_of(s, "let me count") < row_of(s, "four rabbits")
    ctx.check_screen(s)


def test_reasoning_is_muted_and_reply_is_not(ctx):
    """Thinking reads grey, the answer keeps the transcript's text colour."""
    ctx.scenario("reasoning=thinking+hard,text=the+answer")
    s = ctx.spawn()
    s.submit("go")
    s.wait_text("the answer")
    s.wait_turn_done()
    assert fg_of(s, "thinking hard") == MUTED
    assert fg_of(s, "the answer") == TEXT


def test_reasoning_markdown_is_formatted_with_a_muted_base(ctx):
    """Thinking gets Markdown shapes while ordinary prose remains grey."""
    ctx.scenario(
        "reasoning=##+Approach\\n-+check+**facts**+with+`code`,"
        "text=**final**"
    )
    s = ctx.spawn()
    s.submit("go")
    s.wait_text("final")
    s.wait_turn_done()
    text = s.text()
    assert "## Approach" not in text and "**facts**" not in text, text
    assert "Approach" in text and "• check facts with code" in text, text
    assert attr_of(s, "Approach").fg == CYAN
    assert attr_of(s, "•").fg == BLUE
    assert attr_of(s, "check").fg == MUTED
    assert attr_of(s, "facts").bold
    assert attr_of(s, "code").fg == MONO
    assert attr_of(s, "final").bold, "reply Markdown starts a fresh stream"


def test_openrouter_reasoning_field(ctx):
    """The 'reasoning' spelling is understood like 'reasoning_content'."""
    ctx.scenario(
        "reasoning=via+openrouter,reasoning_field=reasoning,text=done"
    )
    s = ctx.spawn()
    s.submit("go")
    s.wait_text("done")
    s.wait_turn_done()
    assert fg_of(s, "via openrouter") == MUTED


def test_reasoning_is_not_sent_back(ctx):
    """The next request carries the reply only: no thinking trace is echoed."""
    ctx.scenario("reasoning=secret+scratchpad,text=hello")
    s = ctx.spawn()
    s.submit("first")
    s.wait_text("hello")
    s.wait_turn_done()
    s.submit("second")
    s.wait_turn_done()

    req = ctx.mock.requests[-1]
    assistant = [m for m in req["messages"] if m["role"] == "assistant"]
    assert assistant, req["messages"]
    assert all("secret scratchpad" not in m["content"] for m in assistant)


def test_a_reasoning_delta_larger_than_the_line_buffer_arrives(ctx):
    """A thinking trace sent as one huge event is shown, not dropped."""
    trace = "+".join(f"thought{i}" for i in range(4000))
    ctx.scenario(f"reasoning={trace},chunk=4000,text=finally+the+answer")
    s = ctx.spawn()
    s.submit("go")
    s.wait_text("finally the answer")
    s.wait_turn_done()
    # The tail of the trace is the last thing above the reply.
    assert "thought3999" in s.text(), s.text()
