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
    assert "\u2502 \u25be 4 more lines" in text, text
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
    assert "\u25be 188 more lines" in s.text(), s.text()
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
    assert "\u25be 28 more lines" in s.text(), s.text()


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


def row_of(s, needle):
    """1-based screen row containing `needle`."""
    r = s.screen.find_row(needle)
    assert r >= 0, f"{needle!r} not on screen\n{s.text()}"
    return r + 1


def click(s, needle, col=6):
    s.mouse("down", row_of(s, needle), col)
    s.mouse("up", row_of(s, needle), col)
    return s.sync()


def read_a_big_file(ctx, s, lines=40):
    body = "\n".join(f"line {i:04d} of output" for i in range(lines))
    ctx.write_file("big.txt", body)
    ctx.scenario('tool=read:{"path":"big.txt"},final_text=that+is+a+lot')
    s.submit("read big.txt")
    s.wait_text("that is a lot")
    s.wait_turn_done()
    return s


def test_verbose_repaints_the_blocks_already_on_screen(ctx):
    """/verbose applies to the transcript, not only to the next tool call."""
    s = ctx.spawn()
    read_a_big_file(ctx, s)
    assert "\u25be 28 more lines" in s.text(), s.text()

    s.submit("/verbose")
    s.wait_text("verbose: tool output is shown in full")
    text = s.text()
    assert "more lines" not in text, text
    assert "line 0039 of output" in text, text

    s.submit("/verbose")
    s.wait_text("verbose: tool output is truncated")
    assert "\u25be 28 more lines" in s.text(), s.text()


def test_clicking_a_truncated_block_expands_and_folds_it(ctx):
    """The '... N more lines' tail is a click target for that block alone."""
    s = ctx.spawn()
    read_a_big_file(ctx, s)
    assert "\u25be 28 more lines" in s.text(), s.text()

    click(s, "\u25be 28 more lines")
    s.wait_text("\u25b4 show less")
    text = s.text()
    assert "more lines" not in text, text
    assert "line 0039 of output" in text, text

    click(s, "\u25b4 show less")
    s.wait_text("\u25be 28 more lines")
    assert "line 0039 of output" not in s.text(), s.text()


def test_expanding_one_block_leaves_the_other_truncated(ctx):
    """Expansion is per block: the call's preview is not the result's."""
    body = "".join(f"line {i}\n" for i in range(12))
    args = json.dumps({"path": "long.txt", "content": body})
    ctx.scenario(f"tool=write:{args},final_text=written")
    s = ctx.spawn()
    s.submit("write a file")
    s.wait_text("written")
    s.wait_turn_done()
    assert "\u2502 \u25be 4 more lines" in s.text(), s.text()

    click(s, "\u2502 \u25be 4 more lines")
    s.wait_text("line 11")
    text = s.text()
    assert "more lines" not in text, text
    assert "\u2514\u2500 wrote" in text, text


def test_dragging_over_the_tail_selects_instead_of_expanding(ctx):
    """A drag is a copy; only a click on the same row folds a block."""
    s = ctx.spawn()
    read_a_big_file(ctx, s)
    row = row_of(s, "\u25be 28 more lines")
    s.mouse("down", row, 6)
    s.mouse("drag", row, 12)
    s.mouse("up", row, 12).sync()
    assert "\u25be 28 more lines" in s.text(), s.text()
    assert s.screen.clipboard, "the drag should have copied"


def test_expanding_from_a_scrolled_view_keeps_the_block_in_place(ctx):
    """A block unfolded above the viewport's bottom stays where it was."""
    s = ctx.spawn()
    read_a_big_file(ctx, s, lines=200)
    # push the tail row up the screen, so the viewport is no longer pinned
    s.mouse("wheel-up", 5, 10)
    s.sync()
    before = row_of(s, "\u25be 188 more lines")

    click(s, "\u25be 188 more lines")
    s.wait_text("\u25b4 show less")
    assert row_of(s, "\u25b4 show less") == before, s.text()


def test_a_foldable_block_looks_clickable_and_answers_the_pointer(ctx):
    """The tail reads as a link and brightens while the pointer is on it."""
    s = ctx.spawn()
    read_a_big_file(ctx, s)
    row = row_of(s, "\u25be 28 more lines")
    col = s.screen.row_text(row - 1).index("\u25be")   # 0-based, the marker

    idle = s.screen.attr_at(row - 1, col)
    assert idle.underline, idle
    assert not idle.bold, idle

    s.mouse("move", row, col + 3).sync()
    hot = s.screen.attr_at(row - 1, col)
    assert hot.underline and hot.bold, hot
    assert hot.bg != idle.bg, (hot, idle)
    # the highlight hugs the label: the block's indent is not part of it
    assert s.screen.attr_at(row - 1, col - 1).bg is None, s.text()

    s.mouse("move", row_of(s, "line 0000 of output"), col).sync()
    assert s.screen.attr_at(row - 1, col) == idle, s.screen.attr_at(row - 1, col)


