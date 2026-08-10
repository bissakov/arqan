#!/usr/bin/env python3
"""Measure yoke's CPU cost against wall time for a long streamed turn.

Drives a real pty session through the mock provider and reads the child's
utime/stime from /proc, so the number is the agent's own cycles rather than
the harness's. Run from the repo root: python3 scripts/bench_cpu.py
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tests.context import Ctx  # noqa: E402

HZ = os.sysconf("SC_CLK_TCK")


def cpu_seconds(pid: int) -> float:
    """utime+stime of the process and its reaped children, in seconds."""
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    except (OSError, IndexError):
        return 0.0
    # After the "comm) " split, field 0 is state; utime/stime are 11 and 12.
    return (int(fields[11]) + int(fields[12])) / HZ


def run(words: int, chunk: int) -> tuple[float, float, int]:
    ctx = Ctx(case="bench")
    try:
        ctx.scenario(f"words={words},chunk={chunk}")
        s = ctx.spawn()
        pid = s.proc.pid
        base_cpu = cpu_seconds(pid)
        t0 = time.perf_counter()
        s.submit("write me a long answer")
        s.wait_turn_done(timeout=120)
        wall = time.perf_counter() - t0
        cpu = cpu_seconds(pid) - base_cpu
        rss = 0
        try:
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith("VmHWM"):
                    rss = int(line.split()[1])
        except OSError:
            pass
    finally:
        ctx.cleanup()
    return wall, cpu, rss


def keystrokes(turns: int, keys: int = 200) -> tuple[float, float]:
    """CPU per keystroke once `turns` long replies sit in the transcript."""
    ctx = Ctx(case="bench")
    try:
        ctx.scenario("words=1500,chunk=16")
        s = ctx.spawn()
        pid = s.proc.pid
        for _ in range(turns):
            s.submit("go on")
            s.wait_turn_done(timeout=120)
        s.settle()
        base_cpu = cpu_seconds(pid)
        t0 = time.perf_counter()
        for i in range(keys):
            s.send("abcdefgh"[i % 8])
        s.settle()
        wall = time.perf_counter() - t0
        cpu = cpu_seconds(pid) - base_cpu
    finally:
        ctx.cleanup()
    return wall / keys * 1e3, cpu / keys * 1e3


def main() -> int:
    print(f"{'words':>8} {'chunk':>6} {'wall s':>8} {'cpu s':>8} {'cpu%':>6} {'peak RSS':>10}")
    for words, chunk in ((2000, 1), (8000, 1), (8000, 8)):
        wall, cpu, rss = run(words, chunk)
        pct = 100.0 * cpu / wall if wall else 0.0
        print(f"{words:>8} {chunk:>6} {wall:>8.2f} {cpu:>8.2f} {pct:>5.1f}% {rss:>8} KB")
    print()
    print(f"{'turns':>8} {'wall/key ms':>12} {'cpu/key ms':>12}")
    for turns in (1, 8, 24):
        wall_ms, cpu_ms = keystrokes(turns)
        print(f"{turns:>8} {wall_ms:>12.3f} {cpu_ms:>12.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
