"""Tool calls: the agent loop, transcript rendering and the follow-up turn."""

import json

from .test_image import png


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


def test_tool_call_without_an_id_still_runs(ctx):
    """An OpenAI-compatible server may omit the call id; the round still runs.

    An absent id reaches the conversation as an empty Str with a NULL pointer,
    which duplicating into the persistent arena used to copy from NULL.
    """
    ctx.write_file("a.txt", "content A")
    ctx.scenario('tool=read:{"path":"a.txt"},tool_ids=0,final_text=done')
    s = ctx.spawn()
    s.submit("read a.txt")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["content A"], ctx.mock.tool_results()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["system", "user", "assistant", "tool"]
    assert messages[2]["tool_calls"][0].get("id", "") == ""
    assert messages[3]["tool_call_id"] == ""


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


def patch_call(diff, **kw):
    """A scenario running one patch call over `diff`."""
    args = json.dumps({"patch": diff})
    rest = ",".join(f"{k}={v}" for k, v in kw.items())
    return f"tool=patch:{args}" + (f",{rest}" if rest else "")


def test_patch_shows_the_diff_it_applies(ctx):
    """A patch call reads as the diff it carries, coloured by its markers."""
    ctx.write_file("diff.txt", "keep\nold one\nkeep\n")
    diff = (
        "--- a/diff.txt\n+++ b/diff.txt\n@@ -1,3 +1,3 @@\n"
        " keep\n-old one\n+new one\n keep\n"
    )
    ctx.scenario(patch_call(diff, final_text="patched"))
    s = ctx.spawn()
    s.submit("patch it")
    s.wait_text("patched")
    s.wait_turn_done()

    text = s.text()
    assert "\u25c6  patch diff.txt" in text, text
    assert "\u2502 -old one" in text, text
    assert "\u2502 +new one" in text, text
    old_row = s.screen.find_row("\u2502 -old one")
    old_col = s.screen.row_text(old_row).index("-old one")
    new_row = s.screen.find_row("\u2502 +new one")
    new_col = s.screen.row_text(new_row).index("+new one")
    assert s.screen.attr_at(old_row, old_col).fg == 203
    assert s.screen.attr_at(new_row, new_col).fg == 114
    assert s.screen.attr_at(old_row, old_col + 1).fg == 253
    assert s.screen.attr_at(new_row, new_col + 1).fg == 253


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


def test_patch_rewrites_a_line_in_place(ctx):
    """A hunk is located by its context and applied to the file."""
    ctx.write_file("edit.txt", "alpha\nBETA\ngamma\n")
    diff = (
        "--- a/edit.txt\n+++ b/edit.txt\n@@ -1,3 +1,3 @@\n"
        " alpha\n-BETA\n+beta\n gamma\n"
    )
    ctx.scenario(patch_call(diff, final_text="edited"))
    s = ctx.spawn()
    s.submit("fix the case")
    s.wait_text("edited")
    s.wait_turn_done()
    assert (ctx.work / "edit.txt").read_text() == "alpha\nbeta\ngamma\n"
    assert ctx.mock.tool_results()[-1].strip() == "edit.txt +1 -1"


def test_patch_reports_all_bad_hunks_with_locations(ctx):
    """One failed call names every bad hunk and leaves the file untouched."""
    original = "same\nold a\nsame\nold b\nsame\n"
    ctx.write_file("edit.txt", original)
    diff = (
        "--- edit.txt\n+++ edit.txt\n"
        "@@\n same\n-old x\n+new x\n"
        "@@\n same\n-old y\n+new y\n"
    )
    ctx.scenario(patch_call(diff, final_text="not changed"))
    s = ctx.spawn()
    s.submit("apply it")
    s.wait_text("not changed")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "edit.txt hunk 1:" in result, result
    assert "edit.txt hunk 2:" in result, result
    assert "line 2 is \"old a\" where the hunk wants \"old x\"" in result, result
    assert "nothing was written" in result, result
    assert (ctx.work / "edit.txt").read_text() == original


