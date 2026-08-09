"""The Anthropic wire format: content blocks in, content blocks out.

A turn reads the same on screen either way, so these cases assert on what
went over the wire as well as on the transcript.
"""

import json


def anth(ctx, **env):
    """A session whose endpoint speaks the Anthropic API."""
    return ctx.spawn(YOKE_API="anthropic", **env)


def test_a_reply_streams_from_content_blocks(ctx):
    """text_delta events paint the same transcript a chat completion does."""
    ctx.scenario("text=hello+from+anthropic")
    s = anth(ctx)
    s.submit("say hi")
    s.wait_text("hello from anthropic")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert body["messages"] == [
        {"role": "user", "content": [{"type": "text", "text": "say hi"}]}
    ], body["messages"]
    assert body["system"] == "You are a test fixture.", body


def test_the_system_prompt_is_a_parameter_not_a_message(ctx):
    """That API has no system role, so slot 0 leaves the messages array."""
    ctx.scenario("text=fine")
    s = anth(ctx)
    s.submit("hello")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert all(m["role"] != "system" for m in body["messages"]), body["messages"]
    assert body["system"] == "You are a test fixture."


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


def test_a_thinking_block_reaches_the_screen_and_not_the_wire(ctx):
    """A trace is shown as it streams and never replayed to the provider."""
    ctx.scenario("reasoning=weighing+it+up,text=done,tool_rounds=0")
    s = anth(ctx)
    s.submit("think first")
    s.wait_text("weighing it up")
    s.wait_text("done")
    s.wait_turn_done()
    s.submit("again")
    s.wait_turn_done()
    raw = json.dumps(ctx.mock.requests[-1])
    assert "weighing it up" not in raw, raw


def test_an_unstreamed_reply_is_read_the_same_way(ctx):
    """With streaming off the message document reaches the same slots."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=whole+reply')
    s = anth(ctx)
    s.settings_toggle("Stream replies")
    s.submit("read the notes")
    s.wait_text("whole reply")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["stream"] is False, ctx.mock.requests[-1]
    assert any("kept it" in r for r in ctx.mock.tool_results()), ctx.mock.tool_results()


def test_usage_from_message_start_and_message_delta(ctx):
    """The prompt is priced on the first event and the reply on the last."""
    ctx.scenario("text=counted,usage=1200/40")
    s = anth(ctx)
    s.submit("hello")
    s.wait_turn_done()
    s.wait_for(lambda t: "1240" in t.row_text(t.rows - 1), "token total")
    assert s.status_field(5) != "-", s.status_line()


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
