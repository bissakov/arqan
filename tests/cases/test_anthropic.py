"""The Anthropic wire format: content blocks in, content blocks out.

A turn reads the same on screen either way, so these cases assert on what
went over the wire as well as on the transcript.
"""

import json


def row_of(s, needle: str) -> int:
    for row in range(s.screen.rows):
        if needle in s.screen.row_text(row):
            return row
    raise AssertionError(f"{needle!r} is not on screen:\n{s.text()}")


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


def test_bash_schema_names_the_configured_timeout_ceiling(ctx):
    ctx.write_config("shell_timeout_ms = 4321\n")
    ctx.scenario("text=nothing+to+do")
    s = anth(ctx)
    s.submit("hello")
    s.wait_turn_done()

    tools = ctx.mock.requests[-1]["tools"]
    bash = next(t for t in tools if t["name"] == "bash")
    timeout = bash["input_schema"]["properties"]["timeout_ms"]
    assert timeout["maximum"] == 4321, timeout
    assert "4321" in timeout["description"], timeout


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


def test_thinking_blocks_are_separated_without_changing_their_signatures(ctx):
    """Separate summaries read as lines and round-trip as distinct blocks."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario(
        "reasoning_summaries=Designing+the+module|Defining+the+API,chunk=1,"
        'tool=read:{"path":"notes.txt"},final_text=done'
    )
    s = anth(ctx)
    s.submit("think first")
    s.wait_text("done")
    s.wait_turn_done()
    assert row_of(s, "Designing the module") < row_of(s, "Defining the API")
    assistant = ctx.mock.requests[-1]["messages"][1]["content"]
    assert assistant[:2] == [
        {
            "type": "thinking",
            "thinking": "Designing the module",
            "signature": "sig_mock_0",
        },
        {
            "type": "thinking",
            "thinking": "Defining the API",
            "signature": "sig_mock_1",
        },
    ], json.dumps(assistant)


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


def assert_unsigned_thinking_is_not_replayed(ctx, s):
    s.submit("think first")
    s.wait_text("half a thought")
    s.wait_text("done")
    s.wait_turn_done()
    assert any("kept it" in r for r in ctx.mock.tool_results()), ctx.mock.tool_results()
    assistant = ctx.mock.requests[-1]["messages"][1]["content"]
    assert [blk["type"] for blk in assistant] == ["tool_use"], json.dumps(assistant)

    ctx.scenario("final_text=second+answer")
    s.submit("second question")
    s.wait_text("second answer")
    s.wait_turn_done()
    for message in ctx.mock.requests[-1]["messages"]:
        content = message["content"]
        if isinstance(content, list):
            for blk in content:
                assert blk["type"] != "thinking" or blk.get("signature"), blk


def test_a_streamed_thinking_block_without_a_signature_is_not_replayed(ctx):
    """The API refuses an unsigned block, so it would break every later turn."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario(
        'reasoning=half+a+thought,unsigned_thinking,'
        'tool=read:{"path":"notes.txt"},final_text=done'
    )
    assert_unsigned_thinking_is_not_replayed(ctx, anth(ctx))


def test_an_unstreamed_thinking_block_without_a_signature_is_not_replayed(ctx):
    """The message document path drops the unsigned block the same way."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario(
        'reasoning=half+a+thought,unsigned_thinking,'
        'tool=read:{"path":"notes.txt"},final_text=done'
    )
    s = anth(ctx)
    s.settings_toggle("Stream replies")
    assert_unsigned_thinking_is_not_replayed(ctx, s)


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
    assert counted.lstrip("~") == "1k", s.status_line()


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


def test_malformed_number_in_tool_arguments_is_refused_not_guessed(ctx):
    """A number the parser cannot read whole must fail, not decay to a prefix.
    "1.2.3" once reached strtod, which stopped at "1.2" and handed back a
    limit the model never wrote. Refusing the object is the only honest
    answer, so the call is recorded as invalid arguments.
    """
    bad = '{"path":"notes.txt","limit":1.2.3}'
    ctx.write_file("notes.txt", "alpha\nbeta\ngamma\n")
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
    assert not any("alpha" in r for r in ctx.mock.tool_results()), \
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


RETRY = {"ARQAN_RETRIES": 3, "ARQAN_RETRY_DELAY_MS": 10}


def test_an_overloaded_api_is_retried(ctx):
    """529 is how that API says it is overloaded: weather, not an answer."""
    ctx.scenario("fail_times=1,fail_status=529,text=fine+now")
    s = anth(ctx, **RETRY)
    s.submit("say hi")
    s.wait_text("fine now")
    s.wait_turn_done()
    assert "HTTP 529: mock provider error" in s.text(), s.text()
    assert len(ctx.mock.requests) == 2, ctx.mock.requests


def test_a_reply_cut_off_at_max_tokens_says_so(ctx):
    """A reply that ran out of tokens must not pass for a finished one."""
    ctx.scenario("text=half+a+sentence,stop=max_tokens")
    s = anth(ctx)
    s.submit("say a lot")
    s.wait_text("half a sentence")
    s.wait_text("[the reply stopped at the max_tokens limit of 32768]")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests


def test_a_tool_call_cut_off_at_max_tokens_is_explained_to_the_model(ctx):
    """The cut call fails, and the model hears why instead of guessing."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},stop=max_tokens,'
                 "final_text=smaller+now")
    s = anth(ctx)
    s.submit("read the notes")
    s.wait_text("smaller now")
    s.wait_turn_done()
    assert "[note to the model: Your last reply hit the max_tokens" in s.text(), \
        s.text()
    answer = ctx.mock.requests[-1]["messages"][-1]["content"]
    assert [b["type"] for b in answer] == ["tool_result", "text"], answer
    assert answer[0].get("is_error") is True, answer
    assert "max_tokens" in answer[1]["text"], answer


