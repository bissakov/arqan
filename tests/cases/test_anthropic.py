"""The Anthropic wire format: content blocks in, content blocks out.

A turn reads the same on screen either way, so these cases assert on what
went over the wire as well as on the transcript.
"""

import json


def anth(ctx, **env):
    """A session whose endpoint speaks the Anthropic API."""
    return ctx.spawn(ARQAN_API="anthropic", **env)


def test_a_reply_streams_from_content_blocks(ctx):
    """text_delta events paint the same transcript a chat completion does."""
    ctx.scenario("text=hello+from+anthropic")
    s = anth(ctx)
    s.submit("say hi")
    s.wait_text("hello from anthropic")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert body["messages"] == [{
        "role": "user",
        "content": [{
            "type": "text", "text": "say hi",
            "cache_control": {"type": "ephemeral"},
        }],
    }], body["messages"]
    assert body["system"] == [{
        "type": "text",
        "text": "You are a test fixture.",
        "cache_control": {"type": "ephemeral"},
    }], body


def test_requests_enable_prompt_caching(ctx):
    """A growing agent loop caches its system and newest user prefixes."""
    ctx.scenario("text=cached")
    s = anth(ctx)
    s.submit("hello")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert body["system"][-1]["cache_control"] == {"type": "ephemeral"}
    assert body["messages"][-1]["content"][-1]["cache_control"] == {
        "type": "ephemeral"
    }


def test_the_system_prompt_is_a_parameter_not_a_message(ctx):
    """That API has no system role, so slot 0 leaves the messages array."""
    ctx.scenario("text=fine")
    s = anth(ctx)
    s.submit("hello")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert all(m["role"] != "system" for m in body["messages"]), body["messages"]
    assert body["system"][0]["text"] == "You are a test fixture."


def test_tools_are_declared_with_an_input_schema(ctx):
    """Anthropic takes the schema flat, not wrapped in a function object."""
    ctx.scenario("text=nothing+to+do")
    s = anth(ctx)
    s.submit("hello")
    s.wait_turn_done()
    tools = ctx.mock.requests[-1]["tools"]
    read = next(t for t in tools if t["name"] == "read")
    assert "input_schema" in read, read
    assert "function" not in read, read
    assert read["input_schema"]["type"] == "object", read


def test_a_tool_call_round_trips_as_blocks(ctx):
    """tool_use in, tool_result back, and the same transcript as ever."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    s = anth(ctx)
    s.submit("read the notes")
    s.wait_text("I read it")
    s.wait_turn_done()

    assert any("kept it" in r for r in ctx.mock.tool_results()), ctx.mock.tool_results()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["user", "assistant", "user"], messages
    use = messages[1]["content"][-1]
    assert use["type"] == "tool_use" and use["name"] == "read", use
    assert use["input"] == {"path": "notes.txt"}, use
    result = messages[2]["content"][0]
    assert result["type"] == "tool_result", result
    assert result["tool_use_id"] == use["id"], (result, use)


def test_parallel_results_ride_in_one_user_message(ctx):
    """Consecutive slots of one role are a single message there, not two."""
    ctx.write_file("a.txt", "alpha\n")
    ctx.write_file("b.txt", "beta\n")
    ctx.scenario(
        'tool=read:{"path":"a.txt"},tool=read:{"path":"b.txt"},final_text=both+read'
    )
    s = anth(ctx)
    s.submit("read both")
    s.wait_text("both read")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["user", "assistant", "user"], messages
    kinds = [b["type"] for b in messages[2]["content"]]
    assert kinds == ["tool_result", "tool_result"], messages[2]


def test_a_thinking_block_reaches_the_screen_and_the_tool_follow_up(ctx):
    """A trace is shown, then returned unchanged to continue a tool turn."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario(
        'redacted=opaque-secret,reasoning=weighing+it+up,'
        'tool=read:{"path":"notes.txt"},'
        'final_text=done'
    )
    s = anth(ctx)
    s.submit("think first")
    s.wait_text("weighing it up")
    s.wait_text("done")
    s.wait_turn_done()
    assistant = ctx.mock.requests[-1]["messages"][1]["content"]
    assert assistant[0] == {
        "type": "redacted_thinking",
        "data": "opaque-secret",
    }, json.dumps(assistant)
    assert assistant[1] == {
        "type": "thinking",
        "thinking": "weighing it up",
        "signature": "sig_mock",
    }, json.dumps(assistant)
    assert assistant[2]["type"] == "tool_use", assistant


