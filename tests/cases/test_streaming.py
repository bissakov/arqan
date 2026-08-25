"""A full turn against the dummy provider: request, SSE deltas, transcript."""

import json

RULE = "\u258c"     # the rule down the left of a user turn


def test_simple_turn(ctx):
    """One turn renders the user box, the reply, and returns to ready."""
    ctx.scenario("text=Hello+from+the+mock+provider.,usage=120/8")
    s = ctx.spawn()
    s.submit("say hi")
    s.wait_text("Hello from the mock provider.")
    s.wait_turn_done()
    ctx.check_screen(s)

    assert "say hi" in s.text(), "the user turn stays in the transcript"
    assert len(ctx.mock.requests) == 1


def test_request_shape(ctx):
    """The request carries model, system prompt, the user turn and the tools."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("what is 2+2?")
    s.wait_text("ok")
    s.wait_turn_done()

    req = ctx.mock.requests[-1]
    assert req["model"] == "mock-model"
    assert req["stream"] is True
    assert req["stream_options"] == {"include_usage": True}
    roles = [m["role"] for m in req["messages"]]
    assert roles == ["system", "user"], roles
    assert req["messages"][0]["content"] == "You are a test fixture."
    assert req["messages"][1]["content"] == "what is 2+2?"
    names = sorted(t["function"]["name"] for t in req["tools"])
    assert names == [
        "ask_user", "bash", "find", "grep", "internet_search", "job",
        "page_fetch", "patch", "read", "task", "todo", "write"
    ], names


def test_usage_updates_context_counter(ctx):
    """The status line shows the context the request carried, which is the
    prompt the provider counted and not what the turn was billed."""
    ctx.scenario("text=counted,usage=1234/66")
    s = ctx.spawn()
    s.submit("count me")
    s.wait_text("counted")
    s.wait_for(lambda t: s.status_field(-2) not in ("-", ""), "the context")
    field = s.status_field(-2)
    assert field.lstrip("~") == "1k", s.status_line()


def test_context_survives_an_interrupt_once_usage_was_heard(ctx):
    """Usage heard mid-stream reaches the status line even if the turn ends
    early: an interrupt cannot take back the context the request carried, and
    what streamed before it is context the next request carries."""
    ctx.scenario("words=400,chunk=1,delay=0.02,usage_first=1,usage=5000/200")
    s = ctx.spawn()
    s.submit("go on")
    # The counter precedes Permissions: while the turn runs the status line
    # carries no state word.
    s.wait_for(lambda t: s.status_field(-2) == "5k", "context counter")
    s.key("ctrl-c")
    s.wait_text("[interrupted]")
    s.wait_turn_done()
    # The interrupted reply does not reach the conversation, so the
    # measurement still describes it exactly.
    assert s.status_field(-2) == "5k", s.status_line()


def test_spinner_says_thinking_while_streaming(ctx):
    """The spinner row reads 'thinking' until the stream finishes, and the
    status line leaves the word to it."""
    ctx.scenario("words=30,chunk=1,delay=0.05,first_delay=0.2")
    s = ctx.spawn()
    s.submit("slow down")
    s.wait_activity("thinking")
    assert "thinking" not in s.status_line(), s.status_line()
    s.wait_turn_done()
    assert s.activity() is None
    assert s.status_kind() == "ready", s.status_line()


def reply_text(s, after="stream it"):
    """Transcript text below the user's message, chrome excluded.

    The status line and composer change independently of the stream, so a
    length comparison has to look at the transcript alone. Before the user
    turn is painted there is no reply yet: an empty string, not the whole
    screen.
    """
    body = "\n".join(s.screen.lines()[: s.transcript_height()])
    head, sep, tail = body.partition(after)
    return tail.strip() if sep else ""


def test_streaming_is_incremental(ctx):
    """Deltas appear as they arrive rather than all at the end."""
    ctx.scenario("words=40,chunk=2,delay=0.03")
    s = ctx.spawn()
    s.submit("stream it")

    s.wait_for(lambda t: len(reply_text(s)) > 20, "partial assistant output")
    mid = reply_text(s)
    s.wait_turn_done()
    assert len(reply_text(s)) > len(mid), "more text should have arrived"


def test_fast_multiline_stream_finishes_without_painting_every_line(ctx):
    """A burst is throttled by frame rate even when every delta ends a line."""
    ctx.scenario("text=" + "+".join(f"line-{i}\\n" for i in range(600))
                 + ",chunk=1")
    s = ctx.spawn()
    s.submit("stream lines")
    s.wait_turn_done(timeout=5)
    assert "line-599" in s.text(), s.text()


def test_done_ends_a_stream_even_when_http_stays_open(ctx):
    """The OpenAI sentinel ends a turn without waiting for a broken server."""
    ctx.scenario("text=finished,keep_open=1")
    s = ctx.spawn()
    s.submit("do not wait for EOF")
    s.wait_text("finished")
    s.wait_turn_done(timeout=2)
    assert "[provider error" not in s.text(), s.text()


def test_multiline_prompt_is_sent_verbatim(ctx):
    """A composed multi-line message reaches the provider with its newline."""
    ctx.scenario("text=got+it")
    s = ctx.spawn()
    s.type("first").sync()
    s.key("newline").sync()
    s.type("second").sync()
    s.submit()
    s.wait_text("got it")
    s.wait_turn_done()
    sent = ctx.mock.requests[-1]["messages"][-1]["content"]
    assert sent == "first\nsecond", repr(sent)


def test_conversation_is_cumulative(ctx):
    """The second turn replays the first, including the assistant reply."""
    ctx.scenario("text=first+answer")
    s = ctx.spawn()
    s.submit("one")
    s.wait_text("first answer")
    s.wait_turn_done()

    ctx.scenario("text=second+answer")
    s.submit("two")
    s.wait_text("second answer")
    s.wait_turn_done()

    roles = [m["role"] for m in ctx.mock.requests[-1]["messages"]]
    assert roles == ["system", "user", "assistant", "user"], roles
    contents = [m["content"] for m in ctx.mock.requests[-1]["messages"]]
    assert contents[2] == "first answer", contents
    assert contents[3] == "two", contents


def test_reply_keeps_one_blank_row_before_the_next_turn(ctx):
    """A reply and the user box that follows it are one blank row apart."""
    ctx.scenario("text=first+answer")
    s = ctx.spawn()
    s.submit("one")
    s.wait_text("first answer")
    s.wait_turn_done()

    ctx.scenario("text=second+answer")
    s.submit("two")
    s.wait_text("second answer")
    s.wait_turn_done()

    lines = s.screen.lines()
    reply = s.screen.find_row("first answer")
    box = s.screen.find_row("two")
    # One air row, then the box's own top padding row: the padding carries no
    # text either, but it belongs to the turn and so carries its rule.
    empty = [i for i in range(reply + 1, box) if not lines[i].strip(RULE)]
    assert len(empty) == 2, s.screen.snapshot()
    assert not lines[reply + 1], s.screen.snapshot()
    assert lines[box - 1] == RULE, s.screen.snapshot()
    assert box - reply == 3, s.screen.snapshot()


def test_long_output_scrolls_and_shows_a_scrollbar(ctx):
    """A reply taller than the viewport pins the transcript to its bottom."""
    ctx.scenario("words=400,paragraphs=4,chunk=8")
    s = ctx.spawn()
    s.submit("write a lot")
    s.wait_turn_done()
    bar = s.scrollbar()
    assert "\u2503" in bar, f"expected a scrollbar thumb, got {bar}"
    assert "\u2502" in bar, "expected an unfilled scrollbar track"


def test_a_delta_larger_than_the_line_buffer_is_not_lost(ctx):
    """A provider that sends the reply in one huge event is still rendered.

    The event is accumulated whole; clipping it to a fixed buffer would hand
    the parser something that is not JSON and the turn would say nothing.
    """
    ctx.scenario("words=4000,chunk=4000")
    s = ctx.spawn()
    s.submit("all at once")
    s.wait_turn_done()
    # The reply is taller than the viewport, so the user turn it followed has
    # scrolled off: what is on screen is the reply.
    body = s.screen.lines()[: s.transcript_height()]
    assert any(row.strip() for row in body), s.text()

    ctx.scenario("text=done")
    s.submit("again")
    s.wait_text("done")
    s.wait_turn_done()
    reply = [m["content"] for m in ctx.mock.requests[-1]["messages"]
             if m["role"] == "assistant"][0]
    assert len(reply) > 20000, len(reply)


def test_provider_error_is_surfaced(ctx):
    """An HTTP error becomes a bracketed notice and the UI stays usable."""
    ctx.scenario("status=500")
    s = ctx.spawn()
    s.submit("please fail")
    s.wait_text("[provider error: HTTP 500]")
    s.wait_turn_done()
    ctx.check_screen(s)
    # the composer still accepts input afterwards
    s.type("still alive").sync()
    assert s.composer_text() == "still alive", s.composer_lines()


def test_unreachable_provider_is_surfaced(ctx):
    """A dead endpoint reports a request failure instead of hanging."""
    s = ctx.spawn(ARQAN_BASE_URL="http://127.0.0.1:1/v1")
    s.submit("nobody home")
    s.wait_text("[provider error:")
    s.wait_turn_done()


def test_typing_while_busy_is_kept(ctx):
    """Keystrokes edit the composer and Enter queues the next turn."""
    ctx.scenario("words=30,chunk=1,delay=0.06,first_delay=0.15")
    s = ctx.spawn()
    s.submit("take your time")
    s.wait_activity("thinking")
    # 'thinking' is painted before curl connects, so wait for the request to
    # actually land before counting them.
    s.wait_for(lambda t: len(ctx.mock.requests) == 1, "the request to be sent")

    s.type("typed while busy")
    s.key("enter")
    s.wait_text("message queued")
    assert s.composer_text() == "", s.composer_lines()
    assert s.activity_label() != "", "the turn should still be running"
    assert len(ctx.mock.requests) == 1, "Enter must not start a second turn"

    ctx.scenario("text=second+turn")
    s.wait_text("second turn")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 2
    assert ctx.mock.requests[-1]["messages"][-1]["content"] == "typed while busy"


def test_interrupt_stops_the_turn(ctx):
    """Ctrl-C during a stream marks the turn interrupted and returns to ready."""
    ctx.scenario("words=200,chunk=1,delay=0.05")
    s = ctx.spawn()
    s.submit("go on forever")
    s.wait_turn_underway()
    s.wait_for(lambda t: len(reply_text(s, "go on forever")) > 20,
               "streaming started")
    s.key("ctrl-c")
    s.wait_text("[interrupted]")
    s.wait_turn_done()
    # and the UI is still alive
    s.type("after interrupt").sync()
    assert s.composer_text() == "after interrupt", s.composer_lines()


def test_escape_stops_the_turn(ctx):
    """Esc during a stream cancels it, keeping whatever was being composed."""
    ctx.scenario("words=200,chunk=1,delay=0.05")
    s = ctx.spawn()
    s.submit("go on forever")
    s.wait_turn_underway()
    s.wait_for(lambda t: len(reply_text(s, "go on forever")) > 20,
               "streaming started")
    s.type("draft kept")
    s.key("esc")
    s.wait_text("[interrupted]")
    s.wait_turn_done()
    assert s.composer_text() == "draft kept", s.composer_lines()


def test_escape_when_idle_does_not_disturb_the_composer(ctx):
    """With no turn running, Esc is inert rather than an interrupt."""
    s = ctx.spawn()
    s.type("still here").sync()
    s.key("esc").sync()
    assert s.composer_text() == "still here", s.composer_lines()
    assert "[interrupted]" not in s.text()


def test_scenario_from_model_name(ctx):
    """The mock also takes its scenario from the model name, for manual runs."""
    s = ctx.spawn(ARQAN_MODEL="lorem:text=via+model+name")
    s.submit("hi")
    s.wait_text("via model name")
    s.wait_turn_done()


def test_piped_turn_is_line_oriented(ctx):
    """Without a tty the same turn prints plain lines and no escape codes."""
    ctx.scenario("text=piped+reply")
    out = ctx.run_piped("hello\n/exit\n")
    assert "piped reply" in out.stdout, out.stdout
    assert "\x1b[" not in out.stdout, "no escape sequences without a tty"
    body = json.dumps(ctx.mock.requests[-1])
    assert "hello" in body
