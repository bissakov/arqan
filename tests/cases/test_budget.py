"""What a tool is allowed to spend of the context it shares.

A tool result is not a view: it is replayed to the provider on every later
turn of the session, so a whole file read once is billed many times. These
cases pin where each tool stops and what it says instead.
"""

import json


def run_tool(ctx, name, args, reply="done"):
    ctx.scenario(f"tool={name}:{json.dumps(args)},final_text={reply.replace(' ', '+')}")
    s = ctx.spawn()
    s.submit(f"use {name}")
    s.wait_text(reply)
    s.wait_turn_done()
    return ctx.mock.tool_results()[-1]


def numbered(lines, width=0):
    return "".join(f"line {i:04d}".ljust(width) + "\n" for i in range(lines))


def test_read_stops_at_the_line_cap_and_says_where_to_continue(ctx):
    """A long file comes back as a page naming the call that follows it."""
    ctx.write_file("long.txt", numbered(2500))
    result = run_tool(ctx, "read", {"path": "long.txt"})
    assert "line 0000" in result, result[:200]
    assert "line 0792" in result, result[-200:]
    assert "line 0793" not in result, result[-200:]
    assert "[read 793 of 2500 lines; continue with offset=794]" in result, result[-200:]


def test_read_takes_an_offset_and_a_limit(ctx):
    """The page the model asks for is the page it gets."""
    ctx.write_file("short.txt", numbered(100))
    result = run_tool(ctx, "read", {"path": "short.txt", "offset": 50, "limit": 3})
    assert result.startswith("line 0049\nline 0050\nline 0051\n"), result
    assert "[read 3 of 100 lines; continue with offset=53]" in result, result


def test_read_returns_a_short_file_whole_and_says_nothing(ctx):
    """A file that fits carries no note: the note would be the larger half."""
    ctx.write_file("tiny.txt", "one\ntwo\n")
    assert run_tool(ctx, "read", {"path": "tiny.txt"}) == "one\ntwo\n"


def test_read_stops_at_the_byte_cap_before_the_line_cap(ctx):
    """Few long lines cost as much as many short ones, and are capped too."""
    ctx.write_file("wide.txt", numbered(200, width=1000))
    result = run_tool(ctx, "read", {"path": "wide.txt"})
    assert len(result) < 8192, len(result)
    assert "continue with offset=8]" in result, result[-120:]


def test_read_past_the_end_says_how_long_the_file_is(ctx):
    """An offset with nothing behind it is answered, not returned empty."""
    ctx.write_file("short.txt", numbered(10))
    result = run_tool(ctx, "read", {"path": "short.txt", "offset": 99})
    assert result.startswith("ERROR:"), result
    assert "short.txt has 10 lines, offset 99 is past its end" in result, result


def test_read_refuses_a_nonsense_offset(ctx):
    """A fractional line is a mistake worth naming rather than rounding."""
    ctx.write_file("short.txt", numbered(10))
    result = run_tool(ctx, "read", {"path": "short.txt", "offset": 2.5})
    assert "offset must be a whole number" in result, result


def test_bash_pages_a_flood_by_output_byte_range(ctx):
    """A shell flood is a bounded page that names the next byte range."""
    result = run_tool(ctx, "bash", {"command": "seq 1 200000", "limit": 100})
    assert result.startswith("1\n2\n3\n"), result[:80]
    assert "[read 100 of " in result, result[-160:]
    assert "continue with offset=101]" in result, result[-160:]
    assert result.rstrip().endswith("[exit 0]"), result[-80:]

    page = run_tool(ctx, "bash", {"command": "printf abcdef", "offset": 4, "limit": 2})
    assert page.startswith("de[read 2 of 6 output bytes; continue with offset=6]"), page
    assert page.rstrip().endswith("[exit 0]"), page


def test_short_command_output_is_untouched(ctx):
    """Nothing is said about a cap that did not bite."""
    result = run_tool(ctx, "bash", {"command": "echo hello"})
    assert result == "hello\n\n[exit 0]", repr(result)


