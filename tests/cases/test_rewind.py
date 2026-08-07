"""Rewinding the conversation: Escape twice, or /rewind, to edit an earlier turn."""

ARM = "Press Escape again to edit previous message"


def turn(ctx, s, prompt, reply):
    ctx.scenario("text=" + reply.replace(" ", "+"))
    s.submit(prompt)
    s.wait_text(reply)
    s.wait_turn_done()


def test_escape_arms_the_rewind(ctx):
    """The first Escape on an idle composer asks for a second one."""
    s = ctx.spawn()
    turn(ctx, s, "first question", "first answer")
    s.key("esc").sync()
    assert ARM in s.text(), s.text()
    assert s.composer_text() == "", s.composer_lines()


def test_a_keystroke_disarms_it(ctx):
    """Anything but Escape retires the notice and the next Escape only re-arms."""
    s = ctx.spawn()
    turn(ctx, s, "first question", "first answer")
    s.key("esc").sync()
    s.type("x").sync()
    assert ARM not in s.text(), s.text()
    s.key("esc").sync()
    assert ARM in s.text(), "one Escape never opens the picker"
    assert s.composer_text() == "x", s.composer_lines()


def test_second_escape_opens_the_picker(ctx):
    """The picker lists the user turns in transcript order, oldest at the top."""
    s = ctx.spawn()
    turn(ctx, s, "first question", "first answer")
    turn(ctx, s, "second question", "second answer")
    s.key("esc").sync()
    s.key("esc")
    s.wait_status("rewind to a message")
    rows = s.screen.lines()
    older = [i for i, row in enumerate(rows) if "first question" in row]
    newer = [i for i, row in enumerate(rows) if "second question" in row]
    assert older and newer, rows
    assert older[0] < newer[0], rows


def test_the_newest_message_is_selected(ctx):
    """The list opens on the turn nearest the composer, its last row."""
    s = ctx.spawn()
    turn(ctx, s, "first question", "first answer")
    turn(ctx, s, "second question", "second answer")
    s.key("esc").sync()
    s.key("esc")
    s.wait_status("rewind to a message")
    rows = [row for row in s.screen.lines() if "question" in row]
    assert rows[-1].strip().startswith("\u203a second question"), rows
    assert "\u203a" not in rows[0], rows


def test_rewinding_reloads_the_message_and_trims_the_transcript(ctx):
    """The chosen turn returns to the composer; everything from it on is gone."""
    s = ctx.spawn()
    turn(ctx, s, "first question", "first answer")
    turn(ctx, s, "second question", "second answer")
    s.key("esc").sync()
    s.key("esc")
    s.wait_status("rewind to a message")
    s.key("enter")            # the newest entry
    s.wait_gone("second answer")
    s.wait_text("first answer")   # the trimmed conversation is repainted
    s.settle()
    assert s.composer_text() == "second question", s.composer_lines()
    transcript = "\n".join(s.screen.lines()[: s.transcript_height()])
    assert "first question" in transcript, transcript
    assert "second question" not in transcript, transcript


def test_the_next_turn_continues_from_there(ctx):
    """What was rewound past never reaches the provider again."""
    s = ctx.spawn()
    turn(ctx, s, "first question", "first answer")
    turn(ctx, s, "second question", "second answer")
    s.submit("/rewind")
    s.wait_status("rewind to a message")
    s.key("enter")
    s.wait_gone("second answer")
    ctx.scenario("text=fresh+answer")
    s.key("enter")            # send the reloaded message
    s.wait_turn_done()
    sent = [m["content"] for m in ctx.mock.requests[-1]["messages"]]
    assert sent[-1] == "second question", sent
    assert "second answer" not in sent, sent
    assert "first answer" in sent, sent


def test_picking_an_older_message_drops_everything_after_it(ctx):
    """Rewinding two turns back leaves only what came before."""
    s = ctx.spawn()
    turn(ctx, s, "first question", "first answer")
    turn(ctx, s, "second question", "second answer")
    s.submit("/rewind")
    s.wait_status("rewind to a message")
    s.key("up").sync()          # Up goes further back, as it does everywhere
    s.key("enter")
    s.wait_gone("first answer")
    assert s.composer_text() == "first question", s.composer_lines()
    assert "second answer" not in s.text(), s.text()


def test_cancelling_keeps_the_conversation_and_the_draft(ctx):
    """Esc out of the picker changes nothing, typed text included."""
    s = ctx.spawn()
    turn(ctx, s, "first question", "first answer")
    s.type("draft kept").sync()
    s.key("esc").sync()
    s.key("esc")
    s.wait_status("rewind to a message")
    s.key("esc")
    s.wait_status("ready")
    assert s.composer_text() == "draft kept", s.composer_lines()
    assert "first answer" in s.text(), s.text()


def test_search_keeps_the_newest_match_selected(ctx):
    """Past ten entries the picker filters, and Enter still takes the newest."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    for i in range(1, 12):
        s.submit(f"message {i:02d}")
        s.wait_turn_done()
    s.submit("/rewind")
    s.wait_status("rewind to a message")
    assert "search:" in s.text(), "a long list takes the keyboard"
    s.type("message 1").sync()  # matches 01, 10 and 11
    s.key("enter")
    s.wait_for(lambda t: s.composer_text() == "message 11", "the newest match")


def test_nothing_to_rewind_to_answers_in_the_notice(ctx):
    """A conversation with no user turn has no earlier message to edit."""
    s = ctx.spawn()
    s.key("esc").sync()
    s.key("esc")
    s.wait_text("no message to go back to")
