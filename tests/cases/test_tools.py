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
    assert s.screen.attr_at(s.screen.find_row("\u2502 -old one"), 2).fg == 203
    assert s.screen.attr_at(s.screen.find_row("\u2502 +new one"), 2).fg == 114


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
    marker = "x" * 200
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
