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
    assert "line 1999" in result, result[-200:]
    assert "line 2000" not in result, result[-200:]
    assert "[read 2000 of 2500 lines; continue with offset=2001]" in result, result[-200:]


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
    assert len(result) < 60000, len(result)
    assert "continue with offset=52]" in result, result[-120:]


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


def test_bash_keeps_the_tail_of_a_flood(ctx):
    """A command says why it failed on its last lines, not its first."""
    result = run_tool(ctx, "bash", {"command": "seq 1 200000"})
    assert result.startswith("[output truncated: last 51200 of "), result[:80]
    assert result.rstrip().endswith("[exit 0]"), result[-80:]
    assert "\n200000\n" in result, result[-80:]
    assert "\n1\n2\n3\n" not in result, result[:200]
    assert len(result) < 52000, len(result)


def test_short_command_output_is_untouched(ctx):
    """Nothing is said about a cap that did not bite."""
    result = run_tool(ctx, "bash", {"command": "echo hello"})
    assert result == "hello\n\n[exit 0]", repr(result)


def big_read(ctx, s, reply):
    ctx.write_file("big.txt", numbered(80))
    ctx.scenario(
        f'tool=read:{{"path":"big.txt"}},final_text={reply.replace(" ", "+")}'
    )
    s.submit("read big.txt")
    s.wait_text(reply)
    s.wait_turn_done()


def tool_messages(ctx):
    return [m for m in ctx.mock.requests[-1]["messages"] if m["role"] == "tool"]


def test_an_old_tool_result_is_elided_on_the_wire(ctx):
    """A result older than the last two user turns costs a line, not a file."""
    s = ctx.spawn()
    big_read(ctx, s, "that is a lot")

    ctx.scenario("text=sure,final_text=sure")
    s.submit("thanks")
    s.wait_turn_done()
    assert "line 0079" in tool_messages(ctx)[0]["content"], tool_messages(ctx)

    s.submit("and again")
    s.wait_turn_done()
    content = tool_messages(ctx)[0]["content"]
    assert content.startswith("[read result elided after 2 turns:"), content
    assert "line 0079" not in content, content

    # what the reader sees is unchanged: the transcript renders Conv, not the wire
    assert "line 0011" in s.text(), s.text()


def test_a_small_result_is_never_elided(ctx):
    """Under the threshold, saying it was elided costs more than sending it."""
    ctx.write_file("tiny.txt", "hello from disk\n")
    ctx.scenario('tool=read:{"path":"tiny.txt"},final_text=read+it')
    s = ctx.spawn()
    s.submit("read tiny.txt")
    s.wait_text("read it")
    s.wait_turn_done()

    ctx.scenario("text=sure,final_text=sure")
    for prompt in ("thanks", "and again"):
        s.submit(prompt)
        s.wait_turn_done()
    assert tool_messages(ctx)[0]["content"] == "hello from disk\n", tool_messages(ctx)


def test_an_elided_result_keeps_answering_its_call(ctx):
    """The message stays a tool result: a call left unanswered breaks the turn."""
    s = ctx.spawn()
    big_read(ctx, s, "plenty")
    ctx.scenario("text=sure,final_text=sure")
    for prompt in ("thanks", "and again"):
        s.submit(prompt)
        s.wait_turn_done()

    messages = ctx.mock.requests[-1]["messages"]
    calls = [c["id"] for m in messages if m.get("tool_calls") for c in m["tool_calls"]]
    answered = [m["tool_call_id"] for m in messages if m["role"] == "tool"]
    assert calls and answered == calls, (calls, answered)