def test_a_refusal_is_reported(ctx):
    """What streamed before the refusal stays, and the stop is named."""
    ctx.scenario("text=I+can+start+with,stop=refusal")
    s = anth(ctx)
    s.submit("do something")
    s.wait_text("I can start with")
    s.wait_text("[the model refused to continue]")
    s.wait_turn_done()


def test_an_empty_refusal_is_not_retried(ctx):
    """A refusal is an answer: asking again gets the same one."""
    ctx.scenario("text=,stop=refusal")
    s = anth(ctx, **RETRY)
    s.submit("do something")
    s.wait_text("[provider error: the model refused to continue]")
    s.wait_turn_done()
    assert "retrying in" not in s.text(), s.text()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests


def test_a_full_context_window_is_reported(ctx):
    """The model stopped for room, and /compact is how to make some."""
    ctx.scenario("text=partial,stop=model_context_window_exceeded")
    s = anth(ctx)
    s.submit("go on")
    s.wait_text("partial")
    s.wait_text("larger than the model's context window; /compact")
    s.wait_turn_done()


def test_failed_tool_results_are_flagged(ctx):
    """is_error tells the model a call failed without reading the text."""
    ctx.write_file("notes.txt", "kept it\n")
    ctx.scenario('tool=read:{"path":"missing.txt"},'
                 'tool=read:{"path":"notes.txt"},final_text=done')
    s = anth(ctx)
    s.submit("read both")
    s.wait_text("done")
    s.wait_turn_done()
    results = ctx.mock.requests[-1]["messages"][2]["content"]
    assert [b.get("is_error") for b in results] == [True, None], results


def test_interleaved_thinking_returns_in_its_place(ctx):
    """The API refuses a latest assistant message whose thinking moved, so a
    block that came between two calls goes back between them, resumed or not."""
    ctx.write_file("a.txt", "alpha\n")
    ctx.write_file("b.txt", "beta\n")
    ctx.scenario('reasoning=first+thought,interleaved,'
                 'tool=read:{"path":"a.txt"},tool=read:{"path":"b.txt"},'
                 "final_text=both+read")
    s = anth(ctx)
    s.submit("read both")
    s.wait_text("both read")
    s.wait_turn_done()

    def assistant():
        return ctx.mock.requests[-1]["messages"][1]["content"]

    blocks = assistant()
    assert [b["type"] for b in blocks] == [
        "thinking", "tool_use", "thinking", "tool_use",
    ], json.dumps(blocks)
    assert blocks[2] == {
        "type": "thinking",
        "thinking": "before call 1",
        "signature": "sig_call_1",
    }, json.dumps(blocks)
    assert blocks[3]["input"] == {"path": "b.txt"}, blocks

    s.submit("/exit")
    s.wait_exit()
    again = anth(ctx)
    again.submit("/resume")
    again.wait_status("pick a session")
    again.key("enter")
    again.wait_text("both read")
    ctx.scenario("final_text=second+answer")
    again.submit("thanks")
    again.wait_text("second answer")
    again.wait_turn_done()
    assert assistant() == blocks, json.dumps(assistant())


def test_an_unstreamed_reply_keeps_its_thinking_in_place_and_its_stop(ctx):
    """The message document path places thinking and reads stop_reason too."""
    ctx.write_file("a.txt", "alpha\n")
    ctx.write_file("b.txt", "beta\n")
    ctx.scenario('reasoning=first+thought,interleaved,'
                 'tool=read:{"path":"a.txt"},tool=read:{"path":"b.txt"},'
                 "final_text=both+read")
    s = anth(ctx)
    s.settings_toggle("Stream replies")
    s.submit("read both")
    s.wait_text("both read")
    s.wait_turn_done()
    blocks = ctx.mock.requests[-1]["messages"][1]["content"]
    assert [b["type"] for b in blocks] == [
        "thinking", "tool_use", "thinking", "tool_use",
    ], json.dumps(blocks)

    ctx.scenario("text=half+a+sentence,stop=max_tokens")
    s.submit("/exit")
    s.wait_exit()
    s = anth(ctx)
    s.settings_toggle("Stream replies")
    s.submit("say a lot")
    s.wait_text("[the reply stopped at the max_tokens limit of 32768]")
    s.wait_turn_done()


def test_whitespace_only_text_is_not_sent(ctx):
    """The API refuses a text block with nothing but spaces in it."""
    ctx.scenario("text=+")
    s = anth(ctx)
    s.submit("say nothing")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 2, ctx.mock.requests
    for message in ctx.mock.requests[-1]["messages"]:
        for blk in message["content"]:
            if blk["type"] == "text":
                assert blk["text"].strip(), ctx.mock.requests[-1]["messages"]


def test_the_model_list_asks_for_every_model(ctx):
    """That API pages its list at 20, so a default request hides the rest."""
    ctx.scenario("model_count=30")
    s = anth(ctx)
    s.submit("/model")
    s.wait_status("pick a model")
    s.wait_for(lambda t: ctx.mock.listings, "the model list to be fetched")
    s.settle()
    s.type("029").sync()
    assert "model-029" in s.text(), s.text()