def blank_above(s, needle):
    """Whether the row above `needle` holds nothing but the scrollbar."""
    row = s.screen.find_row(needle)
    assert row > 0, f"{needle!r} not on screen\n{s.text()}"
    return s.screen.row_text(row - 1).strip("\u2502\u2503 ") == ""


def test_a_replayed_reply_keeps_its_air_above(ctx):
    """A tool result and the reply below it stay one blank row apart."""
    s = ctx.spawn()
    read_a_big_file(ctx, s)
    assert blank_above(s, "that is a lot"), s.text()

    click(s, "\u25be 28 more lines")
    s.wait_text("\u25b4 show less")
    assert blank_above(s, "that is a lot"), s.text()


def test_hovering_a_tail_leaves_the_scrollbar_alone(ctx):
    """The bar keeps its own cell and its own style while a row lights up."""
    s = ctx.spawn()
    read_a_big_file(ctx, s, lines=300)
    row = row_of(s, "\u25be 288 more lines")
    last = s.screen.cols - 1
    s.mouse("move", row, 8).sync()
    assert s.screen.row_text(row - 1)[last] in "\u2502\u2503", s.text()
    assert s.screen.attr_at(row - 1, last).bg is None, s.text()


def test_an_ambiguous_edit_is_refused(ctx):
    """A repeated old_text names no hunk: patching the first is a coin toss."""
    ctx.write_file("twice.txt", "same line\nkeep\nsame line\n")
    args = json.dumps(
        {"path": "twice.txt", "old_text": "same line", "new_text": "changed"}
    )
    ctx.scenario(f"tool=edit:{args},final_text=ambiguous")
    s = ctx.spawn()
    s.submit("patch it")
    s.wait_text("ambiguous")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert result.startswith("ERROR:"), result
    assert "appears 2 times" in result, result
    assert (ctx.work / "twice.txt").read_text() == "same line\nkeep\nsame line\n"


def test_one_edit_call_carries_several_replacements(ctx):
    """Separate locations in one file are one call, not one call each."""
    ctx.write_file("multi.txt", "alpha\nbeta\ngamma\n")
    args = json.dumps(
        {
            "path": "multi.txt",
            "edits": [
                {"old_text": "alpha", "new_text": "ALPHA"},
                {"old_text": "gamma", "new_text": "GAMMA"},
            ],
        }
    )
    ctx.scenario(f"tool=edit:{args},final_text=both+applied")
    s = ctx.spawn()
    s.submit("patch both")
    s.wait_text("both applied")
    s.wait_turn_done()

    assert (ctx.work / "multi.txt").read_text() == "ALPHA\nbeta\nGAMMA\n"
    assert ctx.mock.tool_results()[-1] == "2 edits applied"
    text = s.text()
    assert "\u2502 - alpha" in text and "\u2502 + ALPHA" in text, text
    assert "\u2502 - gamma" in text and "\u2502 + GAMMA" in text, text


def test_a_failing_edit_leaves_the_file_untouched(ctx):
    """The batch is written once at the end, so it is all or nothing."""
    ctx.write_file("multi.txt", "alpha\nbeta\n")
    args = json.dumps(
        {
            "path": "multi.txt",
            "edits": [
                {"old_text": "alpha", "new_text": "ALPHA"},
                {"old_text": "nowhere", "new_text": "x"},
            ],
        }
    )
    ctx.scenario(f"tool=edit:{args},final_text=nothing+changed")
    s = ctx.spawn()
    s.submit("patch it")
    s.wait_text("nothing changed")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "edit 2: old_text not found" in result, result
    assert (ctx.work / "multi.txt").read_text() == "alpha\nbeta\n"


def test_an_edit_sees_what_the_edit_before_it_left(ctx):
    """Replacements apply in order against the running text, not the original."""
    ctx.write_file("chain.txt", "one\n")
    args = json.dumps(
        {
            "path": "chain.txt",
            "edits": [
                {"old_text": "one", "new_text": "two"},
                {"old_text": "two", "new_text": "three"},
            ],
        }
    )
    ctx.scenario(f"tool=edit:{args},final_text=chained")
    s = ctx.spawn()
    s.submit("chain them")
    s.wait_text("chained")
    s.wait_turn_done()
    assert (ctx.work / "chain.txt").read_text() == "three\n"
