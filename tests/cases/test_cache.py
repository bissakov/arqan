"""The provider's prompt cache: what keeps it, and what is allowed to break it.

The Anthropic API matches a cached prefix from the first byte, so any rewrite
of text an earlier request already sent costs the whole prefix. Eliding and
compacting both rewrite; these cases pin how rarely they do it, where the
breakpoints that make a rewrite partial are placed, and that a rebuild nobody
asked for is reported rather than paid for in silence.
"""

import json


def numbered(lines):
    return "".join(f"line {i:04d}\n" for i in range(lines))


def windowed(ctx, window=1000, **env):
    """A model that declares a small window, with compaction off.

    The elision boundary only advances under context pressure, so a case
    about it has to declare a window and fill it. Compaction answers the same
    pressure and would rewrite the conversation out from under the case.
    """
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
                     ARQAN_API_KEY=None, **env)


def wire(ctx) -> str:
    return json.dumps(ctx.mock.requests[-1]["messages"])


def tool_results(ctx):
    return [m["content"] for m in ctx.mock.requests[-1]["messages"]
            if m["role"] == "tool"]


def paired(ctx):
    """The call ids one request carries, and the ids that answer them."""
    messages = ctx.mock.requests[-1]["messages"]
    calls = [c["id"] for m in messages if m.get("tool_calls")
             for c in m["tool_calls"]]
    answered = [m["tool_call_id"] for m in messages if m["role"] == "tool"]
    return calls, answered


def breakpoints(body):
    """Every cache_control in one Anthropic request, in wire order.

    The system parameter comes before the messages, so it sorts as message
    -1: what the API needs is that the blocks appear in conversation order,
    and that is the order this list has to be sorted in.
    """
    marks = []
    for i, blk in enumerate(body.get("system") or []):
        if isinstance(blk, dict) and blk.get("cache_control"):
            marks.append((-1, i))
    for mi, m in enumerate(body.get("messages") or []):
        content = m.get("content")
        if not isinstance(content, list):
            continue
        for bi, blk in enumerate(content):
            if isinstance(blk, dict) and blk.get("cache_control"):
                marks.append((mi, bi))
    return marks


def test_the_boundary_holds_across_rounds(ctx):
    """Twelve rounds, and no request rewrites what an earlier one sent.

    The boundary used to be recomputed from the conversation's length, so it
    moved on its own every fourth round and threw the cached prefix away to
    buy back four rounds of elision. Under no pressure it must not move at
    all: every request is the previous one with more on the end.
    """
    args = json.dumps({"command": "echo hello"})
    ctx.scenario(f"tool=bash:{args},tool_rounds=12,text=ok,final_text=done")
    s = ctx.spawn()
    s.submit("go")
    s.wait_text("done")
    s.wait_turn_done()

    sent = [r["messages"] for r in ctx.mock.requests]
    assert len(sent) == 13, len(sent)
    for n, (older, newer) in enumerate(zip(sent, sent[1:])):
        assert newer[:len(older)] == older, (
            f"request {n + 2} rewrote what request {n + 1} sent")


def test_the_valve_fires_once_and_then_holds(ctx):
    """Pressure moves the boundary; the turns after it leave it where it is.

    A second advance would rewrite the note the first one wrote, which is the
    cadence the sticky boundary exists to stop.
    """
    s = windowed(ctx)
    ctx.write_file("big.txt", numbered(400))
    ctx.scenario('tool=read:{"path":"big.txt"},final_text=read')
    s.submit("read big.txt")
    s.wait_text("read")
    s.wait_turn_done()

    ctx.scenario("text=sure,final_text=sure")
    s.submit("thanks")
    s.wait_turn_done()
    assert "line 0399" in wire(ctx), "two turns back is not old enough"

    s.submit("and again")
    s.wait_turn_done()
    assert "older read result elided" in wire(ctx), wire(ctx)[:400]
    fired = ctx.mock.requests[-1]["messages"]

    for prompt in ("once more", "still here"):
        s.submit(prompt)
        s.wait_turn_done()
        now = ctx.mock.requests[-1]["messages"]
        assert now[:len(fired)] == fired, "the boundary advanced again"


def test_prose_past_the_threshold_does_not_move_the_boundary(ctx):
    """A conversation carrying little tool traffic is compaction's problem.

    Under the gain guard the valve declines, so nothing declares the rebuild
    that follows and it is reported as the defect it would be.
    """
    s = windowed(ctx)
    ctx.scenario("words=300,usage=1000/10,cache_read=1000")
    for prompt in ("one", "two"):
        s.submit(prompt)
        s.wait_turn_done()

    ctx.scenario("words=300,usage=1000/10,cache_read=0")
    s.submit("three")
    s.wait_text("unexpected cache miss")
    s.wait_turn_done()
    assert "cache rebuilt after eliding" not in s.text(), s.text()