def test_patch_ambiguous_context_names_matching_lines(ctx):
    ctx.write_file("edit.txt", "repeat\nother\nrepeat\n")
    diff = "--- edit.txt\n+++ edit.txt\n@@\n-repeat\n+changed\n"
    ctx.scenario(patch_call(diff, final_text="ambiguous"))
    s = ctx.spawn()
    s.submit("apply it")
    s.wait_text("ambiguous")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "matches 2 places (lines 1, 3)" in result, result
    assert (ctx.work / "edit.txt").read_text() == "repeat\nother\nrepeat\n"


def test_patch_explains_apply_patch_envelope(ctx):
    ctx.write_file("edit.txt", "old\n")
    diff = (
        "*** Begin Patch\n*** Update File: edit.txt\n@@\n-old\n+new\n"
        "*** End Patch"
    )
    ctx.scenario(patch_call(diff, final_text="wrong format"))
    s = ctx.spawn()
    s.submit("apply it")
    s.wait_text("wrong format")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "apply_patch envelope, not a unified diff" in result, result
    assert "'--- path', '+++ path', then '@@'" in result, result
    assert (ctx.work / "edit.txt").read_text() == "old\n"


def test_bad_tool_json_reports_the_byte_and_input(ctx):
    ctx.scenario('tool=read:{"path":"a",,"limit":1},final_text=bad+json')
    s = ctx.spawn()
    s.submit("read it")
    s.wait_text("bad json")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "bad args json at byte" in result, result
    assert 'near "' in result and ",," in result, result


def test_reading_a_directory_points_to_directory_tools(ctx):
    ctx.scenario('tool=read:{"path":"."},final_text=is+a+directory')
    s = ctx.spawn()
    s.submit("read it")
    s.wait_text("is a directory")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "is a directory; use find" in result, result
    assert "bash with ls" in result, result


def test_write_failure_includes_the_system_reason(ctx):
    args = json.dumps({"path": "missing/file.txt", "content": "x"})
    ctx.scenario(f"tool=write:{args},final_text=not+written")
    s = ctx.spawn()
    s.submit("write it")
    s.wait_text("not written")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "open missing/file.txt for write failed:" in result, result
    assert "No such file or directory" in result, result


def test_patch_keeps_a_file_that_ends_without_a_newline(ctx):
    """A hunk ending at EOF matches a last line the file never terminated."""
    ctx.write_file("tail.txt", "one\ntwo")
    diff = (
        "--- a/tail.txt\n+++ b/tail.txt\n@@ -1,2 +1,2 @@\n"
        " one\n-two\n+three\n"
    )
    ctx.scenario(patch_call(diff, final_text="done"))
    s = ctx.spawn()
    s.submit("patch the tail")
    s.wait_text("done")
    s.wait_turn_done()
    assert (ctx.work / "tail.txt").read_text() == "one\nthree"


def test_patch_creates_and_deletes_files(ctx):
    """/dev/null on either side is a file appearing or going away."""
    ctx.write_file("old.txt", "gone\n")
    diff = (
        "--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1,2 @@\n+fresh\n+lines\n"
        "--- a/old.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-gone\n"
    )
    ctx.scenario(patch_call(diff, final_text="swapped"))
    s = ctx.spawn()
    s.submit("replace the file")
    s.wait_text("swapped")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "new.txt created +2 -0" in result, result
    assert "old.txt deleted" in result, result
    assert (ctx.work / "new.txt").read_text() == "fresh\nlines\n"
    assert not (ctx.work / "old.txt").exists()


def test_patch_over_several_files_is_one_call(ctx):
    """Files of one change are one call, and the header says how many."""
    ctx.write_file("a.txt", "alpha\n")
    ctx.write_file("b.txt", "beta\n")
    diff = (
        "--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-alpha\n+ALPHA\n"
        "--- a/b.txt\n+++ b/b.txt\n@@ -1 +1 @@\n-beta\n+BETA\n"
    )
    ctx.scenario(patch_call(diff, final_text="both+applied"))
    s = ctx.spawn()
    s.submit("patch both")
    s.wait_text("both applied")
    s.wait_turn_done()

    assert (ctx.work / "a.txt").read_text() == "ALPHA\n"
    assert (ctx.work / "b.txt").read_text() == "BETA\n"
    assert "\u25c6  patch a.txt +1 more" in s.text(), s.text()


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


