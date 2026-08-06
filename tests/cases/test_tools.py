"""Tool calls: the agent loop, transcript rendering and the follow-up turn."""

import json


def test_read_tool_round_trip(ctx):
    """A read call runs, shows in the transcript and is fed back to the model."""
    ctx.write_file("notes.txt", "hello from disk\n")
    # usage is pinned so the golden's token counter does not move with the
    # exact byte length of the serialised conversation
    ctx.scenario(
        'tool=read:{"path":"notes.txt"},final_text=I+read+the+file.,usage=200/12'
    )
    s = ctx.spawn()
    s.submit("what is in notes.txt?")
    s.wait_text("I read the file.")
    s.wait_turn_done()

    text = s.text()
    assert "\u25c6  Tool \u00b7 read" in text, text
    assert '"path":"notes.txt"' in text.replace(" ", ""), text
    assert "\u2514\u2500 Result" in text, text
    assert "hello from disk" in text, text
    ctx.check_screen(s)

    # two provider calls: the tool request, then the follow-up with its result
    assert len(ctx.mock.requests) == 2, ctx.mock.requests
    assert ctx.mock.tool_results() == ["hello from disk\n"], ctx.mock.tool_results()


def test_tool_call_is_replayed_to_the_provider(ctx):
    """The follow-up request carries the assistant tool_call and its result."""
    ctx.write_file("a.txt", "content A")
    ctx.scenario('tool=read:{"path":"a.txt"},final_text=done')
    s = ctx.spawn()
    s.submit("read a.txt")
    s.wait_text("done")
    s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    roles = [m["role"] for m in messages]
    assert roles == ["system", "user", "assistant", "tool"], roles
    call = messages[2]["tool_calls"][0]
    assert call["type"] == "function"
    assert call["function"]["name"] == "read"
    assert json.loads(call["function"]["arguments"]) == {"path": "a.txt"}
    assert messages[3]["tool_call_id"] == call["id"]
    assert messages[3]["content"] == "content A"


def test_write_tool_creates_a_file(ctx):
    """The write tool actually touches the filesystem."""
    args = json.dumps({"path": "created.txt", "content": "written by a tool"})
    ctx.scenario(f"tool=write:{args},final_text=file+written")
    s = ctx.spawn()
    s.submit("create a file")
    s.wait_text("file written")
    s.wait_turn_done()
    assert (ctx.work / "created.txt").read_text() == "written by a tool"


def test_edit_tool_replaces_text(ctx):
    """The edit tool rewrites the matched span in place."""
    ctx.write_file("edit.txt", "alpha BETA gamma")
    args = json.dumps({"path": "edit.txt", "old_text": "BETA", "new_text": "beta"})
    ctx.scenario(f"tool=edit:{args},final_text=edited")
    s = ctx.spawn()
    s.submit("fix the case")
    s.wait_text("edited")
    s.wait_turn_done()
    assert (ctx.work / "edit.txt").read_text() == "alpha beta gamma"


def test_bash_tool_runs_a_command(ctx):
    """The bash tool's stdout comes back as the tool result."""
    args = json.dumps({"command": "echo tool-output-marker"})
    ctx.scenario(f"tool=bash:{args},final_text=ran+it")
    s = ctx.spawn()
    s.submit("run echo")
    s.wait_text("ran it")
    s.wait_turn_done()
    assert "tool-output-marker" in s.text()
    assert any("tool-output-marker" in r for r in ctx.mock.tool_results())


def test_failing_tool_reports_an_error(ctx):
    """A tool that fails feeds its error back instead of aborting the turn."""
    ctx.scenario('tool=read:{"path":"missing.txt"},final_text=that+file+is+gone')
    s = ctx.spawn()
    s.submit("read a missing file")
    s.wait_text("that file is gone")
    s.wait_turn_done()
    results = ctx.mock.tool_results()
    assert results and results[0].startswith("ERROR:"), results
    assert "ERROR" in s.text()


def test_unknown_tool_is_reported(ctx):
    """A call to a tool that does not exist is an error, not a crash."""
    ctx.scenario("tool=nosuchtool:{},final_text=oh+well")
    s = ctx.spawn()
    s.submit("call something odd")
    s.wait_text("oh well")
    s.wait_turn_done()
    assert ctx.mock.tool_results()[0].startswith("ERROR:"), ctx.mock.tool_results()


def test_parallel_tool_calls(ctx):
    """Several tool calls in one assistant message all run, in order."""
    ctx.write_file("one.txt", "first file")
    ctx.write_file("two.txt", "second file")
    ctx.scenario(
        'tool=read:{"path":"one.txt"},tool=read:{"path":"two.txt"},'
        "tool_rounds=1,final_text=both+read"
    )
    s = ctx.spawn()
    s.submit("read both")
    s.wait_text("both read")
    s.wait_turn_done()
    text = s.text()
    assert "first file" in text and "second file" in text, text
    assert ctx.mock.tool_results() == ["first file", "second file"]


def test_status_names_the_running_tool(ctx):
    """While a tool runs the status line says which one."""
    args = json.dumps({"command": "sleep 0.6; echo slept"})
    ctx.scenario(f"tool=bash:{args},final_text=finished")
    s = ctx.spawn()
    s.submit("run something slow")
    s.wait_for(lambda t: s.status_kind() == "running bash", "the running-tool status")
    s.wait_text("finished")
    s.wait_turn_done()
    assert s.status_kind() == "ready"


def test_large_tool_output_is_truncated_in_the_transcript(ctx):
    """A big result is trimmed on screen but sent to the model in full."""
    body = "\n".join(f"line {i:04d} of output" for i in range(200))
    ctx.write_file("big.txt", body)
    ctx.scenario('tool=read:{"path":"big.txt"},final_text=that+is+a+lot')
    s = ctx.spawn()
    s.submit("read big.txt")
    s.wait_text("that is a lot")
    s.wait_turn_done()
    assert "output truncated in transcript" in s.text()
    assert ctx.mock.tool_results()[0].strip().endswith("line 0199 of output")
