"""One live instance per session, read-only in the others.

A session is appended to by one process at a time. The instance that has it
live holds a lock under $XDG_STATE_HOME/arqan/locks/<cwd>/; another instance
that resumes the same transcript reads it and nothing more, until it forks a
copy of its own. Sessions are independent, so a second instance is free to
run its own.
"""

from tests.context import wait_until

from .test_resume import sessions_dir

READ_ONLY = "read-only"


def live(ctx, prompt="first question", reply="first answer"):
    """An instance with one turn saved, still running and still live."""
    ctx.scenario(f"text={reply.replace(' ', '+')}")
    s = ctx.spawn()
    s.submit(prompt)
    s.wait_text(reply)
    s.wait_turn_done()
    return s


def resume_newest(s):
    """Open the picker in `s` and take the newest session."""
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    return s


def session_file(ctx):
    files = [p for p in sessions_dir(ctx).iterdir() if p.name.endswith(".jsonl")]
    assert len(files) == 1, sorted(p.name for p in files)
    return files[0]


def test_a_second_instance_resumes_a_live_session_read_only(ctx):
    """The transcript is replayed, and the notice says it is read-only."""
    live(ctx)

    second = ctx.spawn()
    second.submit("/resume")
    second.wait_status("pick a session")
    assert "live elsewhere" in second.text(), second.text()

    second.key("enter")
    second.wait_text("first answer")
    second.wait_text(READ_ONLY)
    assert second.status_kind() == "ready", second.status_line()


def test_a_read_only_session_refuses_to_send(ctx):
    """The prompt is not sent, not rendered, and not appended to the file."""
    live(ctx)
    saved = session_file(ctx)
    before = saved.read_text()
    sent = len(ctx.mock.requests)

    second = ctx.spawn()
    resume_newest(second)
    second.wait_text("first answer")

    ctx.scenario("text=must+not+be+asked")
    second.submit("a question for the model")
    second.wait_text(READ_ONLY)
    second.settle()
    assert "a question for the model" not in second.text(), second.text()
    assert len(ctx.mock.requests) == sent, ctx.mock.requests[sent:]
    assert saved.read_text() == before, saved.read_text()


def test_a_read_only_session_refuses_a_shell_line(ctx):
    """`!` writes its output into the conversation, so it is refused too."""
    live(ctx)
    second = ctx.spawn()
    resume_newest(second)
    second.wait_text("first answer")

    second.submit("!echo unwelcome")
    second.wait_text(READ_ONLY)
    second.settle()
    assert "unwelcome" not in second.text(), second.text()


def test_a_read_only_session_refuses_to_compact_or_rename(ctx):
    """Both rewrite the session file, so both answer with the notice."""
    live(ctx)
    saved = session_file(ctx)
    before = saved.read_text()

    second = ctx.spawn()
    resume_newest(second)
    second.wait_text("first answer")

    second.submit("/compact")
    second.wait_text(READ_ONLY)
    second.submit("/title a name of its own")
    second.wait_text(READ_ONLY)
    second.settle()
    assert saved.read_text() == before, saved.read_text()


def test_clear_leaves_a_read_only_session_for_a_live_one(ctx):
    """/clear starts a session of this instance's own, which it may write."""
    live(ctx)
    second = ctx.spawn()
    resume_newest(second)
    second.wait_text(READ_ONLY)

    second.submit("/clear")
    second.wait_gone("first answer")

    ctx.scenario("text=answered+after+the+clear")
    second.submit("a question of my own")
    second.wait_text("answered after the clear")
    second.wait_turn_done()

    files = sorted(p for p in sessions_dir(ctx).iterdir()
                   if p.name.endswith(".jsonl"))
    assert len(files) == 2, [p.name for p in files]
    assert any("a question of my own" in p.read_text() for p in files)


def test_forking_a_read_only_session_continues_in_a_copy(ctx):
    """/fork writes a second file and leaves the live one alone."""
    first = live(ctx)
    original = session_file(ctx)
    before = original.read_text()

    second = ctx.spawn()
    resume_newest(second)
    second.wait_text("first answer")
    second.submit("/fork")
    second.wait_text("forked: this copy continues, the original is unchanged")

    ctx.scenario("text=answered+in+the+copy")
    second.submit("a question for the copy")
    second.wait_text("answered in the copy")
    second.wait_turn_done()

    assert original.read_text() == before, "the live session is untouched"
    files = sorted(p for p in sessions_dir(ctx).iterdir()
                   if p.name.endswith(".jsonl"))
    assert len(files) == 2, [p.name for p in files]
    fork = [p for p in files if p != original][0]
    assert "answered in the copy" in fork.read_text()

    ctx.scenario("text=still+answering+the+first")
    first.submit("and one more here")
    first.wait_text("still answering the first")
    first.wait_turn_done()
    assert "and one more here" in original.read_text()


