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
    assert "\u25c6  read notes.txt" in text, text
    assert "{" not in text, text          # no JSON arguments on screen
    assert "\u2514\u2500 1 line" in text, text
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


def test_write_tool_previews_the_content(ctx):
    """A write shows the path and the head of what it writes, not JSON."""
    body = "".join(f"line {i}\n" for i in range(12))
    args = json.dumps({"path": "long.txt", "content": body})
    ctx.scenario(f"tool=write:{args},final_text=written")
    s = ctx.spawn()
    s.submit("write a file")
    s.wait_text("written")
    s.wait_turn_done()

    text = s.text()
    assert "\u25c6  write long.txt" in text, text
    assert "\u2502 line 0" in text, text
    assert "\u2502 ... 4 more lines" in text, text
    assert "line 8" not in text, text
    assert "\u2514\u2500 wrote" in text, text


def test_edit_tool_shows_a_diff(ctx):
    """An edit call reads as the lines it removes and the ones it adds."""
    ctx.write_file("diff.txt", "keep\nold one\nkeep\n")
    args = json.dumps(
        {"path": "diff.txt", "old_text": "old one", "new_text": "new one"}
    )
    ctx.scenario(f"tool=edit:{args},final_text=patched")
    s = ctx.spawn()
    s.submit("patch it")
    s.wait_text("patched")
    s.wait_turn_done()

    text = s.text()
    assert "\u25c6  edit diff.txt" in text, text
    assert "\u2502 - old one" in text, text
    assert "\u2502 + new one" in text, text
    assert s.screen.attr_at(s.screen.find_row("\u2502 - old one"), 2).fg == 203
    assert s.screen.attr_at(s.screen.find_row("\u2502 + new one"), 2).fg == 114


def test_bash_result_is_summarised_by_its_exit_status(ctx):
    """The command heads the call and its exit code heads the result."""
    args = json.dumps({"command": "echo hi; exit 3"})
    ctx.scenario(f"tool=bash:{args},final_text=it+failed")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("it failed")
    s.wait_turn_done()

    text = s.text()
    assert "\u25c6  bash echo hi; exit 3" in text, text
    assert "\u2514\u2500 exit 3" in text, text
    assert "   hi" in text, text


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
    text = s.text()
    assert "\u2514\u2500 error: open missing.txt failed" in text, text
    row = s.screen.find_row("\u2514\u2500 error:")
    assert s.screen.attr_at(row, 2).fg == 203, text   # S_RED


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
    assert "... 188 more lines" in s.text(), s.text()
    assert ctx.mock.tool_results()[0].strip().endswith("line 0199 of output")


def test_verbose_shows_every_line_of_a_result(ctx):
    """/verbose drops the transcript's caps; toggling it back restores them."""
    body = "\n".join(f"line {i:04d} of output" for i in range(40))
    ctx.write_file("big.txt", body)
    ctx.scenario('tool=read:{"path":"big.txt"},final_text=all+of+it')
    s = ctx.spawn()
    s.submit("/verbose")
    s.wait_text("verbose: tool output is shown in full")
    s.submit("read big.txt")
    s.wait_text("all of it")
    s.wait_turn_done()

    text = s.text()
    assert "more lines" not in text, text
    assert "line 0039 of output" in text, text
    for _ in range(12):
        s.mouse("wheel-up", 5, 10).sync()
    assert "line 0000 of output" in s.text(), s.text()

    s.submit("/verbose")
    s.wait_text("verbose: tool output is truncated")
    # tool_rounds counts the whole conversation's tool replies, and the
    # verbose turn already left one behind
    ctx.scenario(
        'tool=read:{"path":"big.txt"},tool_rounds=2,final_text=just+the+head'
    )
    s.submit("read it again")
    s.wait_text("just the head")
    s.wait_turn_done()
    assert "... 28 more lines" in s.text(), s.text()


def test_verbose_shows_a_long_command_whole(ctx):
    """A call header is clipped only while verbose is off."""
    marker = "x" * 200
    args = json.dumps({"command": f"echo {marker}"})
    ctx.scenario(f"tool=bash:{args},final_text=ran")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("ran")
    s.wait_turn_done()
    assert " ..." in s.text(), s.text()

    s.submit("/verbose")
    s.wait_text("verbose: tool output is shown in full")
    ctx.scenario(f"tool=bash:{args},tool_rounds=2,final_text=ran+again")
    s.submit("run it again")
    s.wait_text("ran again")
    s.wait_turn_done()
    assert " ..." not in s.text(), s.text()
