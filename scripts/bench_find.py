#!/usr/bin/env python3
"""Transcript search under a ~1M-token session.

Plants a session of about a million tokens with a known number of planted
markers, resumes it, and drives the search box from the real TUI while reading
CPU and Private_Dirty from /proc. Two things are measured together, because
either one alone would lie: the cost of a keystroke (every character retypes
the query, which counts the matches again) and the count itself, which the
planted markers make checkable at this size.

The interesting queries are the extremes. A marker that appears a few hundred
times is the ordinary case; a single common letter is the worst one, since the
scan stops at every match and every painted row has to be highlighted.

Run from the repo root: python3 scripts/bench_find.py
"""

from __future__ import annotations

import json
import os
import random
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tests.context import Ctx  # noqa: E402

HZ = os.sysconf("SC_CLK_TCK")
CHARS_PER_TOKEN = 4
MARKER = "zephyrmark"
MARKERS_PLANTED = 400
# Per keystroke, in the process rather than on the wall: the harness waits a
# quiet window after every key, which is most of the wall time and none of the
# search. A key that costs more CPU than this is a search that types badly.
KEY_BUDGET_MS = 20.0


def cpu_seconds(pid: int) -> float:
    """On-CPU time, in nanosecond resolution where the kernel offers it.

    A keystroke here costs single-digit milliseconds, which the 10ms tick of
    /proc/pid/stat cannot see at all.
    """
    try:
        return int(Path(f"/proc/{pid}/schedstat").read_text().split()[0]) / 1e9
    except (OSError, IndexError, ValueError):
        pass
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    except (OSError, IndexError):
        return 0.0
    return (int(fields[11]) + int(fields[12])) / HZ


def private_dirty_kb(pid: int) -> int:
    total = 0
    try:
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
            if line.startswith("Private_Dirty:"):
                total += int(line.split()[1])
    except OSError:
        return 0
    return total


WORDS = ("context window token stream arena transcript provider session "
         "buffer render wrap scroll resume persist scratch message").split()


def plant_session(ctx, name: str, tokens: int, turns: int = 40) -> tuple[Path, int]:
    """Write a ~`tokens` session, returning it and the marker count in it.

    The markers are spread evenly over the turns so that a search has to walk
    the whole transcript to count them, and so that the newest and oldest both
    have one.
    """
    d = ctx.home / ".local" / "share" / "yoke" / "sessions"
    d.mkdir(parents=True, exist_ok=True)
    subs = [p for p in d.iterdir() if p.is_dir()]
    target = subs[0] if subs else d / str(ctx.work).replace("/", "%2f")
    target.mkdir(parents=True, exist_ok=True)

    rng = random.Random(20240117)
    avg_word = sum(len(w) for w in WORDS) / len(WORDS) + 1
    words_per_msg = max(8, int(tokens * CHARS_PER_TOKEN / turns / avg_word))
    per_turn = MARKERS_PLANTED // turns

    path = target / f"{name}.jsonl"
    planted = 0
    with path.open("w") as f:
        for i in range(turns):
            f.write(json.dumps({
                "role": "user",
                "content": f"turn {i}: " + " ".join(rng.choice(WORDS)
                                                    for _ in range(12)),
            }) + "\n")
            left, lines = words_per_msg, []
            while left > 0:
                take = min(left, rng.randint(40, 90))
                lines.append(" ".join(rng.choice(WORDS) for _ in range(take)))
                left -= take
            # One marker per paragraph until this turn's share is used up.
            for k in range(min(per_turn, len(lines))):
                lines[k] += " " + MARKER
                planted += 1
            f.write(json.dumps({
                "role": "assistant", "content": "\n\n".join(lines),
            }) + "\n")
    return path, planted


def resume_at(s, index: int, timeout: float = 120.0):
    s.submit("/resume")
    s.wait_text("pick a session", timeout=timeout)
    for _ in range(index):
        s.key("down")
    s.settle()
    s.key("enter")
    s.wait_gone("pick a session", timeout=timeout)
    s.settle(timeout=timeout)


