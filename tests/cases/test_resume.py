"""Persistent sessions and the /resume picker.

Sessions are keyed by the directory arqan runs in, saved as they happen, and
browsed through the same popup slash-command completion uses.
"""

import json


def sessions_dir(ctx):
    """The per-cwd session directory under $HOME/.local/share."""
    root = ctx.home / ".local" / "share" / "arqan" / "sessions"
    dirs = [p for p in root.iterdir() if p.is_dir()] if root.exists() else []
    assert len(dirs) == 1, sorted(p.name for p in dirs)
    return dirs[0]


def record(ctx, prompt: str, reply: str):
    """Run one full turn in a fresh session and quit."""
    ctx.scenario(f"text={reply.replace(' ', '+')}")
    s = ctx.spawn()
    s.submit(prompt)
    s.wait_text(reply)
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()
    return s


def test_a_turn_is_saved_under_the_data_dir(ctx):
    """The conversation lands in $XDG_DATA_HOME/arqan/sessions/<cwd>/."""
    record(ctx, "remember this turn", "sure thing")

    d = sessions_dir(ctx)
    assert "%2fhome%2fwork" in d.name or d.name.endswith("work"), d.name
    files = sorted(d.iterdir())
    assert len(files) == 1 and files[0].name.endswith(".jsonl"), files
    lines = files[0].read_text().splitlines()
    assert json.loads(lines[0]) == {"type": "session", "title": ""}
    assert '"role":"user"' in lines[1] and "remember this turn" in lines[1]
    assert '"role":"assistant"' in lines[2] and "sure thing" in lines[2]


def test_a_turn_is_saved_round_by_round(ctx):
    """A tool round is on disk before the next request, not at the turn's end."""
    ctx.write_file("sample.txt", "kept bytes\n")
    args = json.dumps({"path": "sample.txt"})
    ctx.scenario(
        f"tool=read:{args},tool_rounds=2,final_text=done,hold_round=2"
    )
    s = ctx.spawn()
    s.submit("read it")
    s.wait_text("kept bytes")
    # The claim is about what is on disk by the time the next round is sent,
    # so wait for that request rather than for the result to be painted. The
    # round is held before it answers, so nothing else can reach the file
    # while the records below are read.
    s.wait_for(lambda t: len(ctx.mock.requests) == 2, "the next round")

    files = sorted(sessions_dir(ctx).iterdir())
    assert len(files) == 1, files
    lines = [json.loads(l) for l in files[0].read_text().splitlines()]
    assert lines[0] == {"type": "session", "title": ""}, lines[0]
    assert [l["role"] for l in lines[1:]] == [
        "user", "assistant", "assistant", "tool"
    ], lines[1:]
    assert lines[3]["name"] == "read", lines[3]
    assert "kept bytes" in lines[4]["content"], lines[4]

    ctx.mock.release()
    s.wait_turn_done()
    done = [json.loads(l) for l in files[0].read_text().splitlines()]
    assert done[:len(lines)] == lines, done
    assert done[-1] == {"role": "assistant", "content": "done"}, done[-1]


def test_resume_without_sessions_answers_in_the_popup_slot(ctx):
    """Nothing to browse is said where the popup would be, not in the view."""
    s = ctx.spawn()
    s.submit("/resume")
    s.wait_text("no saved sessions in this directory")
    # the popup's own row: immediately above the composer's padding row
    rows = s.screen.lines()
    assert "no saved sessions" in rows[s.screen.rows - 6], rows[s.screen.rows - 8 :]
    assert s.status_kind() == "ready", s.status_line()
    ctx.check_screen(s)


def test_resume_without_sessions_keeps_the_welcome_screen(ctx):
    """A command that opens nothing leaves the view exactly as it was."""
    s = ctx.spawn()
    assert "| (_| | | | (_| |" in s.text(), "the welcome art starts on screen"
    s.submit("/resume")
    s.wait_text("no saved sessions in this directory")
    assert "| (_| | | | (_| |" in s.text(), s.text()