def test_arguments_are_stubbed_below_the_boundary(ctx):
    """A write's content is on disk; replaying it buys the model nothing."""
    s = windowed(ctx)
    content = "x" * 4000
    args = json.dumps({"path": "out.txt", "content": content})
    ctx.scenario(f"tool=write:{args},final_text=written")
    s.submit("write it")
    s.wait_text("written")
    s.wait_turn_done()

    ctx.scenario("text=sure,final_text=sure")
    s.submit("thanks")
    s.wait_turn_done()
    assert content in wire(ctx), "the newest rounds go out whole"

    s.submit("and again")
    s.wait_turn_done()
    body = wire(ctx)
    assert content not in body, body[:400]
    assert "older write arguments removed: " in body, body[:400]
    # A stub is still an object: args_object handling reads it either way.
    messages = ctx.mock.requests[-1]["messages"]
    stubbed = [json.loads(c["function"]["arguments"])
               for m in messages if m.get("tool_calls")
               for c in m["tool_calls"]]
    assert any("elided" in a for a in stubbed), stubbed


def test_a_call_that_repeats_the_stub_is_refused(ctx):
    """The stub records a call; it is not one the model may make again.

    It goes out in the arguments field, so a model can read it as the shape
    the tool takes and send it straight back. Running it would report a
    missing argument the model never meant to omit, so the answer names what
    those arguments actually are.
    """
    stub = json.dumps({"elided": "older bash arguments removed: 1467 bytes"})
    ctx.scenario(f"tool=bash:{stub},final_text=understood")
    s = ctx.spawn()
    s.submit("go")
    s.wait_text("understood")
    s.wait_turn_done()

    results = tool_results(ctx)
    assert len(results) == 1, results
    assert results[0].startswith("ERROR: "), results[0]
    assert "not an input" in results[0], results[0]
    assert "missing command" not in results[0], results[0]


def test_a_failed_call_is_stubbed_on_both_sides(ctx):
    """A refusal and the arguments that earned it both stop being replayed.

    Neither survives being read again: the change never happened, and the
    retry that followed is already in the conversation. The blocks stay,
    since a call without its result is a request the API rejects.
    """
    s = windowed(ctx)
    ctx.write_file("big.txt", numbered(400))
    ctx.scenario('tool=read:{"path":"missing.txt"},'
                 'tool=read:{"path":"big.txt"},final_text=tried')
    s.submit("read both")
    s.wait_text("tried")
    s.wait_turn_done()

    ctx.scenario("text=sure,final_text=sure")
    for prompt in ("thanks", "and again"):
        s.submit(prompt)
        s.wait_turn_done()

    results = tool_results(ctx)
    assert len(results) == 2, results
    assert results[0].startswith("[older read result elided:"), results[0]
    body = wire(ctx)
    assert "missing.txt" not in body, body[:600]
    # Only the failed call: the other one's arguments are a path, and a note
    # in place of a path is the larger half.
    assert body.count("older read arguments removed: ") == 1, body[:600]
    assert '"path\\":\\"big.txt' in body, body[:600]
    calls, answered = paired(ctx)
    assert calls and answered == calls, (calls, answered)


def test_the_request_never_carries_more_than_four_breakpoints(ctx):
    """Four is what the API accepts, and only in conversation order."""
    ctx.write_file("big.txt", numbered(400))
    args = json.dumps({"path": "big.txt"})
    ctx.scenario(f"tool=read:{args},tool_rounds=3,text=ok,final_text=done")
    s = windowed(ctx, ARQAN_API="anthropic")
    s.submit("read it")
    s.wait_text("done")
    s.wait_turn_done()
    ctx.scenario("text=sure,final_text=sure")
    for prompt in ("thanks", "and again", "once more"):
        s.submit(prompt)
        s.wait_turn_done()

    assert len(ctx.mock.requests) > 4, len(ctx.mock.requests)
    for n, body in enumerate(ctx.mock.requests):
        marks = breakpoints(body)
        assert marks, f"request {n + 1} caches nothing"
        assert len(marks) <= 4, (n + 1, marks)
        assert marks == sorted(marks), (n + 1, marks)
        assert len(set(marks)) == len(marks), (n + 1, marks)


def test_a_breakpoint_parks_below_the_elision_boundary(ctx):
    """What makes an advance partial: the head under the old boundary is
    byte-identical, so a breakpoint left there keeps it readable and the
    rewrite covers only the tail."""
    ctx.write_file("big.txt", numbered(400))
    args = json.dumps({"path": "big.txt"})
    ctx.scenario(f"tool=read:{args},final_text=read")
    s = windowed(ctx, ARQAN_API="anthropic")
    s.submit("read big.txt")
    s.wait_text("read")
    s.wait_turn_done()
    ctx.scenario("text=sure,final_text=sure")
    for prompt in ("thanks", "and again"):
        s.submit(prompt)
        s.wait_turn_done()

    body = ctx.mock.requests[-1]
    assert "older read result elided" in json.dumps(body["messages"]), body
    marks = breakpoints(body)
    assert len(marks) == 3, marks
    system, parked, newest = marks
    assert system[0] == -1, marks
    assert 0 <= parked[0] < newest[0], marks
    assert newest[0] == len(body["messages"]) - 1, marks