def test_the_live_instance_keeps_writing_while_another_reads(ctx):
    """Reading a session from elsewhere costs the one that owns it nothing."""
    first = live(ctx)
    saved = session_file(ctx)

    second = ctx.spawn()
    resume_newest(second)
    second.wait_text(READ_ONLY)

    ctx.scenario("text=second+answer")
    first.submit("second question")
    first.wait_text("second answer")
    first.wait_turn_done()

    assert saved.read_text().count('"role":"user"') == 2, saved.read_text()
    assert "second answer" in saved.read_text()
    assert "second answer" not in second.text(), "the reader is a snapshot"


def test_the_session_goes_live_again_once_the_first_instance_leaves(ctx):
    """The lock is the running process, so its exit hands the session over."""
    first = live(ctx)
    first.submit("/exit")
    first.wait_exit()

    second = ctx.spawn()
    resume_newest(second)
    second.wait_text("first answer")
    second.settle()
    assert READ_ONLY not in second.text(), second.text()

    ctx.scenario("text=second+answer")
    second.submit("second question")
    second.wait_text("second answer")
    second.wait_turn_done()
    assert '"role":"user"' in session_file(ctx).read_text()
    assert "second answer" in session_file(ctx).read_text()


def test_resume_last_opens_a_live_session_read_only(ctx):
    """Starting into the newest session follows the same rule as /resume."""
    live(ctx)

    second = ctx.spawn(ARQAN_RESUME_LAST="true")
    second.wait_text("first answer")
    second.wait_text(READ_ONLY)


def test_a_live_session_is_not_deleted_from_another_instance(ctx):
    """Ctrl-X leaves a session another process is appending to."""
    live(ctx)
    saved = session_file(ctx)

    second = ctx.spawn()
    second.submit("/resume")
    second.wait_status("pick a session")
    second.key("ctrl-x").sync()
    assert "Press Ctrl-X again to delete" in second.text(), second.text()
    second.key("ctrl-x")
    second.wait_text("that session is live in another arqan")
    assert saved.exists(), "the file stays"


def test_two_instances_run_their_own_sessions(ctx):
    """Different sessions are unaffected: both stay live and both save."""
    first = live(ctx, "first question", "first answer")
    first_file = session_file(ctx)

    ctx.scenario("text=own+answer")
    second = ctx.spawn()
    second.submit("own question")
    second.wait_text("own answer")
    second.wait_turn_done()
    assert READ_ONLY not in second.text(), second.text()

    ctx.scenario("text=more+for+the+first")
    first.submit("more for the first")
    first.wait_text("more for the first")
    first.wait_turn_done()

    files = sorted(p for p in sessions_dir(ctx).iterdir()
                   if p.name.endswith(".jsonl"))
    assert len(files) == 2, [p.name for p in files]
    other = [p for p in files if p != first_file][0]
    assert "own question" in other.read_text()
    assert "own question" not in first_file.read_text()
    assert "more for the first" in first_file.read_text()


def test_two_starts_in_the_same_second_do_not_share_a_file(ctx):
    """A session path is reserved by its first append, not by its name."""
    first = ctx.spawn()
    second = ctx.spawn()

    ctx.scenario("text=first+answer")
    first.submit("first question")
    first.wait_text("first answer")
    first.wait_turn_done()

    ctx.scenario("text=second+answer")
    second.submit("second question")
    second.wait_text("second answer")
    second.wait_turn_done()
    assert READ_ONLY not in second.text(), second.text()

    files = sorted(p for p in sessions_dir(ctx).iterdir()
                   if p.name.endswith(".jsonl"))
    assert len(files) == 2, [p.name for p in files]
    bodies = [p.read_text() for p in files]
    assert sum("first question" in b for b in bodies) == 1, bodies
    assert sum("second question" in b for b in bodies) == 1, bodies
    for body in bodies:
        assert not ("first question" in body and "second question" in body)


def test_the_locks_live_beside_the_data_not_in_it(ctx):
    """The session directory holds transcripts; the lock is under state."""
    live(ctx)
    saved = session_file(ctx)

    names = sorted(p.name for p in sessions_dir(ctx).iterdir())
    assert names == [saved.name], names

    locks = ctx.home / ".local" / "share" / "arqan"
    assert not (locks / "locks").exists(), "not under the data directory"
    root = ctx.home / ".local" / "state" / "arqan" / "locks"
    held = wait_until(
        lambda: [p for d in root.iterdir() for p in d.iterdir()],
        "a lock file for the live session",
    )
    assert [p.name for p in held] == [saved.name + ".lock"], held


def test_a_start_that_saves_nothing_leaves_no_lock(ctx):
    """A session with no turn owns no file, so it takes no lock either."""
    s = ctx.spawn()
    s.submit("/exit")
    s.wait_exit()

    root = ctx.home / ".local" / "state" / "arqan" / "locks"
    assert not root.exists() or not [
        p for d in root.iterdir() for p in d.iterdir()
    ], "no lock file is started"