def test_the_notice_survives_typing_and_goes_on_the_next_command(ctx):
    """It answers the last command, so the next one retires it, not a keypress."""
    ctx.scenario("text=hello+back")
    s = ctx.spawn()
    s.submit("/resume")
    s.wait_text("no saved sessions in this directory")

    s.type("hello").sync()
    assert "no saved sessions" in s.text(), "typing must not chase it away"
    assert s.composer_text() == "hello", s.composer_lines()

    s.key("enter")
    s.wait_gone("no saved sessions")
    s.wait_turn_done()


def test_esc_retires_the_notice(ctx):
    """Esc dismisses the popup first, then the notice above it."""
    s = ctx.spawn()
    s.submit("/resume")
    s.wait_text("no saved sessions in this directory")
    s.type("/").sync()
    assert "/clear" in s.text(), "the popup opens under the notice"

    s.key("esc").sync()
    assert "/clear" not in s.text(), "the popup goes first"
    assert "no saved sessions" in s.text(), s.text()
    s.key("esc").sync()
    assert "no saved sessions" not in s.text(), s.text()


def test_a_notice_stacks_above_the_popup(ctx):
    """Top to bottom: notice, popup, composer, status line."""
    s = ctx.spawn()
    s.submit("/resume")
    s.wait_text("no saved sessions in this directory")
    s.type("/").sync()

    rows = s.screen.lines()
    bottom = s.screen.rows
    assert "no saved sessions" in rows[bottom - 14], rows[bottom - 15 :]
    assert "/clear" in rows[bottom - 13], rows[bottom - 15 :]
    assert "/resume" in rows[bottom - 12], rows[bottom - 15 :]
    assert "/fork" in rows[bottom - 11], rows[bottom - 15 :]
    assert "/compact" in rows[bottom - 10], rows[bottom - 15 :]
    assert "/model" in rows[bottom - 9], rows[bottom - 15 :]
    assert "/provider" in rows[bottom - 8], rows[bottom - 15 :]
    assert "/mode" in rows[bottom - 7], rows[bottom - 15 :]
    assert "/rewind" in rows[bottom - 6], rows[bottom - 15 :]
    assert s.composer_text() == "/", s.composer_lines()
    ctx.check_screen(s, "stacked")


def test_resume_lists_and_replays_a_session(ctx):
    """A saved session is offered by timestamp and replayed into the view."""
    record(ctx, "what is arqan", "a tiny agent")

    s = ctx.spawn()
    assert "what is arqan" not in s.text(), "a new session starts empty"
    s.submit("/resume")
    s.wait_status("pick a session")
    assert "what is arqan" in s.text(), "the entry previews its first prompt"

    s.key("enter")
    s.wait_text("a tiny agent")
    s.wait_for(lambda t: t.contains("what is arqan"), "the prompt to be replayed")
    assert s.status_kind() == "ready", s.status_line()


def test_a_resumed_session_keeps_its_history(ctx):
    """The provider sees the replayed messages, not just the new one."""
    record(ctx, "first question", "first answer")

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("first answer")

    ctx.scenario("text=second+answer")
    s.submit("second question")
    s.wait_text("second answer")
    s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == [
        "system", "user", "assistant", "user"
    ], messages
    assert messages[1]["content"] == "first question"
    assert messages[2]["content"] == "first answer"
    assert messages[3]["content"] == "second question"


def test_a_resumed_session_keeps_appending_to_its_file(ctx):
    """Continuing a session writes into the file it came from."""
    record(ctx, "one", "two")
    d = sessions_dir(ctx)
    saved = sorted(d.iterdir())[0]

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("two")

    ctx.scenario("text=four")
    s.submit("three")
    s.wait_text("four")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    assert sorted(p.name for p in d.iterdir()) == [saved.name], "no second file"
    contents = saved.read_text()
    assert contents.count('"role":"user"') == 2, contents
    assert "three" in contents and "four" in contents