def test_an_unexplained_miss_stops_the_tool_loop(ctx):
    """Nothing declared a rewrite, so the rebuild is a defect: say where to
    report it and stop rather than pay for it every round."""
    ctx.scenario("text=hi,final_text=hi,usage=1000/10,cache_read=1000")
    s = ctx.spawn()
    s.submit("one")
    s.wait_turn_done()

    args = json.dumps({"command": "echo x"})
    ctx.scenario(f"tool=bash:{args},tool_rounds=5,text=ok,final_text=done,"
                 "usage=1000/10,cache_read=0")
    sent = len(ctx.mock.requests)
    s.submit("two")
    s.wait_text("unexpected cache miss")
    s.wait_turn_done()

    assert "arqan/issues" in s.text(), s.text()
    assert len(ctx.mock.requests) - sent == 1, (
        "the loop kept asking after the miss")
    assert "done" not in s.text(), s.text()


def test_a_rebuild_the_boundary_explains_does_not_stop_the_turn(ctx):
    """An advance is a price that was chosen: name it and carry on."""
    s = windowed(ctx)
    ctx.write_file("big.txt", numbered(400))
    ctx.scenario('tool=read:{"path":"big.txt"},final_text=read,'
                 'usage=1000/10,cache_read=1000')
    s.submit("read big.txt")
    s.wait_text("read")
    s.wait_turn_done()

    ctx.scenario("text=sure,final_text=sure,usage=1000/10,cache_read=1000")
    s.submit("thanks")
    s.wait_turn_done()

    ctx.scenario("text=sure,final_text=sure,usage=1000/10,cache_read=0")
    s.submit("and again")
    s.wait_text("cache rebuilt after eliding")
    s.wait_turn_done()
    assert "unexpected cache miss" not in s.text(), s.text()


def test_a_server_answering_from_an_older_prefix_is_not_a_defect(ctx):
    """A cache write that trails the request reads back a prefix an earlier
    request sent whole. The conversation only grows, so that prefix was never
    rewritten: say the server is behind and keep going.
    """
    ctx.scenario("text=hi,final_text=hi,usage=1000/10,cache_read=0")
    s = ctx.spawn()
    s.submit("one")
    s.wait_turn_done()

    ctx.scenario("text=hi,final_text=hi,usage=2000/10,cache_read=0")
    s.submit("two")
    s.wait_turn_done()

    args = json.dumps({"command": "echo x"})
    ctx.scenario(f"tool=bash:{args},tool_rounds=2,text=ok,final_text=done,"
                 "usage=3000/10,cache_read=1000")
    s.submit("three")
    s.wait_text("cache behind")
    s.wait_text("done")
    s.wait_turn_done()
    assert "unexpected cache miss" not in s.text(), s.text()


def missed(ctx, s):
    """One turn of tool rounds whose requests all read nothing back."""
    ctx.scenario("text=hi,final_text=hi,usage=1000/10,cache_read=1000")
    s.submit("one")
    s.wait_turn_done()

    args = json.dumps({"command": "echo x"})
    ctx.scenario(f"tool=bash:{args},tool_rounds=2,text=ok,final_text=done,"
                 "usage=1000/10,cache_read=0")
    sent = len(ctx.mock.requests)
    s.submit("two")
    return sent


def test_warn_reports_the_miss_without_stopping(ctx):
    """A session with no time to investigate still wants to be told: the row
    is the same one, minus the word that stopped the turn."""
    s = ctx.spawn(ARQAN_CACHE_GUARD="warn")
    sent = missed(ctx, s)
    s.wait_text("unexpected cache miss")
    s.wait_text("done")
    s.wait_turn_done()

    assert "stopped." not in s.text(), s.text()
    assert len(ctx.mock.requests) - sent > 1, (
        "the loop stopped at the miss")


def test_off_neither_reports_the_miss_nor_stops(ctx):
    """Off is for a session that has no answer to a bug report: the guard
    keeps measuring, and the turn runs as if nothing was rebuilt."""
    s = ctx.spawn(ARQAN_CACHE_GUARD="off")
    sent = missed(ctx, s)
    s.wait_text("done")
    s.wait_turn_done()

    assert "unexpected cache miss" not in s.text(), s.text()
    assert len(ctx.mock.requests) - sent > 1, (
        "the loop stopped at the miss")


def test_the_settings_screen_cycles_the_guard(ctx):
    """It is a row like every other setting, and the choice is remembered."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.open_settings()
    s.settings_select("Cache guard")
    assert s.settings_option("Cache guard") == "Stop", s.text()
    s.key("right").sync()
    s.wait_for(lambda t: s.settings_option("Cache guard") == "Warn",
               "the guard reporting without stopping")
    s.key("right").sync()
    s.wait_for(lambda t: s.settings_option("Cache guard") == "Off",
               "the guard off")

    s.key("escape").sync()
    assert ctx.state()["cache_guard"] == "off", ctx.state()
