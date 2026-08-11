"""The built-in tools, against trees and files large enough to hurt.

Tool output is replayed to the provider on every later turn, so a tool that
is slow costs the turn it ran in, and a tool that returns too much costs
every turn after it. Both are measured here: time to run, and the size of
what it handed back.
"""

from __future__ import annotations

import json

from bench.case import needs, slow
from bench.fixtures import big_file, source_tree, unified_diff

NEEDLE = "zqneedle"


def call(b, s, name: str, args: dict, reply: str = "done", timeout: float = 120.0):
    """Run one tool call through a fresh turn, returning what it fed the model.

    The conversation is cleared first: the mock offers a tool only until the
    round budget of the conversation in front of it is spent, so a second
    measured call in the same transcript would quietly measure a plain turn.
    """
    s.submit("/clear")
    s.settle()
    b.ctx.scenario(f"tool={name}:{json.dumps(args)},final_text={reply}")
    s.submit(f"use {name}")
    s.wait_turn_done(timeout=timeout)
    results = b.ctx.mock.tool_results()
    return results[-1] if results else ""


def measured_call(b, s, label: str, name: str, args: dict,
                  budget_ms: float | None = None) -> str:
    out = ""
    with b.step(label, budget_ms=budget_ms):
        out = call(b, s, name, args)
    b.row(f"{label} result", units=max(1, len(out)), unit="byte",
          note=f"{len(out.splitlines())} lines replayed to the provider")
    return out


@needs("proc")
def bench_grep_a_tree(b):
    """grep walks a source tree and bounds what it reports."""
    shape = source_tree(b.ctx.work / "repo", dirs=b.scale(30, floor=4),
                        files_per_dir=b.scale(10, floor=2),
                        lines=b.scale(120, floor=40), needle=NEEDLE)
    b.row("tree", units=shape["files"], unit="file",
          note=f"{shape['hits']} planted matches")
    s = b.spawn()
    out = measured_call(b, s, "grep everything", "grep",
                        {"pattern": NEEDLE}, budget_ms=1500.0)
    b.check(len(out) < 200 * 1024, f"grep replayed {len(out)} bytes")
    b.check("more" in out.lower() or shape["hits"] < 100 or "\n" in out,
            "grep said nothing about what it omitted")
    measured_call(b, s, "grep by glob", "grep",
                  {"pattern": NEEDLE, "glob": "*.h"}, budget_ms=1500.0)
    measured_call(b, s, "grep a miss", "grep",
                  {"pattern": "zzz-no-such-token"}, budget_ms=1500.0)
    b.alive(s)


@needs("proc")
def bench_find_a_tree(b):
    """find matches names over the same walk, with no file reads at all."""
    shape = source_tree(b.ctx.work / "repo", dirs=b.scale(30, floor=4),
                        files_per_dir=b.scale(10, floor=2), lines=10)
    b.row("tree", units=shape["files"], unit="file")
    s = b.spawn()
    measured_call(b, s, "find by extension", "find", {"name": "*.c"},
                  budget_ms=800.0)
    measured_call(b, s, "find by path glob", "find", {"name": "repo/**/mod00*"},
                  budget_ms=800.0)
    b.alive(s)