def test_the_picker_navigates_like_the_completion_popup(ctx):
    """Arrows and Ctrl-N/P move the highlight; Enter takes it."""
    record(ctx, "older session", "older reply")
    record(ctx, "newer session", "newer reply")

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    text = s.text()
    assert "older session" in text and "newer session" in text, text

    s.key("down").sync()          # newest is first, so this lands on the older
    s.key("ctrl-n").sync()        # wraps back to the newest
    s.key("ctrl-p").sync()        # and back down to the older one
    s.key("enter")
    s.wait_text("older reply")
    assert "newer reply" not in s.text(), s.text()


def test_the_list_is_ordered_by_last_use(ctx):
    """A session written to again is first, whenever it was started."""
    record(ctx, "older session", "older reply")
    record(ctx, "newer session", "newer reply")

    ctx.scenario("text=picked+up+again")
    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    assert "newer session" in s.popup_selected(), s.popup_selected()
    s.key("down")
    s.wait_for(lambda _: "older session" in s.popup_selected(),
               "the older session to be selected")
    s.key("enter")
    s.wait_text("older reply")
    s.submit("say more")
    s.wait_text("picked up again")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    # the file that was just appended to, though it was created first
    assert "older session" in s.popup_selected(), s.popup_selected()
    s.key("enter")
    s.wait_text("picked up again")


def test_esc_leaves_the_picker_without_resuming(ctx):
    """Cancelling keeps the empty session that was already running."""
    record(ctx, "not resumed", "not replayed")

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("esc")
    s.wait_status("ready")
    assert "not replayed" not in s.text(), s.text()
    assert "not resumed" not in s.text(), s.text()
    assert s.composer_text() == "", s.composer_lines()


def open_picker(s):
    """The session picker, open on its newest entry."""
    s.submit("/resume")
    return s.wait_status("pick a session")


def test_the_picker_says_which_key_deletes(ctx):
    """The hint is in the notice slot while the list is up, and not after."""
    record(ctx, "hinted session", "hinted reply")

    s = ctx.spawn()
    open_picker(s)
    assert "Ctrl-X deletes the selected session" in s.text(), s.text()
    s.key("esc")
    s.wait_status("ready")
    assert "Ctrl-X deletes" not in s.text(), s.text()


def test_ctrl_x_deletes_the_selected_session_on_the_second_press(ctx):
    """The first press asks, the second removes both the file and the row."""
    record(ctx, "throwaway session", "forget me")
    record(ctx, "kept session", "keep me")
    d = sessions_dir(ctx)
    assert len(sorted(d.iterdir())) == 2, sorted(p.name for p in d.iterdir())

    s = ctx.spawn()
    open_picker(s)
    s.key("down")                     # newest first, so this is the older one
    s.wait_for(lambda t: "throwaway session" in s.popup_selected(),
               "the older session to be selected")

    s.key("ctrl-x").sync()
    assert "Press Ctrl-X again to delete" in s.text(), s.text()
    assert len(sorted(d.iterdir())) == 2, "the first press only asks"
    assert "throwaway session" in s.text(), "and the row stays where it was"

    s.key("ctrl-x")
    s.wait_text("deleted session")
    # the notice is painted with the delete, the shorter list on the frame
    # after it, so the row leaving is its own wait
    s.wait_gone("throwaway session")
    files = sorted(d.iterdir())
    assert len(files) == 1, [p.name for p in files]
    assert "kept session" in files[0].read_text()

    # the list is still a list: the survivor resumes from where it now sits
    s.key("enter")
    s.wait_text("keep me")


def test_a_second_press_on_another_row_only_arms_that_row(ctx):
    """The arming belongs to the row it was asked on, not to the key."""
    record(ctx, "first session", "first reply")
    record(ctx, "second session", "second reply")
    d = sessions_dir(ctx)

    s = ctx.spawn()
    open_picker(s)
    s.key("ctrl-x").sync()
    assert "Press Ctrl-X again to delete" in s.text(), s.text()
    s.key("down").sync()
    s.key("ctrl-x").sync()
    assert len(sorted(d.iterdir())) == 2, "moving away re-asks"
    assert "Press Ctrl-X again to delete" in s.text(), s.text()


