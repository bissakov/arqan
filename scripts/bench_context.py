#!/usr/bin/env python3
"""Large-context behaviour: a ~1M-token session, scrolling, and resumes.

Plants synthetic session files under the per-cwd session directory, then
drives the real TUI through /resume, scrolling and repeated session switches
while reading CPU and Private_Dirty from /proc. The point is the shape of the
curves: resuming a huge session then a small one must not cost more than
resuming the huge one once, since the persistent arena is rewound per resume.

Run from the repo root: python3 scripts/bench_context.py
"""

from __future__ import annotations

import json
import os
import random
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tests.context import Ctx  # noqa: E402

HZ = os.sysconf("SC_CLK_TCK")
# The mock text is ordinary prose, so a token is about four characters.
CHARS_PER_TOKEN = 4


def cpu_seconds(pid: int) -> float:
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


def paragraph(rng: random.Random, words: int, vocab) -> str:
    return " ".join(rng.choice(vocab) for _ in range(words))


def plant_session(ctx, name: str, tokens: int, tag: str, turns: int = 40) -> Path:
    """Write a session file of roughly `tokens` tokens, as yoke would.

    `tag` is salted through the text so a resumed screen says which session
    is on it: the benchmark is worthless if a resume quietly does nothing.
    """
    d = ctx.home / ".local" / "share" / "yoke" / "sessions"
    d.mkdir(parents=True, exist_ok=True)
    # yoke slugs the cwd; reuse whatever directory it already made, or make
    # the slug the same way so /resume finds the file.
    subs = [p for p in d.iterdir() if p.is_dir()]
    if subs:
        target = subs[0]
    else:
        slug = str(ctx.work).replace("/", "%2f")
        target = d / slug
        target.mkdir(parents=True, exist_ok=True)

    rng = random.Random(1234)
    vocab = WORDS + [tag] * 4    # frequent enough to land on any screenful
    avg_word = sum(len(w) for w in vocab) / len(vocab) + 1
    words_per_msg = max(8, int(tokens * CHARS_PER_TOKEN / turns / avg_word))

    path = target / f"{name}.jsonl"
    with path.open("w") as f:
        for i in range(turns):
            user = f"turn {i} {tag}: " + paragraph(rng, 12, vocab)
            f.write(json.dumps({"role": "user", "content": user}) + "\n")
            # Real replies are broken into paragraphs; one unbroken line is a
            # different path and is measured separately.
            left, lines = words_per_msg, []
            while left > 0:
                take = min(left, rng.randint(40, 90))
                lines.append(paragraph(rng, take, vocab))
                left -= take
            reply = "\n\n".join(lines)
            f.write(json.dumps({"role": "assistant", "content": reply}) + "\n")
    return path


def resume_at(s, index: int, timeout: float = 120.0):
    """Open /resume and pick the entry `index` rows down, newest first.

    Selection is by position rather than by typing: the picker lists sessions
    under formatted timestamps, and stepping to a known row keeps the
    benchmark independent of how the filter treats them.
    """
    s.submit("/resume")
    s.wait_text("pick a session", timeout=timeout)
    for _ in range(index):
        s.key("down")
    s.settle()
    s.key("enter")
    s.wait_gone("pick a session", timeout=timeout)
    s.settle(timeout=timeout)


def main() -> int:
    if not Path("/proc/self/smaps_rollup").exists():
        print("needs Linux /proc")
        return 1

    ctx = Ctx(case="benchctx")
    try:
        ctx.scenario("text=ok")
        # A first real turn creates the session directory with yoke's own slug.
        s = ctx.spawn()
        s.submit("seed")
        s.wait_turn_done()
        s.submit("/exit")
        s.wait_exit()

        print("planting sessions...")
        big = plant_session(ctx, "20240101-000001", 1_000_000, "alpha")
        small = plant_session(ctx, "20240101-000002", 200_000, "bravo")
        for label, p in (("1M-token", big), ("200k-token", small)):
            chars = sum(len(json.loads(l)["content"])
                        for l in p.read_text().splitlines())
            print(f"  {label} session: {p.stat().st_size/1e6:.1f} MB file, "
                  f"~{chars // CHARS_PER_TOKEN // 1000}k tokens")

        s = ctx.spawn()
        pid = s.proc.pid
        base_rss = private_dirty_kb(pid)
        print(f"\nbaseline private dirty: {base_rss} KB")

        print(f"\n{'action':<28} {'wall s':>8} {'cpu s':>8} {'privKB':>9}")

        def step(label, fn):
            c0, t0 = cpu_seconds(pid), time.perf_counter()
            fn()
            wall, cpu = time.perf_counter() - t0, cpu_seconds(pid) - c0
            print(f"{label:<28} {wall:>8.2f} {cpu:>8.2f} {private_dirty_kb(pid):>9}")

        # Newest first: row 0 is the seed turn, row 1 the 200k session, row 2
        # the 1M one.
        BIG, SMALL = 2, 1

        def verify(marker):
            if marker not in s.text():
                raise SystemExit(f"resume did not load: {marker!r} absent")

        step("resume 1M", lambda: resume_at(s, BIG))
        verify("alpha")
        step("scroll to top (PageUp x40)",
             lambda: [s.key("pageup") for _ in range(40)] and s.settle())
        step("scroll to bottom (PageDown x40)",
             lambda: [s.key("pagedown") for _ in range(40)] and s.settle())
        step("resize 100x30", lambda: s.resize(100, 30))
        step("resize 80x24", lambda: s.resize(80, 24))

        # The question that matters: does switching sessions accumulate?
        for i in range(4):
            step(f"resume 200k (#{i+1})", lambda: resume_at(s, SMALL))
            verify("bravo")
            step(f"resume 1M   (#{i+1})", lambda: resume_at(s, BIG))
            verify("alpha")

        step("type 60 keys", lambda: [s.send("x") for _ in range(60)] and s.settle())
    finally:
        ctx.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