def windowed(ctx, window=1000, **env):
    """A model that declares a small window, with compaction off.

    Eliding answers context pressure, so a case about the boundary has to
    declare a window and fill it. Compaction is the other answer to the same
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


def big_run(ctx, s, reply, lines=400):
    """A big result from a tool the boundary keeps.

    A read is dropped whole below the boundary rather than elided, so a case
    about the note has to use a call the model cannot simply make again.
    """
    ctx.write_file("big.txt", numbered(lines))
    ctx.scenario(
        'tool=bash:{"command":"cat big.txt"},'
        f'final_text={reply.replace(" ", "+")}'
    )
    s.submit("cat big.txt")
    s.wait_text(reply)
    s.wait_turn_done()


def tool_messages(ctx):
    return [m for m in ctx.mock.requests[-1]["messages"] if m["role"] == "tool"]


def test_an_old_tool_result_is_elided_on_the_wire(ctx):
    """A result older than the last two user turns costs a line, not a file."""
    s = windowed(ctx)
    big_run(ctx, s, "that is a lot")

    ctx.scenario("text=sure,final_text=sure")
    s.submit("thanks")
    s.wait_turn_done()
    assert "line 0079" in tool_messages(ctx)[0]["content"], tool_messages(ctx)

    s.submit("and again")
    s.wait_turn_done()
    content = tool_messages(ctx)[0]["content"]
    assert content.startswith("[older bash result elided:"), content
    assert "line 0079" not in content, content

    # what the reader sees is unchanged: the transcript renders Conv, not the wire
    assert "line 0011" in s.text(), s.text()


def test_a_small_result_is_never_elided(ctx):
    """Under the threshold, saying it was elided costs more than sending it."""
    ctx.write_file("big.txt", numbered(400))
    ctx.scenario('tool=bash:{"command":"echo hello from disk"},'
                 'tool=bash:{"command":"cat big.txt"},final_text=read+it')
    s = windowed(ctx)
    s.submit("run both")
    s.wait_text("read it")
    s.wait_turn_done()

    ctx.scenario("text=sure,final_text=sure")
    for prompt in ("thanks", "and again"):
        s.submit(prompt)
        s.wait_turn_done()
    results = [m["content"] for m in tool_messages(ctx)]
    # The big result beside it is what moved the boundary over both.
    assert results[1].startswith("[older bash result elided:"), results
    assert "hello from disk" in results[0], results


def test_an_elided_result_keeps_answering_its_call(ctx):
    """The message stays a tool result: a call left unanswered breaks the turn."""
    s = windowed(ctx)
    big_run(ctx, s, "plenty")
    ctx.scenario("text=sure,final_text=sure")
    for prompt in ("thanks", "and again"):
        s.submit(prompt)
        s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    calls = [c["id"] for m in messages if m.get("tool_calls") for c in m["tool_calls"]]
    answered = [m["tool_call_id"] for m in messages if m["role"] == "tool"]
    assert calls and answered == calls, (calls, answered)
    assert any(m["content"].startswith("[older bash result elided:")
               for m in tool_messages(ctx)), tool_messages(ctx)


def test_results_older_than_the_last_rounds_are_elided_inside_one_turn(ctx):
    """Age is rounds as well as turns.

    A turn that works on its own produces one user message and many rounds,
    so a rule counting user turns never reaches back over any of it and every
    result is replayed whole to the end of the run. The boundary moves a
    block of four rounds at a time, so at nine rounds the first four are a
    line each and the five that follow are sent as they stand.
    """
    args = json.dumps({"command": "seq 1 300"})
    ctx.scenario(f"tool=bash:{args},tool_rounds=9,text=ok,final_text=done")
    s = windowed(ctx, ARQAN_PERMISSIONS="free")
    s.submit("go")
    s.wait_text("done")
    s.wait_turn_done()

    results = [m["content"] for m in tool_messages(ctx)]
    assert len(results) == 9, results
    elided = [r for r in results if r.startswith("[older bash result elided:")]
    assert len(elided) == 4, results
    # The oldest, and only the oldest: the newest block is what the model is
    # working from.
    assert results[:4] == elided, results
    assert all("300\n" in r for r in results[4:]), results[4:]

    # A note is not an answer that went missing.
    messages = ctx.mock.requests[-1]["messages"]
    calls = [c["id"] for m in messages if m.get("tool_calls") for c in m["tool_calls"]]
    answered = [m["tool_call_id"] for m in messages if m["role"] == "tool"]
    assert calls and answered == calls, (calls, answered)


def test_a_run_under_two_blocks_of_rounds_elides_nothing(ctx):
    """The boundary moves in blocks so the provider's prefix cache still hits.

    Eliding one result per round would rewrite the prefix every round and
    throw the cache away each time. Under two blocks there is nothing old
    enough to be worth that.
    """
    args = json.dumps({"command": "seq 1 300"})
    ctx.scenario(f"tool=bash:{args},tool_rounds=6,text=ok,final_text=done")
    s = windowed(ctx, ARQAN_PERMISSIONS="free")
    s.submit("go")
    s.wait_text("done")
    s.wait_turn_done()

    results = [m["content"] for m in tool_messages(ctx)]
    assert len(results) == 6, results
    assert all("300\n" in r for r in results), results