def test_esc_after_a_delete_reports_it_once_the_screen_closes(ctx):
    """The picker borrows the notice slot, so the count is said again."""
    record(ctx, "gone session", "gone reply")

    s = ctx.spawn()
    open_picker(s)
    s.key("ctrl-x").sync()
    s.key("ctrl-x")
    s.wait_status("ready")            # the last row went, so the screen closed
    s.wait_text("deleted 1 saved session")
    assert not list(sessions_dir(ctx).iterdir()), "the file is gone"

    s.submit("/resume")
    s.wait_text("no saved sessions in this directory")


def test_the_running_session_cannot_be_deleted(ctx):
    """The file being appended to is refused, and says what to do instead."""
    ctx.scenario("text=in+progress")
    s = ctx.spawn()
    s.submit("open a session")
    s.wait_turn_done()

    open_picker(s)
    s.key("ctrl-x").sync()
    s.key("ctrl-x")
    s.wait_text("that session is the one running")
    assert len(sorted(sessions_dir(ctx).iterdir())) == 1
    s.key("esc")
    s.wait_status("ready")


def test_sessions_are_scoped_to_the_directory(ctx):
    """A session recorded elsewhere is not offered here."""
    record(ctx, "in the work dir", "noted")

    other = ctx.home / "elsewhere"
    other.mkdir()
    s = ctx.spawn(cwd=str(other))
    s.submit("/resume")
    s.wait_text("no saved sessions in this directory")
    assert "in the work dir" not in s.text(), s.text()


def test_clear_starts_a_new_session_file(ctx):
    """/clear is a fresh conversation, so it is a fresh session too."""
    ctx.scenario("text=before")
    s = ctx.spawn()
    s.submit("first")
    s.wait_turn_done()
    s.submit("/clear")
    s.wait_gone("before")

    ctx.scenario("text=after")
    s.submit("second")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    files = sorted(sessions_dir(ctx).iterdir())
    assert len(files) == 2, [p.name for p in files]
    assert "first" in files[0].read_text()
    assert "second" in files[1].read_text()
    assert "first" not in files[1].read_text()


def test_tool_calls_survive_a_resume(ctx):
    """A session with tool traffic replays as a valid conversation."""
    ctx.write_file("notes.txt", "hello from disk\n")
    ctx.scenario('tool=read:{"path":"notes.txt"},final_text=I+read+it')
    s = ctx.spawn()
    s.submit("read the notes")
    s.wait_text("I read it")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("I read it")
    text = s.text()
    # the replay renders the block a live turn rendered, result included
    assert "\u25c6  read notes.txt" in text, text
    assert "\u2514\u2500 1 line" in text, text
    assert "hello from disk" in text, text

    # the replayed conversation still holds a tool result, which is what the
    # mock keys its follow-up reply off
    ctx.scenario("final_text=and+again")
    s.submit("thanks")
    s.wait_text("and again")
    s.wait_turn_done()
    roles = [m["role"] for m in ctx.mock.requests[-1]["messages"]]
    assert roles == [
        "system", "user", "assistant", "tool", "assistant", "user"
    ], roles


def test_sigint_cancels_the_picker(ctx):
    """Ctrl-C leaves the picker the way it abandons a draft at the prompt."""
    import signal

    record(ctx, "left alone", "untouched")

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.signal(signal.SIGINT)
    s.wait_status("ready")
    assert "untouched" not in s.text(), s.text()
    assert s.proc.poll() is None, "the session must survive the cancel"

    # and the turn that follows is not born interrupted
    ctx.scenario("text=carrying+on")
    s.submit("still here")
    s.wait_text("carrying on")
    s.wait_turn_done()
    assert "[interrupted]" not in s.text(), s.text()


