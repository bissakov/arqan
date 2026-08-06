"""A full turn against the dummy provider: request, SSE deltas, transcript."""

import json


def test_simple_turn(ctx):
    """One turn renders You/Assistant blocks and returns to ready."""
    ctx.scenario("text=Hello+from+the+mock+provider.,usage=120/8")
    s = ctx.spawn()
    s.submit("say hi")
    s.wait_text("Hello from the mock provider.")
    s.wait_turn_done()
    ctx.check_screen(s)

    assert "\u25cf  You" in s.text()
    assert "\u25cf  Assistant" in s.text()
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
    assert names == ["bash", "edit", "read", "write"], names


def test_usage_updates_context_counter(ctx):
    """The status line shows the total token count reported by the provider."""
    ctx.scenario("text=counted,usage=1234/66")
    s = ctx.spawn()
    s.submit("count me")
    s.wait_text("counted")
    s.wait_for(lambda t: "1300" in t.row_text(t.rows - 1), "token total")
    assert s.status_field(4) != "-", s.status_line()


def test_status_is_thinking_while_streaming(ctx):
    """The status line reads 'thinking' until the stream finishes."""
    ctx.scenario("words=30,chunk=1,delay=0.05,first_delay=0.2")
    s = ctx.spawn()
    s.submit("slow down")
    s.wait_status("thinking")
    s.wait_turn_done()


def test_streaming_is_incremental(ctx):
    """Deltas appear as they arrive rather than all at the end."""
    ctx.scenario("words=40,chunk=2,delay=0.03")
    s = ctx.spawn()
    s.submit("stream it")

    def partial(t):
        body = t.text()
        return "Assistant" in body and len(body.split("Assistant", 1)[1].strip()) > 20

    s.wait_for(partial, "partial assistant output")
    mid = s.text()
    s.wait_turn_done()
    assert len(s.text()) > len(mid), "more text should have arrived after the check"


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


def test_long_output_scrolls_and_shows_a_scrollbar(ctx):
    """A reply taller than the viewport pins the transcript to its bottom."""
    ctx.scenario("words=400,paragraphs=4,chunk=8")
    s = ctx.spawn()
    s.submit("write a lot")
    s.wait_turn_done()
    bar = s.scrollbar()
    assert "\u2503" in bar, f"expected a scrollbar thumb, got {bar}"
    assert "\u2502" in bar, "expected an unfilled scrollbar track"


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
    s = ctx.spawn(YOKE_BASE_URL="http://127.0.0.1:1/v1")
    s.submit("nobody home")
    s.wait_text("[provider error:")
    s.wait_turn_done()


def test_typing_while_busy_is_kept(ctx):
    """Keystrokes during a turn edit the composer; Enter does not submit."""
    ctx.scenario("words=30,chunk=1,delay=0.06,first_delay=0.15")
    s = ctx.spawn()
    s.submit("take your time")
    s.wait_status("thinking")
    # 'thinking' is painted before curl connects, so wait for the request to
    # actually land before counting them.
    s.wait_for(lambda t: len(ctx.mock.requests) == 1, "the request to be sent")

    s.type("typed while busy")
    s.key("enter")                    # must be swallowed while a turn runs
    s.wait_for(
        lambda t: "typed while busy" in "".join(t.lines()[t.rows - 5 :]),
        "composer keeps the text",
    )
    assert s.status_kind() == "thinking", "the turn should still be running"
    assert len(ctx.mock.requests) == 1, "Enter must not start a second turn"

    s.wait_turn_done()
    assert s.composer_text() == "typed while busy", s.composer_lines()

    # once the turn is over, the same text submits normally
    ctx.scenario("text=second+turn")
    s.key("enter")
    s.wait_text("second turn")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 2
    assert ctx.mock.requests[-1]["messages"][-1]["content"] == "typed while busy"


def test_interrupt_stops_the_turn(ctx):
    """Ctrl-C during a stream marks the turn interrupted and returns to ready."""
    ctx.scenario("words=200,chunk=1,delay=0.05")
    s = ctx.spawn()
    s.submit("go on forever")
    s.wait_status("thinking")
    s.wait_for(lambda t: "Assistant" in t.text(), "streaming started")
    s.key("ctrl-c")
    s.wait_text("[interrupted]")
    s.wait_turn_done()
    # and the UI is still alive
    s.type("after interrupt").sync()
    assert s.composer_text() == "after interrupt", s.composer_lines()


def test_scenario_from_model_name(ctx):
    """The mock also takes its scenario from the model name, for manual runs."""
    s = ctx.spawn(YOKE_MODEL="lorem:text=via+model+name")
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
