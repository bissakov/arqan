"""Prompt history recall in the composer: Up/Down, drafts, persistence."""


def submit_all(ctx, s, *prompts):
    ctx.scenario("text=ok")
    for p in prompts:
        s.submit(p)
        s.wait_turn_done()


def test_up_recalls_the_last_prompt(ctx):
    """Up puts the previous prompt back in the composer."""
    s = ctx.spawn()
    submit_all(ctx, s, "first prompt")
    s.key("up").sync()
    assert s.composer_text() == "first prompt", s.composer_lines()


def test_up_walks_further_back(ctx):
    """Repeated Up steps through older prompts, newest first."""
    s = ctx.spawn()
    submit_all(ctx, s, "one", "two", "three")
    s.key("up").sync()
    assert s.composer_text() == "three", s.composer_lines()
    s.key("up").sync()
    assert s.composer_text() == "two", s.composer_lines()
    s.key("up").sync()
    assert s.composer_text() == "one", s.composer_lines()
    s.key("up").sync()
    assert s.composer_text() == "one", "oldest entry is the floor"


def test_down_returns_to_the_draft(ctx):
    """Walking back down past the newest entry restores the typed draft."""
    s = ctx.spawn()
    submit_all(ctx, s, "older")
    s.type("half-typed").sync()
    s.key("up").sync()
    assert s.composer_text() == "older", s.composer_lines()
    s.key("down").sync()
    assert s.composer_text() == "half-typed", s.composer_lines()


def test_down_without_recall_does_nothing(ctx):
    """Down on a fresh composer leaves the text alone."""
    s = ctx.spawn()
    submit_all(ctx, s, "older")
    s.type("typing").sync()
    s.key("down").sync()
    assert s.composer_text() == "typing", s.composer_lines()


def test_recalled_prompt_can_be_edited_and_sent(ctx):
    """A recalled entry is ordinary composer text."""
    s = ctx.spawn()
    submit_all(ctx, s, "count to 3")
    s.key("up").sync()
    s.key("end").sync()
    s.type("0").sync()
    assert s.composer_text() == "count to 30", s.composer_lines()
    ctx.scenario("text=ok")
    s.key("enter")  # send what the composer already holds
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["messages"][-1]["content"] == "count to 30"


def test_duplicate_prompts_are_not_repeated(ctx):
    """Submitting the same text twice keeps one entry."""
    s = ctx.spawn()
    submit_all(ctx, s, "same", "same")
    s.key("up").sync()
    assert s.composer_text() == "same", s.composer_lines()
    s.key("up").sync()
    assert s.composer_text() == "same", "no second copy to walk back to"


def test_history_survives_a_restart(ctx):
    """A new session recalls the previous one's prompts."""
    s = ctx.spawn()
    submit_all(ctx, s, "from the first session")
    s.submit("/exit")
    s.wait_exit()

    s2 = ctx.spawn()
    s2.key("up").sync()
    assert s2.composer_text() == "/exit", s2.composer_lines()
    s2.key("up").sync()
    assert s2.composer_text() == "from the first session", s2.composer_lines()


def test_multiline_prompt_round_trips(ctx):
    """A prompt with a line break survives the on-disk escaping."""
    s = ctx.spawn()
    ctx.scenario("text=ok")
    s.type("line one")
    s.key("newline")
    s.type("line two")
    s.key("enter")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    hist = ctx.home / ".local/state/yoke/history"
    assert hist.read_text().splitlines()[0] == "line one\\nline two"

    s2 = ctx.spawn()
    s2.key("up").sync()
    s2.key("up").sync()
    assert s2.composer_text(2) == "line oneline two", s2.composer_lines(2)


def test_oversized_file_is_trimmed_to_the_newest(ctx):
    """A file longer than the ring keeps its newest entries and is rewritten."""
    hist = ctx.home / ".local/state/yoke/history"
    hist.parent.mkdir(parents=True)
    hist.write_text("".join(f"prompt {i}\n" for i in range(600)))

    # The rewrite happens while loading, so the first frame is proof enough.
    s = ctx.spawn()
    s.key("up").sync()
    assert s.composer_text() == "prompt 599", s.composer_lines()

    lines = hist.read_text().splitlines()
    assert len(lines) == 500, len(lines)
    assert lines[0] == "prompt 100" and lines[-1] == "prompt 599", lines[:1] + lines[-1:]


def test_backslash_round_trips(ctx):
    """The on-disk escaping is reversible, backslashes included."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit(r"a\nb\\c")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    s2 = ctx.spawn()
    s2.key("up").sync()
    s2.key("up").sync()
    assert s2.composer_text() == r"a\nb\\c", s2.composer_lines()


def test_escaped_command_prefix_is_recalled_verbatim(ctx):
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit(r"\/clear")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["messages"][-1]["content"] == "/clear"
    s.key("up").sync()
    assert s.composer_text() == r"\/clear", s.composer_lines()


def test_up_still_drives_the_command_popup(ctx):
    """With the completion popup open, Up moves the selection, not history."""
    s = ctx.spawn()
    submit_all(ctx, s, "earlier prompt")
    s.type("/").sync()
    s.key("up").sync()
    assert s.composer_text() == "/", s.composer_lines()