def find_row(s) -> str:
    for r in range(s.term.rows):
        if "find:" in s.row(r):
            return s.row(r).strip()
    return ""


def reported(s) -> tuple[int, int]:
    """(index, count) from the box, (0, 0) for no match."""
    m = re.search(r"(\d+) of (\d+)", find_row(s))
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def main() -> int:
    if not Path("/proc/self/smaps_rollup").exists():
        print("needs Linux /proc")
        return 1

    failures: list[str] = []
    ctx = Ctx(case="benchfind")
    try:
        ctx.scenario("text=ok")
        s = ctx.spawn()
        s.submit("seed")
        s.wait_turn_done()
        s.submit("/exit")
        s.wait_exit()

        print("planting session...")
        path, planted = plant_session(ctx, "20240101-000001", 1_000_000)
        chars = sum(len(json.loads(l)["content"])
                    for l in path.read_text().splitlines())
        print(f"  {path.stat().st_size/1e6:.1f} MB file, "
              f"~{chars // CHARS_PER_TOKEN // 1000}k tokens, "
              f"{planted} planted markers")

        s = ctx.spawn()
        pid = s.proc.pid
        print(f"\nbaseline private dirty: {private_dirty_kb(pid)} KB")
        print(f"\n{'action':<30} {'wall s':>8} {'cpu s':>8} {'privKB':>9}")

        def step(label, fn, budget_ms=None, per=1):
            c0, t0 = cpu_seconds(pid), time.perf_counter()
            out = fn()
            wall, cpu = time.perf_counter() - t0, cpu_seconds(pid) - c0
            each = cpu * 1000 / per
            over = budget_ms is not None and each > budget_ms
            print(f"{label:<30} {wall:>8.2f} {cpu:>8.2f} "
                  f"{private_dirty_kb(pid):>9}"
                  f"{f'   OVER {budget_ms:.0f}ms/key' if over else ''}")
            if over:
                failures.append(f"{label}: {each:.1f}ms cpu per key")
            return out

        step("resume 1M", lambda: resume_at(s, 1))
        # The last screenful is mid-reply prose, so the vocabulary is what
        # says a transcript is on screen at all.
        if not any(w in s.text() for w in WORDS):
            raise SystemExit("resume did not load")

        def type_query(text: str) -> tuple[list[float], float]:
            """Type `text` a key at a time, timing the frame each one asks for.

            Wall time per key includes the harness's quiet window, so the CPU
            the process burned over the whole query is the number that says
            what the search cost.
            """
            wall, cpu = [], []
            for ch in text:
                c0, t0 = cpu_seconds(pid), time.perf_counter()
                s.send(ch)
                s.sync()
                wall.append((time.perf_counter() - t0) * 1000)
                cpu.append((cpu_seconds(pid) - c0) * 1000)
            return wall, cpu

        def keys_report(label, typed, budget=KEY_BUDGET_MS):
            wall, cpu = typed
            worst = max(cpu)
            note = "" if worst <= budget else f"  OVER {budget:.0f}ms BUDGET"
            print(f"  {label}: {len(cpu)} keys, cpu "
                  f"{sum(cpu)/len(cpu):.1f}ms avg / {worst:.1f}ms worst, "
                  f"worst wall {max(wall):.0f}ms{note}")
            if worst > budget:
                failures.append(f"{label}: {worst:.1f}ms cpu on one keystroke")

        step("open box (ctrl-r)", lambda: s.key("ctrl-r").sync())

        # The ordinary case: a word that is in the transcript a few hundred
        # times, typed a letter at a time from a prefix that matches far more.
        keys = step(f"type {MARKER!r}", lambda: type_query(MARKER))
        keys_report("planted marker", keys)
        index, count = reported(s)
        print(f"  reported: {index} of {count}, planted {planted}")
        if count != planted:
            # Short is eviction, over is a bug; the cap drops the oldest turns
            # once the transcript passes 8 MB, so say which happened.
            kind = "under" if count < planted else "over"
            failures.append(f"marker count {kind}: {count} != {planted}")

        # Walking is a scan per step, and each step scrolls the viewport.
        def walk(n, key):
            for _ in range(n):
                s.key(key)
            s.sync()

        # Stepping is a scan and a scroll per key, on a transcript the reader
        # is walking backwards through.
        step("walk back x20 (enter)", lambda: walk(20, "enter"),
             KEY_BUDGET_MS, 20)
        step("walk forward x20 (down)", lambda: walk(20, "down"),
             KEY_BUDGET_MS, 20)
        index_after, count_after = reported(s)
        if count_after != count:
            failures.append(f"count moved while walking: {count_after} != {count}")

        # The worst case: one common letter, every screenful full of hits.
        step("clear query (ctrl-u)", lambda: s.key("ctrl-u").sync())
        keys = step("type 'e' (matches everywhere)", lambda: type_query("e"))
        keys_report("common letter", keys)
        _, dense = reported(s)
        print(f"  reported: {dense} matches")
        if dense < 10_000:
            failures.append(f"common letter found only {dense} matches")

        step("walk dense x20 (enter)", lambda: walk(20, "enter"),
             KEY_BUDGET_MS, 20)
        step("scroll with hits (pgup x20)", lambda: walk(20, "pageup"),
             KEY_BUDGET_MS, 20)
        step("scroll with hits (pgdn x20)", lambda: walk(20, "pagedown"),
             KEY_BUDGET_MS, 20)

        step("resize 100x30", lambda: s.resize(100, 30))
        step("resize 80x24", lambda: s.resize(80, 24))

        # A query nothing matches never stops early, so it is the one scan
        # that always reads every byte.
        step("clear query (ctrl-u)", lambda: s.key("ctrl-u").sync())
        keys = step("type 'qqzz' (no match)", lambda: type_query("qqzz"))
        keys_report("no match", keys)
        if reported(s) != (0, 0) or "no match" not in find_row(s):
            failures.append(f"expected no match, box says {find_row(s)!r}")

        step("close box (esc)", lambda: s.key("esc").sync())
        # Reopening keeps the query, which means counting it again.
        keys = step("reopen and retype marker",
                    lambda: (s.key("ctrl-r").sync(), s.key("ctrl-u").sync(),
                             type_query(MARKER))[-1])
        keys_report("reopen", keys)
        if reported(s)[1] != count:
            failures.append("count differs after reopen: "
                            f"{reported(s)[1]} != {count}")

        # Past the 8 MB scrollback cap the oldest turns are dropped, and the
        # search has to answer for what is left rather than for what was
        # planted. The reach of a search is the reach of the scrollback.
        print("\nplanting an over-cap session...")
        huge, huge_planted = plant_session(ctx, "20240101-000002", 3_000_000,
                                           turns=60)
        print(f"  {huge.stat().st_size/1e6:.1f} MB file, "
              f"{huge_planted} planted markers")
        s.key("esc").sync()          # the box owns the keyboard until it does not
        step("resume 3M (over cap)", lambda: resume_at(s, 1))
        # The box reopens with the last query still in it, so the phase that
        # measures typing starts from an empty one.
        step("open box (ctrl-r)",
             lambda: s.key("ctrl-r").sync().key("ctrl-u").sync())
        keys = step(f"type {MARKER!r}", lambda: type_query(MARKER))
        keys_report("over cap", keys)
        _, kept = reported(s)
        print(f"  reported: {kept} of {huge_planted} planted "
              f"({huge_planted - kept} evicted with the oldest rows)")
        if not 0 < kept <= huge_planted:
            failures.append(f"over-cap count {kept} of {huge_planted}")
        step("walk back x20 (enter)", lambda: walk(20, "enter"),
             KEY_BUDGET_MS, 20)
        if reported(s)[1] != kept:
            failures.append("over-cap count moved while walking")

        print(f"\nfinal private dirty: {private_dirty_kb(pid)} KB")
        if failures:
            print("\nFAILURES")
            for f in failures:
                print(f"  {f}")
            return 1
        print("\nall checks passed")
    finally:
        ctx.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
