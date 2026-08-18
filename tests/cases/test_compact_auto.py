"""Automatic compaction: what happens as the context window fills.

The window is the user's to declare, so nothing here fires without a model
profile that names one. When it does, the older work becomes one checkpoint
message and the newest rounds are replayed exactly as they stand, so the
model keeps the detail it is working from.
"""

import signal

# A small window keeps the fixtures short: 30% of it is the tail budget, so a
# single reply of a few kilobytes is already more than compaction will keep.
WINDOW = 4000
# Past the 85% threshold, and past the reserve, which does not apply to a
# window smaller than it.
FULL = "usage=3600/10"
# A reply of roughly 6 KB, which is more than the tail budget on its own.
LONG = "words=1000"


def configure(ctx, window=WINDOW, **keys):
    """A provider with a declared context window, plus compaction settings."""
    lines = [f"{key} = {value}" for key, value in keys.items()]
    lines += [
        "[providers.work]",
        f"base_url = {ctx.mock.base_url}",
        "model = alpha",
    ]
    lines += ['[providers.work.models."alpha"]', f"context_window = {window}"]
    ctx.write_config("\n".join(lines) + "\n")
    state = ctx.state_file()
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text("provider = work\n")


def spawn(ctx, **env):
    return ctx.spawn(ARQAN_MODEL="alpha", ARQAN_BASE_URL=None,
                     ARQAN_API_KEY=None, **env)


def sessions_dir(ctx):
    root = ctx.home / ".local" / "share" / "arqan" / "sessions"
    dirs = [p for p in root.iterdir() if p.is_dir()] if root.exists() else []
    assert len(dirs) == 1, sorted(p.name for p in dirs)
    return dirs[0]


def three_turns(ctx, **env):
    """Two exchanges under a declared window, the first answered at length.

    The reported usage stays under the threshold until the second reply, so
    nothing is compacted while the fixture is being built; the long first
    reply is what the tail cannot afford to keep once it is.
    """
    ctx.scenario(f"{LONG},usage=1000/10")
    s = spawn(ctx, **env)
    s.submit("one")
    s.wait_turn_done()
    ctx.scenario(f"text=ok,{FULL}")
    s.submit("two")
    s.wait_turn_done()
    return s


def roles(request):
    return [m["role"] for m in request["messages"]]


def summaries(requests):
    return [r for r in requests
            if "context checkpoint" in r["messages"][0]["content"]]


def wait_compacted(ctx, s):
    """Compaction has happened once the summarizing request has been made.

    Not the notice: it is a banner the next streamed token clears, so a case
    that waits on it waits on a line that may never be on screen when the
    screen is read. What one turn compacts, `test_the_compaction_is_announced`
    holds still long enough to read.
    """
    s.wait_for(lambda _: summaries(ctx.mock.requests), "a summarizing request")
    s.wait_turn_done()


def test_a_full_window_summarizes_the_head_before_the_next_request(ctx):
    """The turn that crosses the threshold pays for one summarizing request,
    and the request it was about to make carries the summary instead of the
    conversation."""
    configure(ctx)
    s = three_turns(ctx)

    ctx.scenario(f"text=##+Goal\\nShip+the+cat,{FULL}")
    sent = len(ctx.mock.requests)
    s.submit("three")
    wait_compacted(ctx, s)

    made = ctx.mock.requests[sent:]
    assert len(made) == 2, [roles(r) for r in made]
    summarize, carry = made
    assert "context checkpoint" in summarize["messages"][0]["content"], \
        summarize["messages"][0]
    # The head only: the turn in flight is not what it was asked to condense.
    assert "three" not in str(summarize["messages"]), summarize["messages"]

    assert roles(carry) == ["system", "user", "user", "assistant", "user"], \
        carry["messages"]
    assert carry["messages"][1]["content"].startswith("# Context checkpoint")
    assert "Ship the cat" in carry["messages"][1]["content"]
    assert carry["messages"][-1]["content"] == "three", carry["messages"]


def test_the_recent_turns_are_replayed_word_for_word(ctx):
    """A summary carries the thread; the kept tail carries the detail."""
    configure(ctx)
    s = three_turns(ctx)

    ctx.scenario(f"text=summary,{FULL}")
    s.submit("three")
    wait_compacted(ctx, s)

    carried = ctx.mock.requests[-1]["messages"]
    assert carried[2]["content"] == "two", carried
    assert carried[3]["content"] == "ok", carried
    # The first exchange is gone: it is what the summary stands for, and its
    # long reply is what the tail had no room for.
    assert "one" not in [m["content"] for m in carried], carried


def test_the_compaction_is_announced(ctx):
    """The notice reaches the screen before the request it made room for.

    The summarizing request answers from the small model's own scenario, so
    only the request that carries the summary is held: that leaves the notice
    standing, since what wipes it is the next token of the transcript.
    """
    configure(ctx, compact_model="small", small_model="mock:text=summary")
    s = three_turns(ctx)

    ctx.scenario(f"text=ok,{FULL},hold=1")
    s.submit("three")
    s.wait_text("context compacted")
    ctx.mock.release()
    s.wait_turn_done()


