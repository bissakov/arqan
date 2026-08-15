"""Commands submitted while the assistant is working.

A turn holds the composer but not the session: the commands that touch
neither the conversation nor the request in flight run where they stand, and
the ones that would change what the agent is doing wait for the prompt.
"""

import json
import time

PLAN = "## Steps\n\n1. Read the file\n2. Change the line"


def running_turn(ctx, spec="first_delay=6,text=done"):
    """A session with a turn slow enough to type a command into."""
    ctx.scenario(spec)
    s = ctx.spawn()
    s.submit("go on")
    s.wait_activity("thinking")
    return s


def test_an_accepted_message_never_reads_ready(ctx):
    """Enter empties the composer and the same frame stops saying ready.

    Anything else puts a frame on screen showing an accepted message under an
    idle status, which reads as a message that went nowhere. It is also the
    only moment at which the turn is indistinguishable from a finished one,
    so a loaded machine that looks here sees a turn that never ran.
    """
    ctx.scenario("first_delay=6,text=done")
    s = ctx.spawn()
    s.submit("go on")
    assert s.status_kind() != "ready", s.snapshot("accepted")
    s.wait_activity("thinking")


def test_a_command_is_not_work(ctx):
    """A command is answered where it stands, so it never claims to be busy.

    The frame that takes a message says so because a turn follows it. Nothing
    follows a command, and a status left saying otherwise is one the next
    trip through the prompt has to take back.
    """
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("/title")
    assert s.status_kind() == "ready", s.status_line()


def test_settings_opens_while_a_turn_streams(ctx):
    """The screen comes up mid-turn and the reply arrives behind it."""
    s = running_turn(ctx)
    s.submit("/settings")
    s.wait_text("Verbose tool output")
    assert "Stream replies" in s.text(), s.text()
    s.wait_text("done")                 # the turn ran on under the screen
    s.key("esc").sync()
    s.wait_gone("Verbose tool output")
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 1, "the turn was not restarted"


def test_a_look_setting_applies_without_disturbing_the_reply(ctx):
    """Verbose tool output is a way of reading the transcript, not a request
    setting, so it changes mid-turn and the streamed reply survives it."""
    ctx.scenario('tool=bash:{"command":"echo hi"},first_delay=4,'
                 'final_text=finished')
    s = ctx.spawn()
    s.submit("run it")
    s.wait_text("echo hi")
    s.submit("/settings")
    s.wait_text("Verbose tool output")
    s.settings_select("Verbose tool output").key("enter").sync()
    s.wait_text("[x] Verbose tool output")
    s.key("esc").sync()
    s.wait_turn_done()
    assert "finished" in s.text(), s.text()
    assert "echo hi" in s.text(), "the tool call outlived the re-render"


def test_a_request_setting_is_refused_until_the_turn_ends(ctx):
    """Tools, mode, streaming and the token limits are what the running turn
    reads, so the screen says no rather than moving the agent's ground."""
    s = running_turn(ctx)
    s.submit("/settings")
    s.wait_text("Stream replies")
    s.settings_select("Stream replies").key("enter").sync()
    s.wait_text("shapes the request")
    assert "[x] Stream replies" in s.text(), "the setting changed anyway"
    s.key("esc").sync()
    s.wait_turn_done()
    # At the prompt the same row answers.
    s.open_settings()
    s.settings_select("Stream replies").key("enter").sync()
    s.wait_text("[ ] Stream replies")


def test_copy_takes_the_reply_the_last_turn_left(ctx):
    """Reading the conversation is not writing to it, so /copy answers while
    the next turn is still running."""
    ctx.scenario("text=first+answer")
    s = ctx.spawn()
    s.submit("say something")
    s.wait_text("first answer")
    s.wait_turn_done()
    ctx.scenario("first_delay=6,text=second")
    s.submit("say more")
    s.wait_activity("thinking")
    s.submit("/copy")
    s.wait_text("copied the last response")
    assert s.screen.clipboard == "first answer", repr(s.screen.clipboard)
    s.wait_turn_done()


