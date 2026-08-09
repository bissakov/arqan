"""Limits and hostile input: what happens at the edges of every fixed buffer.

Everything a provider streams is untrusted input (argument sizes, nesting
depth, how many calls arrive in one turn) and every one of those used to be
clamped silently or not at all. These cases pin the behaviour at the edge.
"""

import json


def test_oversized_bash_command_is_refused(ctx):
    """A command past the limit is rejected, never run truncated."""
    # Truncating a shell line changes the program it describes: cut this one
    # short and the `touch` at the front is all that survives and runs.
    padded = "touch ran-truncated.txt # " + "x" * 70000
    ctx.scenario(f"tool=bash:{json.dumps({'command': padded})},final_text=refused")
    s = ctx.spawn()
    s.submit("run something enormous")
    s.wait_text("refused")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[0]
    assert result.startswith("ERROR:"), result
    assert "command too long" in result, result
    assert not (ctx.work / "ran-truncated.txt").exists(), "the truncated command ran"


def test_oversized_path_is_refused(ctx):
    """A path past the limit is named as too long, not silently clipped."""
    long_path = "d/" * 3000 + "file.txt"
    ctx.scenario(f"tool=read:{json.dumps({'path': long_path})},final_text=too+long")
    s = ctx.spawn()
    s.submit("read a very long path")
    s.wait_text("too long")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[0]
    assert "path too long" in result, result


def test_deeply_nested_arguments_are_rejected(ctx):
    """Nesting is recursion: past the depth cap the arguments are refused."""
    depth = 200
    args = '{"path":' + "[" * depth + "]" * depth + "}"
    ctx.scenario(f"tool=read:{args},final_text=still+here")
    s = ctx.spawn()
    s.submit("send me something deep")
    # The point of the case is that yoke is still alive to answer at all.
    s.wait_text("still here")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[0]
    assert result.startswith("ERROR:"), result
    assert "bad args json" in result, result
    assert s.status_kind() == "ready"


def test_many_tool_calls_in_one_turn(ctx):
    """Every call in a wide turn runs, and each keeps its own id."""
    ctx.write_file("one.txt", "first file")
    count = 20
    calls = ",".join(f'tool=read:{{"path":"one.txt"}}' for _ in range(count))
    ctx.scenario(f"{calls},tool_rounds=1,final_text=all+of+them")
    s = ctx.spawn()
    s.submit("read it many times")
    s.wait_text("all of them")
    s.wait_turn_done()

    assert len(ctx.mock.tool_results()) == count, ctx.mock.tool_results()

    messages = ctx.mock.requests[-1]["messages"]
    assistant = next(m for m in messages if m.get("tool_calls"))
    ids = [c["id"] for c in assistant["tool_calls"]]
    assert len(ids) == count, ids
    assert len(set(ids)) == count, f"tool call ids are not distinct: {ids}"

    # every result names the call it answers, one to one
    answered = [m["tool_call_id"] for m in messages if m["role"] == "tool"]
    assert answered == ids, (answered, ids)


def test_full_conversation_is_reported_and_recoverable(ctx):
    """A conversation at capacity says so instead of writing past its arrays."""
    ctx.scenario("text=ok")
    s = ctx.spawn(YOKE_MAX_MESSAGES=8)

    # system + (user, assistant) per turn: the fourth user message is the one
    # that leaves no room for a reply.
    for i in range(3):
        s.submit(f"turn {i}")
        s.wait_turn_done()

    s.submit("one turn too many")
    s.wait_text("conversation is full")
    s.wait_turn_done()
    assert s.status_kind() == "ready"

    # /clear frees it again, and the next turn behaves normally
    s.submit("/clear")
    s.settle()
    s.submit("after clearing")
    s.wait_text("ok")
    s.wait_turn_done()
    assert "conversation is full" not in s.text()


def test_surrogate_pair_escapes_decode_to_one_glyph(ctx):
    """Astral characters arrive as \\uD83D\\uDE00 pairs and must survive it."""
    ctx.scenario("text=grinning+\U0001f600+done")
    s = ctx.spawn()
    s.submit("send an emoji")
    s.wait_text("done")
    s.wait_turn_done()
    assert "\U0001f600" in s.text(), s.text()


def test_turn_runs_without_an_api_key(ctx):
    """No key configured means no Authorization header, not a formatted NULL."""
    ctx.scenario("text=no+key+needed")
    s = ctx.spawn(YOKE_API_KEY=None)
    s.submit("hello")
    s.wait_text("no key needed")
    s.wait_turn_done()
    assert s.status_kind() == "ready"


def test_read_limit_above_the_cap_is_refused(ctx):
    """limit is a hard cap: 2000 lines is the ceiling, not a floor."""
    ctx.write_file("big.txt", "".join(f"line {i}\n" for i in range(10)))
    args = json.dumps({"path": "big.txt", "limit": 5000})
    ctx.scenario(f"tool=read:{args},final_text=refused")
    s = ctx.spawn()
    s.submit("read with a huge limit")
    s.wait_text("refused")
    s.wait_turn_done()
    result = ctx.mock.tool_results()[0]
    assert result.startswith("ERROR:"), result
    assert "limit must be a whole number in 1..2000" in result, result
