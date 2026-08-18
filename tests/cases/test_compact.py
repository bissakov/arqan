"""/compact: one summarizing request, then a new session built from it.

The conversation is summarized over a copy, so only a summary that arrived
starts a new session; anything else leaves the session as it was.

/compact keeps the same verbatim tail automatic compaction does: asking for
room should not cost the work in hand. A declared context window sizes that
tail; without one the conversation's own size does, so the newest work
survives a checkpoint on a model of unknown size too.
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


def windowed(ctx, window=4000):
    """A provider whose model declares a window, with automatic compaction
    off: what /compact does is the subject, not what fires on its own."""
    ctx.write_config(
        "compact = off\n"
        "[providers.work]\n"
        f"base_url = {ctx.mock.base_url}\n"
        "model = alpha\n"
        '[providers.work.models."alpha"]\n'
        f"context_window = {window}\n"
    )
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = work\n")
    return ctx.spawn(ARQAN_MODEL="alpha", ARQAN_BASE_URL=None,
                     ARQAN_API_KEY=None)


def unwindowed(ctx):
    """The same provider with no model profile: nothing declares a window,
    which is the shape of a model the profile table has never heard of."""
    ctx.write_config(
        "compact = off\n"
        "[providers.work]\n"
        f"base_url = {ctx.mock.base_url}\n"
        "model = alpha\n"
    )
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = work\n")
    return ctx.spawn(ARQAN_MODEL="alpha", ARQAN_BASE_URL=None,
                     ARQAN_API_KEY=None)


def test_compact_keeps_the_newest_work_where_a_window_sizes_it(ctx):
    """The tail is 30% of the declared window: a long first reply is more
    than that on its own, so it is what the summary stands for and the short
    exchange after it is replayed word for word."""
    s = windowed(ctx)
    ctx.scenario("words=1000")          # roughly 6 KB, past the tail budget
    s.submit("one")
    s.wait_turn_done()
    ctx.scenario("text=ok")
    s.submit("two")
    s.wait_turn_done()

    ctx.scenario("text=##+Goal\\nShip+the+cat")
    sent = len(ctx.mock.requests)
    s.submit("/compact")
    s.wait_text("compacted: a new session continues")

    # The head only: what is kept verbatim is not summarized as well.
    summarize = ctx.mock.requests[sent]["messages"]
    assert [m["role"] for m in summarize] == [
        "system", "user", "assistant", "user"
    ], summarize
    assert summarize[1]["content"] == "one", summarize[1]

    ctx.scenario("text=carrying+on")
    s.submit("what next?")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == [
        "system", "user", "user", "assistant", "user"
    ], messages
    assert messages[1]["content"].startswith("# Context checkpoint"), messages[1]
    assert "Ship the cat" in messages[1]["content"], messages[1]
    assert messages[2]["content"] == "two", messages[2]
    assert messages[3]["content"] == "ok", messages[3]


def test_compact_keeps_a_tail_when_the_whole_conversation_fits_the_budget(ctx):
    """A conversation far under the window's share is still cut, not swallowed.

    The tail budget is capped at half the conversation, so asking for a
    checkpoint early leaves the newest work standing instead of summarizing
    everything and losing the thread the request was meant to protect.
    """
    s = windowed(ctx, window=400000)    # 30% of it dwarfs anything below
    ctx.scenario("words=1000")
    s.submit("one")
    s.wait_turn_done()
    ctx.scenario("text=ok")
    s.submit("two")
    s.wait_turn_done()

    ctx.scenario("text=##+Goal\\nShip+the+cat")
    sent = len(ctx.mock.requests)
    s.submit("/compact")
    s.wait_text("compacted: a new session continues")

    summarize = ctx.mock.requests[sent]["messages"]
    assert [m["role"] for m in summarize] == [
        "system", "user", "assistant", "user"
    ], summarize
    assert summarize[1]["content"] == "one", summarize[1]

    ctx.scenario("text=carrying+on")
    s.submit("what next?")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == [
        "system", "user", "user", "assistant", "user"
    ], messages
    assert messages[1]["content"].startswith("# Context checkpoint"), messages[1]
    assert messages[2]["content"] == "two", messages[2]
    assert messages[3]["content"] == "ok", messages[3]


def test_compact_without_a_window_keeps_the_newest_work_verbatim(ctx):
    """Regression: a model that declares no window used to leave a tail of
    nothing, so /compact threw away the exchange in hand. The conversation's
    own size budgets the tail when the model's does not."""
    s = unwindowed(ctx)
    ctx.scenario("words=1000")
    s.submit("one")
    s.wait_turn_done()
    ctx.scenario("text=ok+the+cat+is+fed")
    s.submit("two")
    s.wait_turn_done()

    ctx.scenario("text=##+Goal\\nShip+the+cat")
    s.submit("/compact")
    s.wait_text("compacted: a new session continues from the summary "
                "and the newest work")
    s.sync()
    assert "Ship the cat" in s.text(), s.text()
    assert "ok the cat is fed" in s.text(), s.text()

    ctx.scenario("text=carrying+on")
    s.submit("what next?")
    s.wait_turn_done()
    messages = ctx.mock.requests[-1]["messages"]
    assert [m["role"] for m in messages] == [
        "system", "user", "user", "assistant", "user"], messages
    assert messages[1]["content"].startswith("# Context checkpoint"), messages[1]
    assert messages[2]["content"] == "two", messages[2]
    assert messages[3]["content"] == "ok the cat is fed", messages[3]


def test_compact_summarizes_a_single_exchange_whole(ctx):
    """One exchange has no older work to stand behind a checkpoint, so the
    cut that would split a question from its answer is not taken."""
    s = one_turn(ctx)

    ctx.scenario("text=##+Goal\\nShip+the+cat")
    s.submit("/compact")
    s.wait_text("compacted: a new session continues from the summary")
    assert "noted" not in s.text(), s.text()


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


def test_compaction_prompt_ships_headings_not_authoring_notes(ctx):
    """The optional sections are described by a rule, not by "(if relevant)"."""
    s = one_turn(ctx)

    ctx.scenario("text=##+Goal\\nShip+the+cat")
    s.submit("/compact")
    s.wait_text("compacted: a new session continues")

    prompt = ctx.mock.requests[-1]["messages"][0]["content"]
    assert "(if relevant)" not in prompt, prompt
    for heading in (
        "## Goal",
        "## Constraints & Preferences",
        "### Done",
        "### In Progress",
        "### Blocked",
        "## Key Decisions",
        "## Next Steps",
        "## Critical Context",
    ):
        assert f"{heading}\n" in prompt, (heading, prompt)
    assert "is optional" in prompt, prompt
    assert "leave it out entirely, heading included" in prompt, prompt


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