def test_a_command_that_changes_the_turn_waits_for_the_prompt(ctx):
    """/clear would drop the conversation the turn is still writing to, so it
    is refused and handed back to the composer, ready for the next Enter."""
    s = running_turn(ctx)
    s.type("/clear")
    s.key("enter")
    s.wait_text("waits until the turn ends")
    s.wait_for(lambda t: s.composer_text() == "/clear", "the command to stay")
    s.wait_turn_done()
    s.key("enter")
    s.wait_gone("go on")


def test_an_unknown_command_says_so_mid_turn(ctx):
    """A line the agent never offers is answered the way the prompt answers
    it, rather than sitting there as if it had run."""
    s = running_turn(ctx)
    s.type("/nope")
    s.key("enter")
    s.wait_text("unknown command: /nope")
    s.wait_turn_done()


def test_a_message_submitted_mid_turn_runs_after_the_reply(ctx):
    """A follow-up is queued without interrupting the response in flight."""
    s = running_turn(ctx)
    s.type("and then this")
    s.key("enter")
    s.wait_text("message queued")
    assert s.composer_text() == "", s.composer_lines()
    s.wait_turn_done()
    assert len(ctx.mock.requests) == 2, ctx.mock.requests
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == [
        "system", "user", "assistant", "user"
    ]
    assert messages[-1]["content"] == "and then this"


def test_a_queued_message_joins_the_turn_after_its_tool(ctx):
    """Tool calls receive their results before queued steering reaches the
    provider, preserving the tool-call sequence on the wire."""
    ctx.scenario('tool=bash:{"command":"sleep 1; echo finished"},'
                 'final_text=steered')
    s = ctx.spawn()
    s.submit("start the work")
    s.wait_activity("running bash")
    s.submit("take a different direction")
    s.wait_text("message queued")
    s.wait_text("steered")
    s.wait_turn_done()

    # The fixed scenario asks for the same tool once in each user turn. The
    # request immediately after the first result is where the steering enters.
    messages = ctx.mock.requests[-2]["messages"]
    roles = [m["role"] for m in messages]
    assert roles == [
        "system", "user", "assistant", "tool", "user"
    ], roles
    assert messages[-1]["content"] == "take a different direction"


def test_escape_cancels_a_queued_message_without_interrupting(ctx):
    """The queued follow-up is independently cancellable while the original
    response keeps streaming."""
    s = running_turn(ctx)
    s.submit("do not send this")
    s.wait_text("message queued")
    s.key("esc")
    s.wait_text("queued message cancelled")
    s.wait_turn_done()

    assert len(ctx.mock.requests) == 1, ctx.mock.requests
    assert "[interrupted]" not in s.text(), s.text()


def test_shift_enter_does_not_interrupt_a_running_turn(ctx):
    """Shift-Enter edits the follow-up draft while the response continues."""
    s = running_turn(ctx)
    s.type("first line").sync()
    s.key("shift-enter").sync()
    s.type("second line").sync()
    s.wait_turn_done()

    assert s.composer_body(2) == ["first line", "second line"], \
        s.composer_lines(2)
    assert "[interrupted]" not in s.text(), s.text()
    assert len(ctx.mock.requests) == 1, ctx.mock.requests


def test_a_question_from_the_turn_takes_the_screen_back(ctx):
    """The agent loop is waiting on the answer, so a plan handover closes the
    screen the user left open rather than being refused by it."""
    ctx.scenario("first_delay=4,tool=submit_plan:"
                 + json.dumps({"plan": PLAN}))
    s = ctx.spawn()
    s.key("shift-tab")
    s.wait_for(lambda t: s.status_field(2) == "plan", "plan mode")
    s.submit("plan the change")
    s.wait_activity("thinking")
    s.submit("/settings")
    s.wait_text("Verbose tool output")
    s.wait_status("continue?")
    assert "Verbose tool output" not in s.text(), s.text()
    assert "Yes, but from a new session" in s.text(), s.text()