def test_a_turn_of_tool_rounds_compacts_without_a_second_user_turn(ctx):
    """An autonomous turn is one user message and many rounds, so there are
    no turns to count. The rounds that fit the budget are kept whole and
    everything before them is summarized, mid-turn."""
    configure(ctx, compact_model="small", small_model="mock:text=summary")
    ctx.scenario('tool=bash:{"command":"seq 1 1500"},tool_rounds=9,' + FULL)
    s = spawn(ctx, ARQAN_PERMISSIONS="free")

    s.submit("go")
    s.wait_for(lambda _: any(carries_checkpoint(r) for r in ctx.mock.requests),
               "a request built from the checkpoint")
    carry = [r for r in ctx.mock.requests if carries_checkpoint(r)][0]
    s.signal(signal.SIGINT)
    s.wait_turn_done()

    assert summaries(ctx.mock.requests), [roles(r) for r in ctx.mock.requests]
    # The tail begins inside the turn, at the assistant message that opened a
    # round rather than at a user turn there is only one of.
    assert roles(carry)[:3] == ["system", "user", "assistant"], carry["messages"]
    assert carry["messages"][2].get("tool_calls"), carry["messages"][2]
    assert "go" not in [m.get("content") for m in carry["messages"]], \
        carry["messages"]
    assert_no_orphan_results(carry)


def carries_checkpoint(request):
    messages = request["messages"]
    return (len(messages) > 1 and messages[1].get("role") == "user"
            and str(messages[1].get("content", "")).startswith(
                "# Context checkpoint"))


def assert_no_orphan_results(request):
    """Every tool result names a call some earlier assistant message made."""
    declared = set()
    for m in request["messages"]:
        for call in m.get("tool_calls") or []:
            declared.add(call["id"])
        if m.get("role") == "tool":
            assert m["tool_call_id"] in declared, request["messages"]


def test_an_undeclared_window_never_compacts(ctx):
    """No model profile, no percentage to be past."""
    ctx.scenario("text=ok,usage=9000/10")
    s = ctx.spawn()
    for prompt in ("one", "two", "three"):
        s.submit(prompt)
        s.wait_turn_done()
    assert "compacted" not in s.text(), s.text()
    assert roles(ctx.mock.requests[-1]) == [
        "system", "user", "assistant", "user", "assistant", "user"
    ], ctx.mock.requests[-1]["messages"]


def test_off_leaves_a_full_window_alone(ctx):
    """The setting is the whole feature: off says nothing and changes
    nothing, even against a window it is over."""
    configure(ctx, compact="off")
    s = three_turns(ctx)

    ctx.scenario(f"text=ok,{FULL}")
    sent = len(ctx.mock.requests)
    s.submit("three")
    s.wait_turn_done()
    assert len(ctx.mock.requests[sent:]) == 1, ctx.mock.requests[sent:]
    assert "compact" not in s.text().lower(), s.text()


def test_manual_says_the_window_is_filling_and_waits(ctx):
    """The user asked to be told, not to be summarized."""
    configure(ctx, compact="manual")
    s = three_turns(ctx)

    # Held before it streams: the notice is said before the request, and the
    # first token of the reply is what clears it.
    ctx.scenario(f"text=ok,{FULL},hold=1")
    sent = len(ctx.mock.requests)
    s.submit("three")
    s.wait_text("/compact to continue from a summary")
    ctx.mock.release()
    s.wait_turn_done()
    assert len(ctx.mock.requests[sent:]) == 1, ctx.mock.requests[sent:]
    assert roles(ctx.mock.requests[-1]) == [
        "system", "user", "assistant", "user", "assistant", "user"
    ], ctx.mock.requests[-1]["messages"]


def test_a_threshold_the_conversation_is_under_is_not_crossed(ctx):
    """The percentage is the setting; a request well inside it is left be."""
    configure(ctx, window=100000)
    s = three_turns(ctx)

    ctx.scenario(f"text=ok,{FULL}")
    sent = len(ctx.mock.requests)
    s.submit("three")
    s.wait_turn_done()
    assert len(ctx.mock.requests[sent:]) == 1, ctx.mock.requests[sent:]


def test_a_conversation_the_window_dwarfs_is_not_worth_a_request(ctx):
    """The reserve fires where the percentage would not: a window with less
    room left than one tool round is full whatever the percentage says. What
    fills this one is not the conversation, though, and summarizing a
    conversation smaller than the tail would spend a request to free nothing,
    so it is said rather than done."""
    configure(ctx, window=200000, compact_at=95)
    ctx.scenario("text=ok,usage=185000/10,hold=1")
    s = spawn(ctx)
    ctx.mock.release()
    s.submit("one")
    s.wait_turn_done()

    ctx.mock.hold()
    sent = len(ctx.mock.requests)
    s.submit("two")
    # 185000 is under 95% of 200000, but over the 16384 held back for the
    # round that discovers the threshold.
    s.wait_text("nothing old enough to compact yet")
    ctx.mock.release()
    s.wait_turn_done()
    assert len(ctx.mock.requests[sent:]) == 1, ctx.mock.requests[sent:]
    assert roles(ctx.mock.requests[-1]) == [
        "system", "user", "assistant", "user"
    ], ctx.mock.requests[-1]["messages"]