def test_bash_empty_output_is_not_described_as_a_bad_offset(ctx):
    args = json.dumps({"command": ":"})
    ctx.scenario(f"tool=bash:{args},final_text=ran+it")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("ran it")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "[command produced no output]" in result, result
    assert "offset 1 is past its end" not in result, result


def test_bash_tool_cannot_reach_the_terminal(ctx):
    """A tool command gets no controlling terminal, so /dev/tty will not open.

    Otherwise a `sudo` password prompt lands in the frame the TUI owns and
    its read races the composer for keystrokes.
    """
    args = json.dumps(
        {"command": "echo tty-marker > /dev/tty && echo TTY-WRITABLE || echo NO-TTY"}
    )
    ctx.scenario(f"tool=bash:{args},final_text=no+terminal")
    s = ctx.spawn()
    s.submit("try the terminal")
    s.wait_text("no terminal")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert any("NO-TTY" in r for r in results), results
    assert not any("TTY-WRITABLE" in r for r in results), results


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
    """While a tool runs the spinner row says which one, and the status line
    keeps its colour rather than repeating the word."""
    args = json.dumps({"command": "sleep 0.6; echo slept"})
    ctx.scenario(f"tool=bash:{args},final_text=finished")
    s = ctx.spawn()
    s.submit("run something slow")
    s.wait_activity("running bash")
    assert s.status_colour() == "working", s.status_line()
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
    """Verbose drops the transcript's caps; toggling it back restores them."""
    body = "\n".join(f"line {i:04d} of output" for i in range(40))
    ctx.write_file("big.txt", body)
    ctx.scenario('tool=read:{"path":"big.txt"},final_text=all+of+it')
    s = ctx.spawn()
    s.settings_toggle("Verbose tool output")
    s.submit("read big.txt")
    s.wait_text("all of it")
    s.wait_turn_done()

    text = s.text()
    assert "more lines" not in text, text
    assert "line 0039 of output" in text, text
    for _ in range(12):
        s.mouse("wheel-up", 5, 10).sync()
    assert "line 0000 of output" in s.text(), s.text()

    s.settings_toggle("Verbose tool output")
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
    marker = "x" * 1500
    args = json.dumps({"command": f"echo {marker}"})
    ctx.scenario(f"tool=bash:{args},final_text=ran")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("ran")
    s.wait_turn_done()
    assert " ..." in s.text(), s.text()

    s.settings_toggle("Verbose tool output")
    ctx.scenario(f"tool=bash:{args},tool_rounds=2,final_text=ran+again")
    s.submit("run it again")
    s.wait_text("ran again")
    s.wait_turn_done()
    assert " ..." not in s.text(), s.text()


def test_a_command_is_shown_past_one_row_of_bytes(ctx):
    """A command the header once cut at 120 bytes is now shown whole."""
    args = json.dumps({"command": "echo hi #" + "x" * 400})
    ctx.scenario(f"tool=bash:{args},final_text=ran")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("ran")
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
    """Verbose applies to the transcript, not only to the next tool call."""
    s = ctx.spawn()
    read_a_big_file(ctx, s)
    assert "\u25be 28 more lines" in s.text(), s.text()

    s.settings_toggle("Verbose tool output")
    text = s.text()
    assert "more lines" not in text, text
    assert "line 0039 of output" in text, text

    s.settings_toggle("Verbose tool output")
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


def test_a_cut_command_header_offers_the_rest_in_one_click(ctx):
    """A one-line command too long even for the command width is reachable."""
    args = json.dumps({"command": "echo hi #" + "x" * 1500})
    ctx.scenario(f"tool=bash:{args},final_text=ran")
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("ran")
    s.wait_turn_done()
    assert " ..." in s.text(), s.text()
    assert "\u25be show in full" in s.text(), s.text()

    click(s, "\u25be show in full")
    s.wait_text("\u25b4 show less")
    assert " ..." not in s.text(), s.text()

    click(s, "\u25b4 show less")
    s.wait_text("\u25be show in full")
    assert " ..." in s.text(), s.text()


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
    # a second turn leaves the tail room to stay on screen once scrolled
    ctx.scenario("final_text=and+more")
    s.submit("go on")
    s.wait_text("and more")
    s.wait_turn_done()
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