def test_a_screen_left_open_survives_the_end_of_the_turn(ctx):
    """The prompt waits behind it rather than reading the keys meant for it,
    so the row under the cursor is still the row being acted on."""
    s = running_turn(ctx)
    s.submit("/settings")
    s.wait_text("Verbose tool output")
    s.wait_text("done")                 # the turn ended under the screen
    s.settings_select("Display raw").key("enter").sync()
    s.wait_text("[x] Display raw")
    s.key("esc").sync()
    s.wait_gone("Verbose tool output")
    s.wait_turn_done()
    s.open_settings()
    assert "[x] Display raw" in s.text(), s.text()


def folded_read(ctx, delay=4):
    """A turn still running, with one folded block already on screen."""
    body = "\n".join(f"line {i:04d} of output" for i in range(40))
    ctx.write_file("big.txt", body)
    ctx.scenario('tool=read:{"path":"big.txt"},'
                 f'first_delay={delay},final_text=done')
    s = ctx.spawn()
    s.submit("read big.txt")
    s.wait_text("\u25be 28 more lines")
    return s


def click_tail(s, needle="\u25be 28 more lines"):
    row = s.screen.find_row(needle) + 1
    s.mouse("down", row, 6)
    return s.mouse("up", row, 6).sync()


def test_clicking_a_block_tail_mid_turn_opens_a_window_over_it(ctx):
    """The transcript belongs to the turn while it streams, so the block's
    own lines are shown over it rather than folded into it."""
    s = folded_read(ctx)
    click_tail(s)
    # The window opens on the first line the block itself did not show.
    s.wait_text("line 0012 of output")
    text = s.text()
    # The block is still up there, unchanged, under a window that covers the
    # transcript's last rows rather than rebuilding it.
    assert "\u25c6  read big.txt" in text, text
    assert "read output" in text, text
    assert "[x]" in text, text


def test_the_window_is_not_the_completion_picker(ctx):
    """The text view is a centered rectangle of its own and does not reuse
    the composer's command/picker rows."""
    s = folded_read(ctx)
    click_tail(s)
    s.wait_text("line 0012 of output")
    lines = s.screen.lines()
    top = next(i for i, line in enumerate(lines) if "┌" in line and "┐" in line)
    bottom = next(i for i, line in enumerate(lines[top + 1:], top + 1)
                  if "└" in line and "┘" in line)
    left = lines[top].index("┌")
    right = lines[top].index("┐")
    assert top > 0 and bottom < s.screen.rows - 1, s.text()
    assert left > 0 and right < s.screen.cols - 1, s.text()


def test_window_text_is_selectable_and_copyable(ctx):
    """Window body rows use the ordinary screen capture, selection and OSC
    52 clipboard path."""
    s = folded_read(ctx)
    click_tail(s)
    s.wait_text("line 0012 of output")
    row = s.screen.find_row("line 0012 of output") + 1
    col = s.screen.row_text(row - 1).index("line 0012 of output") + 1
    s.mouse("down", row, col)
    s.mouse("drag", row, col + len("line 0012 of output") - 1)
    s.mouse("up", row, col + len("line 0012 of output") - 1).sync()
    assert s.screen.clipboard == "line 0012 of output", repr(s.screen.clipboard)


def test_streaming_does_not_drop_a_window_selection(ctx):
    """Transcript bytes arriving behind the independent window do not move
    or clear a selection in its body."""
    s = folded_read(ctx, delay=8)
    click_tail(s)
    s.wait_text("line 0012 of output")
    row = s.screen.find_row("line 0012 of output") + 1
    col = s.screen.row_text(row - 1).index("line 0012 of output") + 1
    s.mouse("down", row, col)
    s.mouse("drag", row, col + len("line 0012 of output") - 1).sync()
    assert all(s.screen.attr_at(row - 1, c).reverse
               for c in range(col - 1,
                              col - 1 + len("line 0012 of output")))
    s.wait_turn_done()
    assert all(s.screen.attr_at(row - 1, c).reverse
               for c in range(col - 1,
                              col - 1 + len("line 0012 of output")))
    s.mouse("up", row, col + len("line 0012 of output") - 1).sync()
    assert s.screen.clipboard == "line 0012 of output", repr(s.screen.clipboard)


