"""/compact: one summarizing request, then a new session built from it.

The conversation is summarized over a copy, so only a summary that arrived
starts a new session; anything else leaves the session as it was.
"""


def sessions_dir(ctx):
    """The per-cwd session directory under $HOME/.local/share."""
    root = ctx.home / ".local" / "share" / "arqan" / "sessions"
    dirs = [p for p in root.iterdir() if p.is_dir()] if root.exists() else []
    assert len(dirs) == 1, sorted(p.name for p in dirs)
    return dirs[0]


def one_turn(ctx, prompt="remember the cat", reply="noted"):
    ctx.scenario(f"text={reply.replace(' ', '+')}")
    s = ctx.spawn()
    s.submit(prompt)
    s.wait_text(reply)
    s.wait_turn_done()
    return s


def test_compact_asks_for_a_checkpoint_over_the_whole_conversation(ctx):
    """The request carries the compaction prompt and the history it summarizes."""
    s = one_turn(ctx)

    ctx.scenario("text=##+Goal\\nShip+the+cat")
    s.submit("/compact")
    s.wait_text("compacted: a new session continues")

    req = ctx.mock.requests[-1]["messages"]
    assert req[0]["role"] == "system", req[0]
    assert "context checkpoint" in req[0]["content"], req[0]["content"]
    assert "## Critical Context" in req[0]["content"], req[0]["content"]
    assert [m["role"] for m in req[1:]] == ["user", "assistant", "user"], req
    assert req[1]["content"] == "remember the cat", req[1]
    assert req[2]["content"] == "noted", req[2]
    assert "context checkpoint" in req[-1]["content"], req[-1]


def test_compaction_starts_a_new_session_holding_the_summary(ctx):
    """The summary is the first message of the conversation that follows."""
    s = one_turn(ctx)

    ctx.scenario("text=##+Goal\\nShip+the+cat")
    s.submit("/compact")
    s.wait_text("compacted: a new session continues")
    text = s.text()
    assert "Ship the cat" in text, text
    assert "remember the cat" not in text, text
    assert "noted" not in text, text

    ctx.scenario("text=carrying+on")
    s.submit("what next?")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == ["system", "user", "user"], messages
    assert "context checkpoint" not in messages[0]["content"], messages[0]
    assert messages[1]["content"].startswith("# Context checkpoint"), messages[1]
    assert "Ship the cat" in messages[1]["content"], messages[1]
    assert messages[2]["content"] == "what next?", messages[2]


def test_compaction_leaves_the_original_session_file_alone(ctx):
    """The summary is appended to a new file; the old one keeps what it had."""
    s = one_turn(ctx)
    before = sorted(sessions_dir(ctx).iterdir())
    assert len(before) == 1, before
    original = before[0].read_text()

    ctx.scenario("text=##+Goal\\nShip+the+cat")
    s.submit("/compact")
    s.wait_text("compacted: a new session continues")
    s.submit("/exit")
    s.wait_exit()

    files = sorted(sessions_dir(ctx).iterdir())
    assert len(files) == 2, files
    assert files[0].read_text() == original, files[0].read_text()
    new = files[1].read_text()
    assert '"role":"user"' in new and "Ship the cat" in new, new
    assert "remember the cat" not in new, new


def test_failed_compaction_never_mutates_the_session(ctx):
    """A refused request costs the conversation and its file nothing."""
    s = one_turn(ctx)
    files_before = sorted(p.name for p in sessions_dir(ctx).iterdir())

    ctx.scenario("status=401")
    s.submit("/compact")
    s.wait_text("this session is unchanged")
    assert "remember the cat" in s.text(), s.text()

    assert sorted(p.name for p in sessions_dir(ctx).iterdir()) == files_before

    ctx.scenario("text=still+here")
    s.submit("and now?")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == [
        "system", "user", "assistant", "user"
    ], messages
    assert messages[1]["content"] == "remember the cat", messages[1]


def test_empty_summary_leaves_the_session_as_it_was(ctx):
    """A reply with no prose is not a checkpoint to continue from."""
    s = one_turn(ctx)

    ctx.scenario("text=")
    s.submit("/compact")
    s.wait_text("the model sent no summary")
    assert "remember the cat" in s.text(), s.text()


def test_compact_needs_something_to_compact(ctx):
    """An empty conversation has no request to make."""
    s = ctx.spawn()
    s.submit("/compact")
    s.wait_text("nothing to compact yet")
    assert ctx.mock.requests == [], ctx.mock.requests