def test_fork_without_a_conversation_answers_in_the_popup_slot(ctx):
    """There is nothing to copy before the first turn, and nothing is written."""
    s = ctx.spawn()
    s.submit("/fork")
    s.wait_text("nothing to fork yet")
    assert s.status_kind() == "ready", s.status_line()
    root = ctx.home / ".local" / "share" / "arqan" / "sessions"
    assert not root.exists() or not list(root.iterdir()), "no file is started"


def test_fork_continues_in_a_copy_and_leaves_the_original(ctx):
    """The turns so far are duplicated; later ones only reach the fork."""
    ctx.scenario("text=before+the+fork")
    s = ctx.spawn()
    s.submit("first")
    s.wait_turn_done()

    s.submit("/fork")
    s.wait_text("forked: this copy continues, the original is unchanged")
    assert "before the fork" in s.text(), "the transcript is untouched"

    ctx.scenario("text=after+the+fork")
    s.submit("second")
    s.wait_text("after the fork")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    files = sorted(sessions_dir(ctx).iterdir())
    assert len(files) == 2, [p.name for p in files]
    original, fork = (p.read_text() for p in files)
    assert "first" in original and "before the fork" in original
    assert "second" not in original and "after the fork" not in original
    assert "first" in fork and "before the fork" in fork
    assert "second" in fork and "after the fork" in fork


def test_a_fork_keeps_the_conversation_it_copied(ctx):
    """Only the file changes: the provider still sees the whole history."""
    ctx.scenario("text=one")
    s = ctx.spawn()
    s.submit("first question")
    s.wait_turn_done()
    s.submit("/fork")
    s.wait_text("forked")

    ctx.scenario("text=two")
    s.submit("second question")
    s.wait_text("two")
    s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == [
        "system", "user", "assistant", "user"
    ], messages
    assert messages[1]["content"] == "first question"
    assert messages[3]["content"] == "second question"


def test_a_failed_fork_keeps_appending_to_the_original(ctx):
    """A fork is not selected until its first checkpoint is durable."""
    ctx.scenario("text=first+reply")
    s = ctx.spawn()
    s.submit("first prompt")
    s.wait_turn_done()

    d = sessions_dir(ctx)
    original = sorted(d.glob("*.jsonl"))[0]
    mode = d.stat().st_mode
    d.chmod(0o555)
    try:
        s.submit("/fork")
        s.wait_text("could not start a forked session")
    finally:
        d.chmod(mode)

    ctx.scenario("text=second+reply")
    s.submit("second prompt")
    s.wait_turn_done()

    assert sorted(d.glob("*.jsonl")) == [original]
    contents = original.read_text()
    assert "first prompt" in contents and "second prompt" in contents


def test_a_fork_can_be_rewound_without_touching_the_original(ctx):
    """Going back in the copy leaves the session it was forked from whole."""
    ctx.scenario("text=first+reply")
    s = ctx.spawn()
    s.submit("keep me")
    s.wait_turn_done()
    s.submit("/fork")
    s.wait_text("forked")

    s.submit("/rewind")
    s.wait_status("rewind to a message")
    s.key("enter")
    s.wait_gone("first reply")
    assert s.composer_text() == "keep me", s.composer_lines()

    original = sorted(sessions_dir(ctx).iterdir())[0].read_text()
    assert "keep me" in original and "first reply" in original


def test_resume_offers_both_sides_of_a_fork(ctx):
    """Two sessions, so the one that was forked from is still reachable."""
    ctx.scenario("text=shared+start")
    s = ctx.spawn()
    s.submit("shared prompt")
    s.wait_turn_done()
    s.submit("/fork")
    s.wait_text("forked")

    ctx.scenario("text=only+in+the+fork")
    s.submit("branch prompt")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    rows = s.screen.lines()
    assert sum("shared prompt" in r for r in rows) == 2, rows[-12:]

    s.key("down").sync()          # newest first, so this lands on the original
    s.key("enter")
    s.wait_text("shared start")
    assert "only in the fork" not in s.text(), s.text()


