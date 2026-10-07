"""A reply with no text: the model answered in its reasoning, which the user
never sees, so the agent asks once more for the reply itself."""

NOTE = "The user never sees your reasoning"


def last_message_text(message) -> str:
    content = message["content"]
    if isinstance(content, str):
        return content
    return "".join(block.get("text", "") for block in content)


def test_a_reply_with_no_text_is_asked_for_again(ctx):
    """Reasoning with no reply gets one more round, and the user sees why."""
    ctx.scenario("blank_times=1,reasoning=listing+the+items,"
                 "text=the+items+themselves")
    s = ctx.spawn()
    s.submit("what is messy?")
    s.wait_text("the items themselves")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 2, ctx.mock.requests
    messages = ctx.mock.requests[-1]["messages"]
    assert messages[-2]["role"] == "assistant", messages
    assert messages[-1]["role"] == "user", messages
    assert NOTE in last_message_text(messages[-1]), messages
    assert "[note to the model: " in s.text(), s.text()
    ctx.check_screen(s)


def test_a_reply_with_text_is_not_asked_for_again(ctx):
    """Reasoning followed by a reply is the normal case: one request."""
    ctx.scenario("reasoning=thinking+it+over,text=here+it+is")
    s = ctx.spawn()
    s.submit("what is messy?")
    s.wait_text("here it is")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests
    assert "note to the model" not in s.text(), s.text()


def test_the_reply_is_asked_for_once_per_turn(ctx):
    """A second empty reply ends the turn instead of looping."""
    ctx.scenario("reasoning=still+thinking,text=")
    s = ctx.spawn()
    s.submit("what is messy?")
    s.wait_text("[note to the model: ")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_the_note_rides_as_a_user_block_to_anthropic(ctx):
    """The signed thinking stays on the empty reply and the note follows it."""
    ctx.scenario("blank_times=1,reasoning=listing+the+items,"
                 "text=the+items+themselves")
    s = ctx.spawn(ARQAN_API="anthropic")
    s.submit("what is messy?")
    s.wait_text("the items themselves")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["user", "assistant", "user"], \
        messages
    assert messages[1]["content"] == [{
        "type": "thinking",
        "thinking": "listing the items",
        "signature": "sig_mock",
    }], messages[1]
    assert NOTE in last_message_text(messages[2]), messages[2]


def test_the_note_survives_a_session_resume(ctx):
    """A resumed session shows the note as a note and sends it again."""
    ctx.scenario("blank_times=1,reasoning=listing+the+items,"
                 "text=the+items+themselves")
    first = ctx.spawn()
    first.submit("what is messy?")
    first.wait_text("the items themselves")
    first.wait_turn_done()
    first.submit("/exit")
    first.wait_exit()

    again = ctx.spawn()
    again.submit("/resume")
    again.wait_status("pick a session")
    again.key("enter")
    again.wait_text("the items themselves")
    assert "[note to the model: " in again.text(), again.text()

    ctx.scenario("text=second+answer")
    again.submit("and now?")
    again.wait_text("second answer")
    again.wait_turn_done()
    roles = [m["role"] for m in ctx.mock.requests[-1]["messages"]]
    assert roles == ["system", "user", "assistant", "user", "assistant",
                     "user"], roles
    note = ctx.mock.requests[-1]["messages"][3]
    assert NOTE in last_message_text(note), note