def test_an_ambiguous_hunk_is_refused(ctx):
    """Context matching twice names no hunk: patching the first is a coin toss."""
    ctx.write_file("twice.txt", "same line\nkeep\nsame line\n")
    diff = "--- a/twice.txt\n+++ b/twice.txt\n@@ -1 +1 @@\n-same line\n+changed\n"
    ctx.scenario(patch_call(diff, final_text="ambiguous"))
    s = ctx.spawn()
    s.submit("patch it")
    s.wait_text("ambiguous")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert result.startswith("ERROR:"), result
    assert "matches 2 places" in result, result
    assert (ctx.work / "twice.txt").read_text() == "same line\nkeep\nsame line\n"


def test_a_failing_hunk_leaves_every_file_untouched(ctx):
    """The patch is written once at the end, so it is all or nothing."""
    ctx.write_file("a.txt", "alpha\n")
    ctx.write_file("b.txt", "beta\n")
    diff = (
        "--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-alpha\n+ALPHA\n"
        "--- a/b.txt\n+++ b/b.txt\n@@ -1 +1 @@\n-nowhere\n+x\n"
    )
    ctx.scenario(patch_call(diff, final_text="nothing+changed"))
    s = ctx.spawn()
    s.submit("patch it")
    s.wait_text("nothing changed")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "b.txt hunk 1: context not found" in result, result
    assert (ctx.work / "a.txt").read_text() == "alpha\n"
    assert (ctx.work / "b.txt").read_text() == "beta\n"


def test_a_hunk_sees_what_the_hunk_before_it_left(ctx):
    """Hunks apply in order against the running file, not the original."""
    ctx.write_file("chain.txt", "one\ntail\n")
    diff = (
        "--- a/chain.txt\n+++ b/chain.txt\n"
        "@@ -1,2 +1,2 @@\n-one\n+two\n tail\n"
        "@@ -1,2 +1,2 @@\n-two\n+three\n tail\n"
    )
    ctx.scenario(patch_call(diff, final_text="chained"))
    s = ctx.spawn()
    s.submit("chain them")
    s.wait_text("chained")
    s.wait_turn_done()
    assert (ctx.work / "chain.txt").read_text() == "three\ntail\n"


def test_many_hunks_over_a_large_file_all_land(ctx):
    """The body is edited in place across hunks: every one must still apply.

    A hunk is located in what the hunks before it left, so an in-place edit
    has to leave the same bytes a full rebuild would. Sixty hunks spread over
    a thousand lines, each keyed by a unique line, catch an edit that wrote to
    a stale offset or lost the tail after a growth.
    """
    lines = [f"line {i:04d}" for i in range(1000)]
    ctx.write_file("wide.txt", "\n".join(lines) + "\n")
    targets = list(range(5, 1000, 17))
    hunks = "".join(
        f"@@ -{i} +{i} @@\n-line {i:04d}\n+LINE {i:04d} patched\n"
        for i in targets
    )
    diff = "--- a/wide.txt\n+++ b/wide.txt\n" + hunks
    ctx.scenario(patch_call(diff, final_text="all+applied"))
    s = ctx.spawn()
    s.submit("patch every one")
    s.wait_text("all applied")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert not result.startswith("ERROR:"), result
    assert f"+{len(targets)} -{len(targets)}" in result, result
    want = list(lines)
    for i in targets:
        want[i] = f"LINE {i:04d} patched"
    assert (ctx.work / "wide.txt").read_text() == "\n".join(want) + "\n"