def test_the_compacted_session_continues_in_a_new_file(ctx):
    """A session file is appended to, so the shortened conversation gets one
    of its own and the old file keeps every turn it recorded."""
    configure(ctx)
    s = three_turns(ctx)
    before = sorted(sessions_dir(ctx).iterdir())
    assert len(before) == 1, before

    ctx.scenario(f"text=summary,{FULL}")
    s.submit("three")
    wait_compacted(ctx, s)
    s.submit("/exit")
    s.wait_exit()

    files = sorted(sessions_dir(ctx).iterdir())
    assert len(files) == 2, files
    # Everything up to the compaction, including the turn that triggered it.
    old = files[0].read_text()
    assert '"content":"one"' in old and '"content":"three"' in old, old
    assert "# Context checkpoint" not in old, old
    new = files[1].read_text()
    assert "# Context checkpoint" in new, new
    assert '"content":"two"' in new, new
    assert '"content":"one"' not in new, new


def test_a_refused_summary_leaves_the_conversation_whole(ctx):
    """Compaction that cannot happen is reported; it never truncates on its
    own, because a conversation with nothing in front of it is worse than a
    long one."""
    configure(ctx)
    s = three_turns(ctx)

    ctx.scenario(f"text=,{FULL}")
    s.submit("three")
    s.wait_text("could not compact the context")
    s.wait_turn_done()
    assert roles(ctx.mock.requests[-1]) == [
        "system", "user", "assistant", "user", "assistant", "user"
    ], ctx.mock.requests[-1]["messages"]


def test_the_summarizing_request_can_go_to_the_small_model(ctx):
    """A cheap model is enough to write a checkpoint, and the setting says
    which model does."""
    configure(ctx, compact_model="small", small_model="tiny")
    s = three_turns(ctx)

    ctx.scenario(f"text=summary,{FULL}")
    sent = len(ctx.mock.requests)
    s.submit("three")
    wait_compacted(ctx, s)

    summarize, carry = ctx.mock.requests[sent:]
    assert summarize["model"] == "tiny", summarize["model"]
    assert carry["model"] == "alpha", carry["model"]


def test_the_screen_matches_the_conversation_after_compacting(ctx):
    """The transcript is rendered from the conversation, so what it shows is
    what the next request carries."""
    configure(ctx)
    s = three_turns(ctx)

    ctx.scenario(f"text=##+Goal\\nShip+the+cat,{FULL}")
    s.submit("three")
    wait_compacted(ctx, s)
    text = s.text()
    assert "Ship the cat" in text, text
    assert "two" in text, text


def test_compaction_is_attempted_once_per_turn(ctx):
    """A turn of many rounds pays for one summary attempt, not one a round:
    the threshold stays crossed until something is done about it, and asking
    again every round would spend the window rather than free it."""
    configure(ctx)
    s = three_turns(ctx, ARQAN_PERMISSIONS="free")

    ctx.scenario('tool=bash:{"command":"true"},tool_rounds=2,'
                 f"text=summary,final_text=done,{FULL}")
    sent = len(ctx.mock.requests)
    s.submit("three")
    s.wait_text("done")
    s.wait_turn_done()
    made = ctx.mock.requests[sent:]
    assert len(summaries(made)) == 1, [roles(r) for r in made]


def test_the_settings_screen_owns_every_part_of_it(ctx):
    """Mode, threshold and model are rows of /settings like everything else."""
    configure(ctx)
    ctx.scenario("text=ok")
    s = spawn(ctx)
    s.open_settings()
    s.settings_select("Compact context")
    assert s.settings_option("Compact context") == "Auto", s.text()
    s.key("right").sync()
    s.wait_for(lambda t: s.settings_option("Compact context") == "Off",
               "compaction turned off")

    s.settings_select("Compact with")
    assert s.settings_option("Compact with") == "Main", s.text()
    s.key("right").sync()
    s.wait_for(lambda t: s.settings_option("Compact with") == "Small",
               "the small model")

    s.settings_select("Compact at")
    assert "85%" in s.popup_selected(), s.popup_selected()
    s.key("right").sync()
    s.wait_for(lambda t: "90%" in s.popup_selected(), "a higher threshold")

    s.key("escape").sync()
    assert ctx.state()["compact"] == "off", ctx.state()
    assert ctx.state()["compact_at"] == "90", ctx.state()
    assert ctx.state()["compact_model"] == "small", ctx.state()