@needs("proc")
def bench_read_a_large_file(b):
    """A read is bounded by lines and by bytes: neither may be exceeded."""
    lines = b.scale(20000, floor=2000)
    big_file(b.ctx.work / "big.txt", lines=lines, cols=100)
    b.row("file", units=lines, unit="line")
    s = b.spawn()
    out = measured_call(b, s, "read the head", "read", {"path": "big.txt"},
                        budget_ms=400.0)
    b.check(len(out) < 64 * 1024, f"a read replayed {len(out)} bytes")
    measured_call(b, s, "read a window", "read",
                  {"path": "big.txt", "offset": lines // 2, "limit": 200},
                  budget_ms=400.0)
    measured_call(b, s, "read the tail", "read",
                  {"path": "big.txt", "offset": max(1, lines - 100)},
                  budget_ms=400.0)
    b.alive(s)


@needs("proc")
def bench_read_a_wide_file(b):
    """One line of a hundred thousand columns: the pathological read."""
    path = b.ctx.work / "wide.txt"
    path.write_text("x" * b.scale(200000, floor=20000) + "\n")
    s = b.spawn()
    out = measured_call(b, s, "read one huge line", "read", {"path": "wide.txt"},
                        budget_ms=400.0)
    b.check(len(out) < 64 * 1024, f"the wide read replayed {len(out)} bytes")
    b.alive(s)


@needs("proc")
def bench_bash_output(b):
    """A command that prints megabytes must still hand back one bounded page."""
    s = b.spawn()
    lines = b.scale(200000, floor=20000)
    out = measured_call(b, s, "seq to stdout", "bash",
                        {"command": f"seq 1 {lines}"}, budget_ms=3000.0)
    b.check(len(out) < 32 * 1024, f"bash replayed {len(out)} bytes")
    measured_call(b, s, "output to stderr", "bash",
                  {"command": f"seq 1 {lines} 1>&2"}, budget_ms=3000.0)
    measured_call(b, s, "binary output", "bash",
                  {"command": "head -c 200000 /dev/urandom"}, budget_ms=3000.0)
    b.alive(s)


@needs("proc")
def bench_write_and_patch(b):
    """Writing a file whole, then patching it in many places at once."""
    lines = b.scale(4000, floor=500)
    big_file(b.ctx.work / "src.txt", lines=lines, cols=60)
    s = b.spawn()
    body = "\n".join(f"line {i}" for i in range(b.scale(4000, floor=500)))
    measured_call(b, s, "write a file", "write",
                  {"path": "out.txt", "content": body}, budget_ms=600.0)
    hunks = b.scale(60, floor=10)
    diff = unified_diff(b.ctx.work / "src.txt", hunks,
                        spacing=max(4, lines // (hunks + 1)))
    out = measured_call(b, s, "patch many hunks", "patch", {"patch": diff},
                        budget_ms=800.0)
    b.note(out.splitlines()[0] if out else "(no result)")
    b.check(not out.startswith("ERROR"), f"patch refused the diff: {out[:80]}")
    b.alive(s)


@needs("proc")
def bench_tool_rounds(b):
    """Several tool rounds in one turn: the loop, not the tool."""
    big_file(b.ctx.work / "notes.txt", lines=200)
    rounds = b.scale(6, floor=2)
    b.ctx.scenario(
        'tool=read:{"path":"notes.txt"},'
        f"tool_rounds={rounds},final_text=finished"
    )
    s = b.spawn()
    with b.step("turn", units=rounds, unit="round", budget_ms=120.0):
        s.submit("keep reading")
        s.wait_turn_done(timeout=180.0)
    b.check(len(b.ctx.mock.tool_results()) >= rounds,
            f"only {len(b.ctx.mock.tool_results())} rounds ran")
    b.alive(s)


@slow
@needs("proc")
def bench_tool_output_accumulates(b):
    """Many rounds in one turn: what keeping every earlier result costs.

    Each round replays the whole conversation, results included, so the
    request grows with the square of the rounds. Cost per round that grows
    faster than that is the transcript being rebuilt, not the request.
    """
    big_file(b.ctx.work / "notes.txt", lines=b.scale(2000, floor=200))
    rounds = b.scale(24, floor=6)
    b.ctx.scenario(
        'tool=read:{"path":"notes.txt"},'
        f"tool_rounds={rounds},final_text=finished"
    )
    s = b.spawn()
    start = b.probe.read()
    with b.step("turn", units=rounds, unit="round", budget_ms=150.0):
        s.submit("keep reading")
        s.wait_turn_done(timeout=300.0)
    d = b.probe.since(start)
    b.row("growth", units=rounds, unit="round", growth_kb=d.priv_growth_kb,
          priv_kb=d.priv_kb, peak_kb=d.peak_kb, read_kb=d.read_kb,
          write_kb=d.write_kb,
          note=f"read {d.read_kb}K, wrote {d.write_kb}K over {rounds} rounds")
    b.check(len(b.ctx.mock.tool_results()) >= rounds,
            f"only {len(b.ctx.mock.tool_results())} rounds ran")
    b.alive(s)


@slow
@needs("proc")
def bench_patch_cost_per_hunk(b):
    """The same file patched with more and more hunks, to expose the curve.

    A patch is applied by locating each hunk's context in the file, so cost
    per hunk should be flat. A per-hunk cost that climbs with the number of
    hunks is the file being re-scanned for every one of them.
    """
    lines = b.scale(4000, floor=500)
    per_hunk = []
    s = b.spawn()
    for hunks in (b.scale(10, floor=4), b.scale(40, floor=8),
                  b.scale(80, floor=16)):
        name = f"src{hunks}.txt"
        big_file(b.ctx.work / name, lines=lines, cols=60)
        diff = unified_diff(b.ctx.work / name, hunks,
                            spacing=max(4, lines // (hunks + 1)))
        start = b.probe.read()
        out = call(b, s, "patch", {"patch": diff})
        d = b.probe.since(start)
        per_hunk.append(d.cpu_ms / hunks)
        b.row(f"patch {hunks} hunks", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms,
              units=hunks, unit="hunk", priv_kb=d.priv_kb, peak_kb=d.peak_kb)
        b.check(not out.startswith("ERROR"), f"patch refused {hunks} hunks")
    ratio = per_hunk[-1] / per_hunk[0] if per_hunk[0] > 0 else 0.0
    b.note(f"cost per hunk grew x{ratio:.2f} from the smallest patch")
    b.check(ratio < 4.0, f"cost per hunk grew x{ratio:.1f} with hunk count")