def test_a_hunk_that_grows_the_file_keeps_the_tail(ctx):
    """Insertions past the buffer's reserve must not truncate what follows."""
    ctx.write_file("grow.txt", "head\nmiddle\ntail\n")
    added = "".join(f"+filler {i:03d}\n" for i in range(200))
    diff = (
        "--- a/grow.txt\n+++ b/grow.txt\n"
        "@@ -1,3 +1,203 @@\n head\n-middle\n" + added + " tail\n"
    )
    ctx.scenario(patch_call(diff, final_text="grown"))
    s = ctx.spawn()
    s.submit("grow it")
    s.wait_text("grown")
    s.wait_turn_done()

    body = (ctx.work / "grow.txt").read_text()
    want = "head\n" + "".join(f"filler {i:03d}\n" for i in range(200)) + "tail\n"
    assert body == want, body[:200]


def test_a_second_turn_still_gets_its_tool_call(ctx):
    """The mock's round budget is per turn, not per conversation.

    A budget spent conversation-wide made the second scenario in a session
    answer with plain text, so any case measuring or asserting a tool call
    after the first one quietly checked a turn that ran no tool at all.
    """
    ctx.write_file("one.txt", "first file\n")
    ctx.write_file("two.txt", "second file\n")
    ctx.scenario('tool=read:{"path":"one.txt"},final_text=read+one')
    s = ctx.spawn()
    s.submit("read one")
    s.wait_text("read one")
    s.wait_turn_done()

    ctx.scenario('tool=read:{"path":"two.txt"},final_text=read+two')
    s.submit("read two")
    s.wait_text("read two")
    s.wait_turn_done()

    # the second turn ran its own tool call rather than answering plain text
    tools = [m["content"] for m in ctx.mock.requests[-1]["messages"]
             if m.get("role") == "tool"]
    assert tools == ["first file\n", "second file\n"], tools


def test_find_respects_ignore_files_and_the_ignored_files_setting(ctx):
    """Tool walks and the @ picker share the Ignored files preference."""
    ctx.write_file(".gitignore", "__pycache__/\n*.log\n")
    ctx.write_file(".ignore", "*.tmp\n")
    ctx.write_file("keep.py", "kept\n")
    ctx.write_file("debug.log", "ignored\n")
    ctx.write_file("scratch.tmp", "ignored\n")
    ctx.write_file("bench/__pycache__/keep.cpython-314.pyc", "ignored\n")
    ctx.scenario('tool=find:{"name":"*"},final_text=first+walk')
    s = ctx.spawn()
    s.submit("list files")
    s.wait_text("first walk")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "keep.py" in result, result
    assert "debug.log" not in result, result
    assert "scratch.tmp" not in result, result
    assert "__pycache__" not in result, result

    s.settings_toggle("Ignored files")
    ctx.scenario('tool=find:{"name":"*"},final_text=second+walk')
    s.submit("list ignored files too")
    s.wait_text("second walk")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "debug.log" in result, result
    assert "scratch.tmp" in result, result
    assert "bench/__pycache__/keep.cpython-314.pyc" in result, result


def test_grep_respects_nested_ignore_files(ctx):
    """A grep walk applies each directory's ignore rules to its subtree."""
    ctx.write_file(".gitignore", "*.log\n")
    ctx.write_file("src/.ignore", "generated.c\n")
    ctx.write_file("keep.txt", "needle at root\n")
    ctx.write_file("debug.log", "needle in ignored log\n")
    ctx.write_file("src/main.c", "needle in source\n")
    ctx.write_file("src/generated.c", "needle in generated source\n")
    ctx.scenario('tool=grep:{"pattern":"needle"},final_text=searched')
    s = ctx.spawn()
    s.submit("search files")
    s.wait_text("searched")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[-1]
    assert "keep.txt" in result, result
    assert "src/main.c" in result, result
    assert "debug.log" not in result, result
    assert "src/generated.c" not in result, result


def test_a_read_header_names_the_page_it_asked_for(ctx):
    """Two reads of one file differ by their range, so the range is on screen."""
    ctx.write_file("big.txt", "\n".join(f"line {i:04d}" for i in range(400)))
    ctx.scenario(
        'tool=read:{"path":"big.txt","offset":100,"limit":3},'
        'tool=read:{"path":"big.txt","offset":398},'
        'tool_rounds=1,final_text=two+pages'
    )
    s = ctx.spawn()
    s.submit("read both pages")
    s.wait_text("two pages")
    s.wait_turn_done()

    text = s.text()
    assert "\u25c6  read big.txt lines 100-102" in text, text
    assert "\u25c6  read big.txt from line 398" in text, text