def test_the_window_closes_and_leaves_the_block_folded(ctx):
    """The window is a way of reading the block, not a change to it: the
    turn ends with the transcript exactly as the click found it."""
    s = folded_read(ctx)
    click_tail(s)
    s.wait_text("line 0012 of output")
    s.key("esc").sync()
    s.wait_gone("line 0012 of output")

    s.wait_turn_done()
    text = s.text()
    assert "\u25be 28 more lines" in text, text
    assert "\u25b4 show less" not in text, text


def test_the_visible_close_control_closes_the_window(ctx):
    """The title bar has a clickable close affordance in addition to Esc."""
    s = folded_read(ctx)
    click_tail(s)
    s.wait_text("line 0012 of output")
    row = s.screen.find_row("[x]") + 1
    col = s.screen.row_text(row - 1).index("[x]") + 2
    s.mouse("down", row, col)
    s.mouse("up", row, col).sync()
    s.wait_gone("line 0012 of output")


def test_the_window_scrolls_through_the_whole_block(ctx):
    """Every folded line is reachable, not just the ones the first page of
    the window happened to hold."""
    s = folded_read(ctx, delay=8)
    click_tail(s)
    s.wait_text("line 0012 of output")
    for _ in range(28):
        s.key("down")
    s.sync()
    assert "line 0039 of output" in s.text(), s.text()
    # A page stops at its ends rather than wrapping back to the first line.
    s.key("down", "down").sync()
    assert "line 0039 of output" in s.text(), s.text()


def test_the_window_keeps_the_full_tool_result(ctx):
    """A long block still exposes its final output; only the centered
    viewport is clipped, not the text it scrolls over."""
    body = "\n".join(["x"] * 1499 + ["LAST LINE"])
    ctx.write_file("long.txt", body)
    ctx.scenario('tool=read:{"path":"long.txt","limit":2000},'
                 'first_delay=4,final_delay=12,final_text=done')
    s = ctx.spawn()
    s.submit("read long.txt")
    s.wait_text("\u25be 1488 more lines")
    click_tail(s, "\u25be 1488 more lines")
    s.key("end").sync()
    assert "LAST LINE" in s.text(), s.text()


def test_the_window_keeps_the_tool_input_header_line(ctx):
    """A long tool input opens as the complete input, including the command
    line that the folded transcript summarizes in its header."""
    command = "printf first\\n\n" + "\n".join(
        f"# input {i:02d}" for i in range(20))
    ctx.scenario("tool=bash:" + json.dumps({"command": command})
                 + ",first_delay=4,final_text=done")
    s = ctx.spawn()
    s.submit("run a long command")
    s.wait_text("\u25be 12 more lines")
    click_tail(s, "\u25be 12 more lines")
    s.key("home").sync()
    assert "printf first" in s.text(), s.text()


def test_the_window_contains_a_shell_command_and_all_its_output(ctx):
    """A direct shell block is one window: its command is at the top and the
    last output line remains reachable at the bottom."""
    ctx.scenario("first_delay=8,text=done")
    s = ctx.spawn()
    command = "for i in $(seq 0 39); do printf 'shell %04d\\n' \"$i\"; done"
    s.submit("!" + command)
    s.wait_text("\u25be 28 more lines")
    click_tail(s)
    s.key("home").sync()
    assert command in s.text(), s.text()
    s.key("end").sync()
    assert "shell 0039" in s.text(), s.text()