def test_an_unstreamed_reply_is_read_the_same_way(ctx):
    """With streaming off the message document reaches the same slots."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario(
        'reasoning=checking+the+file,tool=read:{"path":"notes.txt"},'
        'final_text=whole+reply'
    )
    s = anth(ctx)
    s.settings_toggle("Stream replies")
    s.submit("read the notes")
    s.wait_text("whole reply")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["stream"] is False, ctx.mock.requests[-1]
    assert any("kept it" in r for r in ctx.mock.tool_results()), ctx.mock.tool_results()
    thinking = ctx.mock.requests[-1]["messages"][1]["content"][0]
    assert thinking["thinking"] == "checking the file", thinking
    assert thinking["signature"] == "sig_mock", thinking


def test_signed_thinking_survives_a_session_resume(ctx):
    """Resuming keeps both the visible summary and its opaque signature."""
    ctx.scenario("reasoning=remembering+why,text=first+answer")
    first = anth(ctx)
    first.submit("first question")
    first.wait_text("first answer")
    first.wait_turn_done()
    first.submit("/exit")
    first.wait_exit()

    again = anth(ctx)
    again.submit("/resume")
    again.wait_status("pick a session")
    again.key("enter")
    again.wait_text("remembering why")

    ctx.scenario("text=second+answer")
    again.submit("second question")
    again.wait_turn_done()
    old = ctx.mock.requests[-1]["messages"][1]["content"][0]
    assert old == {
        "type": "thinking",
        "thinking": "remembering why",
        "signature": "sig_mock",
    }, old


def test_usage_from_message_start_and_message_delta(ctx):
    """The prompt is priced on the first event and the reply on the last, and
    a cache read is context the request carried like any other."""
    ctx.scenario("text=counted,usage=200/40,cache_read=1000")
    s = anth(ctx)
    s.submit("hello")
    s.wait_turn_done()
    s.wait_for(lambda t: s.status_field(5) not in ("-", ""), "the context")
    counted = s.status_field(5)
    assert int(counted.lstrip("~")) >= 1200, s.status_line()


def test_the_shell_run_is_a_text_block(ctx):
    """A '!' run is a user turn there too: the command, then what it printed."""
    ctx.scenario("text=noted")
    s = anth(ctx)
    s.submit("!echo hello-from-shell")
    s.wait_text("hello-from-shell")
    s.submit("what did that print?")
    s.wait_turn_done()
    first = ctx.mock.requests[-1]["messages"][0]
    assert first["role"] == "user", first
    text = first["content"][0]["text"]
    assert text.startswith("!echo hello-from-shell"), text
    assert "hello-from-shell" in text, text


def test_malformed_tool_arguments_keep_the_request_valid(ctx):
    """A call whose arguments are not JSON must not corrupt every later body."""
    bad = '{"path":"notes.txt",,"limit":3}'
    ctx.scenario("tool=read:" + bad + ",final_text=recovered")
    s = anth(ctx)
    s.submit("read the notes")
    s.wait_text("recovered")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    use = next(b for b in messages[1]["content"] if b["type"] == "tool_use")
    assert use["input"] == {"invalid_arguments": bad}, use
    assert any("bad args json" in r for r in ctx.mock.tool_results()), \
        ctx.mock.tool_results()


def test_resumed_tool_arguments_keep_their_shape(ctx):
    """Validity is decided when a call is recorded, so a resume must redecide.

    A resumed session sends the arguments it saved: well-formed ones as the
    object the model wrote, malformed ones as the string it wrote instead.
    """
    ctx.write_file("notes.txt", "hello from disk\n")
    bad = '{"path":"notes.txt",,"limit":3}'
    ctx.scenario("tool=read:" + bad
                 + ',tool=read:{"path":"notes.txt"},final_text=recovered')
    s = anth(ctx)
    s.submit("read the notes")
    s.wait_text("recovered")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    s = anth(ctx)
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("recovered")
    ctx.scenario("final_text=and+again")
    s.submit("thanks")
    s.wait_text("and again")
    s.wait_turn_done()
    uses = [b for m in ctx.mock.requests[-1]["messages"]
            if isinstance(m["content"], list)
            for b in m["content"] if b["type"] == "tool_use"]
    assert [u["input"] for u in uses] == [
        {"invalid_arguments": bad}, {"path": "notes.txt"},
    ], uses