def test_a_read_of_the_whole_file_names_no_range(ctx):
    """A call that asked for no page has none to report."""
    ctx.write_file("notes.txt", "hello\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=read+it')
    s = ctx.spawn()
    s.submit("read notes.txt")
    s.wait_text("read it")
    s.wait_turn_done()
    text = s.text()
    assert "\u25c6  read notes.txt" in text, text
    assert "line" not in text.split("\u25c6  read notes.txt")[1].split("\n")[0], text


def test_a_long_turn_runs_to_its_end(ctx):
    """No round cap: a turn wide enough to outrun one still reaches its reply."""
    ctx.write_file("one.txt", "first file")
    ctx.scenario(
        'tool=read:{"path":"one.txt"},tool_rounds=40,final_text=all+the+way'
    )
    s = ctx.spawn()
    s.submit("keep reading")
    s.wait_text("all the way")
    s.wait_turn_done()
    # 40 rounds of tool calls, then the round that answered
    assert len(ctx.mock.requests) == 41, len(ctx.mock.requests)
    assert s.status_kind() == "ready"
    assert "stopped" not in s.text(), s.text()


def test_binary_tool_output_is_sent_as_valid_utf8(ctx):
    """A tool that prints raw bytes must not put them on the wire verbatim.

    A JSON body is UTF-8 by definition (RFC 8259), so a provider rejects a
    request carrying the tail of /dev/urandom. What the tool answers is
    replayed, so the sanitising belongs to the serialiser, not the tool.
    """
    args = json.dumps({"command": "head -c 4096 /dev/urandom"})
    ctx.scenario(f"tool=bash:{args},final_text=handled")
    s = ctx.spawn()
    s.submit("dump some bytes")
    s.wait_text("handled")
    s.wait_turn_done()

    assert ctx.mock.bad_utf8 == [], f"{len(ctx.mock.bad_utf8)} non-UTF-8 bodies"
    results = ctx.mock.tool_results()
    assert results, ctx.mock.requests
    results[-1].encode("utf-8")          # decoded cleanly, so it round-trips


def test_read_of_an_image_names_the_image(ctx):
    """read on a PNG says what the file is instead of paging its bytes.

    Every ill-formed byte becomes U+FFFD on the wire, so paging an image
    would charge the model for a page of replacement characters and tell it
    nothing about the file.
    """
    (ctx.work / "shot.png").write_bytes(png(1200, 800))
    ctx.scenario('tool=read:{"path":"shot.png"},final_text=that+is+a+picture')
    s = ctx.spawn()
    s.submit("read shot.png")
    s.wait_text("that is a picture")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert results and results[0].startswith("ERROR:"), results
    assert "shot.png is a png image, 1200x800" in results[0], results[0]
    assert "\ufffd" not in results[0], results[0]
    text = s.text()
    assert "read returns text" in text, text


def test_read_of_a_binary_file_is_refused(ctx):
    """A file with a NUL byte in its head is binary, so read points at bash."""
    (ctx.work / "a.out").write_bytes(b"\x7fELF\x02\x01\x01" + bytes(120))
    ctx.scenario('tool=read:{"path":"a.out"},final_text=not+text')
    s = ctx.spawn()
    s.submit("read a.out")
    s.wait_text("not text")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert results and results[0].startswith("ERROR:"), results
    assert "a.out is a binary file" in results[0], results[0]
    assert "Use bash" in results[0], results[0]


def test_read_of_utf8_text_still_pages(ctx):
    """The binary guard tests the head for a NUL, not for plain ASCII."""
    (ctx.work / "utf8.txt").write_text("h\u00e9llo \u2014 w\u00f6rld\n",
                                       encoding="utf-8")
    ctx.scenario('tool=read:{"path":"utf8.txt"},final_text=read+it')
    s = ctx.spawn()
    s.submit("read utf8.txt")
    s.wait_text("read it")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["h\u00e9llo \u2014 w\u00f6rld\n"], \
        ctx.mock.tool_results()