def test_the_window_reflows_on_a_narrow_terminal(ctx):
    """The centered rectangle stays inset, wraps its text, and preserves its
    scroll range after a resize."""
    s = folded_read(ctx, delay=8)
    click_tail(s)
    s.wait_text("line 0012 of output")
    s.resize(42, 16).sync()
    lines = s.screen.lines()
    top = next(i for i, line in enumerate(lines) if "┌" in line and "┐" in line)
    left = lines[top].index("┌")
    right = lines[top].index("┐")
    assert top > 0 and left > 0 and right < s.screen.cols - 1, s.text()
    s.key("end").sync()
    assert "line 0039 of output" in s.text(), s.text()


def test_the_turn_streams_on_under_the_window(ctx):
    """A screen opened mid-turn is driven by the poll that keeps the turn
    alive, so the reply lands behind it."""
    s = folded_read(ctx)
    click_tail(s)
    s.wait_text("line 0012 of output")
    s.wait_turn_done()
    s.key("esc").sync()
    assert "done" in s.text(), s.text()


def test_the_window_does_not_flicker_behind_a_streaming_turn(ctx):
    """The window owns the rows it covers. A reply landing behind it neither
    erases nor repaints them, so the two never trade the same cells frame
    after frame."""
    body = "\n".join(f"line {i:04d} of output" for i in range(40))
    ctx.write_file("big.txt", body)
    reply = " ".join(f"w{i:03d}" for i in range(60))
    ctx.scenario('tool=read:{"path":"big.txt"},'
                 f'chunk=1,delay=0.03,final_text={reply}')
    s = ctx.spawn()
    s.submit("read big.txt")
    s.wait_text("\u25be 28 more lines")
    click_tail(s)
    s.wait_text("line 0012 of output")
    # The reply lands in the transcript rows the window covers, so the sample
    # holds the frames the turn kept painting while it was open.
    mark = len(s.raw)
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        s.pump(0.05)
    written = s.raw[mark:]
    assert b"\x1b[" in written, "the turn painted no frame at all"
    repaints = written.count(b"line 0012 of output")
    assert repaints == 0, f"the window repainted {repaints} times"

    s.wait_turn_done()
    s.key("esc").sync()
    assert "w059" in s.text(), "the turn was not streaming behind the window"


def test_the_window_outlives_a_turn_that_fails(ctx):
    """A stream that dies behind the window neither disturbs it nor loses
    what it reported: closing shows the error over the block, still folded."""
    body = "\n".join(f"line {i:04d} of output" for i in range(40))
    ctx.write_file("big.txt", body)
    ctx.scenario('tool=read:{"path":"big.txt"},chunk=1,delay=0.05,'
                 'abort_after=2,final_text=one two three four five')
    s = ctx.spawn()
    s.submit("read big.txt")
    s.wait_text("\u25be 28 more lines")
    click_tail(s)
    s.wait_text("line 0012 of output")
    s.wait_turn_done()
    assert "line 0024 of output" in s.text(), s.text()

    s.key("esc").sync()
    text = s.text()
    assert "[provider error:" in text, text
    assert "\u25be 28 more lines" in text, text
    s.type("still alive").sync()
    assert s.composer_text() == "still alive", s.composer_lines()


def test_a_question_the_turn_asks_takes_the_window(ctx):
    """The window is a way of reading, not a keyboard the agent can be stuck
    behind: an approval the next round needs closes it and is asked at once."""
    command = "for i in $(seq 0 39); do printf 'shell %04d\\n' \"$i\"; done"
    ctx.scenario("tool=bash:" + json.dumps({"command": command})
                 + ",tool_rounds=2,first_delay=2,final_text=done")
    s = ctx.spawn(ARQAN_PERMISSIONS="ask")
    s.submit("run it")
    s.wait_status("allow bash?")
    s.key("enter")
    s.wait_text("\u25be 28 more lines")
    click_tail(s)
    s.wait_text("shell 0012")

    s.wait_status("allow bash?")
    text = s.text()
    assert "Yes and remember" in text, text
    assert "shell 0012" not in text, "the window kept the question's keyboard"
    s.key("enter")
    s.wait_turn_done()
    assert "done" in s.text(), s.text()