def plant_session(ctx, text: str, name: str = "20240101-000000.jsonl"):
    """Write a session file directly, the way a crashed run leaves one."""
    import string

    unreserved = string.ascii_letters + string.digits + ".-_"
    slug = "".join(
        c if c in unreserved else f"%{ord(c):02x}" for c in str(ctx.work)
    )
    d = ctx.home / ".local" / "share" / "arqan" / "sessions" / slug
    d.mkdir(parents=True, exist_ok=True)
    p = d / name
    p.write_text(text)
    return p


def test_a_call_cut_off_before_it_ran_is_answered_on_resume(ctx):
    """A round saved before its tool ran replays as a valid conversation."""
    path = plant_session(ctx, "".join(
        json.dumps(m) + "\n" for m in [
            {"role": "user", "content": "read the notes"},
            {"role": "assistant", "calls": True, "content": ""},
            {"role": "assistant", "id": "call_1", "name": "read",
             "content": '{"path":"notes.txt"}'},
        ]
    ))

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("interrupted before this call ran")

    # the answer is on disk too, so the next resume finds the file whole
    lines = [json.loads(l) for l in path.read_text().splitlines()]
    assert [l["role"] for l in lines] == [
        "user", "assistant", "assistant", "tool"
    ], lines
    assert lines[3]["id"] == "call_1", lines[3]
    assert "interrupted" in lines[3]["content"], lines[3]

    # and the provider is sent a call every result follows, which it requires
    ctx.scenario("final_text=picking+up")
    s.submit("carry on")
    s.wait_text("picking up")
    s.wait_turn_done()
    roles = [m["role"] for m in ctx.mock.requests[-1]["messages"]]
    assert roles == ["system", "user", "assistant", "tool", "user"], roles


def test_one_call_answered_twice_replays_as_one_result(ctx):
    """Two runs appending to one file can answer a call twice.

    A second run that resumes a session whose last call is still in flight
    writes the placeholder for it, and the first run appends the real result
    when its tool finally returns. Both APIs refuse a second result for one
    call id, so the replay keeps one slot and the answer that arrived last.
    """
    path = plant_session(ctx, "".join(
        json.dumps(m) + "\n" for m in [
            {"role": "user", "content": "read the notes"},
            {"role": "assistant", "calls": True, "content": ""},
            {"role": "assistant", "id": "call_1", "name": "read",
             "content": '{"path":"notes.txt"}'},
            {"role": "tool", "id": "call_1",
             "content": "ERROR: interrupted before this call ran."},
            {"role": "tool", "id": "call_1", "content": "the notes, at last"},
        ]
    ))
    assert path.exists()

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("the notes, at last")

    ctx.scenario("final_text=picking+up")
    s.submit("carry on")
    s.wait_text("picking up")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    answers = [m for m in messages if m["role"] == "tool"]
    assert len(answers) == 1, messages
    assert answers[0]["content"] == "the notes, at last", answers[0]
    assert "interrupted" not in json.dumps(messages), messages


def test_a_torn_last_line_does_not_swallow_the_next_message(ctx):
    """A save cut mid-line is closed before the next append, not run onto."""
    path = plant_session(
        ctx,
        json.dumps({"role": "user", "content": "before the cut"}) + "\n"
        + '{"role":"assistant","content":"half a li',
    )

    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("before the cut")
    assert "half a li" not in s.text(), s.text()

    ctx.scenario("text=after+the+cut")
    s.submit("keep going")
    s.wait_text("after the cut")
    s.wait_turn_done()

    lines = path.read_text().splitlines()
    assert lines[1] == '{"role":"assistant","content":"half a li', lines
    kept = [json.loads(l) for l in lines[2:]]
    assert [l["content"] for l in kept] == ["keep going", "after the cut"], kept
