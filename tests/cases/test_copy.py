"""'/copy': the last reply on the clipboard, as the Markdown the model wrote."""

MARKDOWN = "# Title\\n\\n- one\\n- two\\n\\n`code`"
MARKDOWN_TEXT = "# Title\n\n- one\n- two\n\n`code`"


def test_copy_puts_the_last_reply_on_the_clipboard(ctx):
    """The clipboard holds the reply's source, newlines and markup included."""
    ctx.scenario(f"text={MARKDOWN}")
    s = ctx.spawn()
    s.submit("write some markdown")
    s.wait_text("two")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert s.screen.clipboard == MARKDOWN_TEXT, repr(s.screen.clipboard)


def test_copy_takes_the_source_not_the_rendered_rows(ctx):
    """A reply wider than the screen is copied unwrapped."""
    long_line = "+".join(f"word{i:02d}" for i in range(40))
    ctx.scenario(f"text={long_line}")
    s = ctx.spawn()
    s.submit("say a long line")
    s.wait_text("word39")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert s.screen.clipboard == long_line.replace("+", " "), repr(
        s.screen.clipboard
    )


def test_copy_skips_tool_calls(ctx):
    """Tool-call slots carry JSON arguments, so the answer is what is copied."""
    ctx.write_file("notes.txt", "hello from the file\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    s = ctx.spawn()
    s.submit("read the notes")
    s.wait_text("I read it")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert s.screen.clipboard == "I read it", repr(s.screen.clipboard)


def test_copy_without_a_reply_answers_in_the_popup_slot(ctx):
    """Nothing said yet leaves the view as it was and says so above the composer."""
    s = ctx.spawn()
    assert "| |_| | (_) |" in s.text(), "the welcome art starts on screen"
    s.submit("/copy")
    s.wait_text("no response to copy")
    rows = s.screen.lines()
    assert "no response to copy" in rows[s.screen.rows - 6], rows[s.screen.rows - 8 :]
    assert "| |_| | (_) |" in s.text(), s.text()
    assert s.screen.clipboard is None or s.screen.clipboard == "", (
        repr(s.screen.clipboard)
    )


def test_copy_acknowledges_on_the_status_line(ctx):
    """The status line reports the copy, as it does for a drag-select."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert "copied" in s.status_line(), s.status_line()


def test_copy_is_not_part_of_the_conversation(ctx):
    """The command never reaches the provider or the transcript."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/copy")
    s.wait_text("copied the last response")
    rows = s.screen.lines()
    transcript = "\n".join(rows[: s.screen.rows - 6])
    assert "/copy" not in transcript, s.text()
    sent = [m["content"] for r in ctx.mock.requests for m in r.get("messages", [])]
    assert not any("/copy" in str(c) for c in sent), sent


def test_copy_after_clear_has_nothing_to_copy(ctx):
    """'/clear' drops the conversation, so there is no last reply left."""
    ctx.scenario("text=alpha+beta")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("alpha beta")
    s.wait_turn_done()
    s.submit("/clear")
    s.wait_gone("alpha beta")
    s.submit("/copy")
    s.wait_text("no response to copy")
