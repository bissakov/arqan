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
    assert '"role":"user"' in lines[0] and "remember this turn" in lines[0]
    assert '"role":"assistant"' in lines[1] and "sure thing" in lines[1]


def test_a_turn_is_saved_round_by_round(ctx):
    """A tool round is on disk before the next request, not at the turn's end."""
    ctx.write_file("sample.txt", "kept bytes\n")
    args = json.dumps({"path": "sample.txt"})
    ctx.scenario(
        f"tool=read:{args},tool_rounds=2,final_text=done,first_delay=2"
    )
    s = ctx.spawn()
    s.submit("read it")
    s.wait_text("kept bytes")

    files = sorted(sessions_dir(ctx).iterdir())
    assert len(files) == 1, files
    lines = [json.loads(l) for l in files[0].read_text().splitlines()]
    assert [l["role"] for l in lines] == [
        "user", "assistant", "assistant", "tool"
    ], lines
    assert lines[2]["name"] == "read", lines[2]
    assert "kept bytes" in lines[3]["content"], lines[3]

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
