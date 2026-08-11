"""A long transcript: scrolling it, searching it, resizing it, selecting it.

Every one of these repaints from the conversation rather than from the screen,
so their cost is a function of how much has been said, not of what is
visible. A step whose cost per key grows with the transcript is the failure
this file exists to catch.
"""

from __future__ import annotations

from bench.case import needs, slow
from bench.fixtures import find_counts, markdown_doc, prose
from tests.mockprovider import Scenario

NEEDLE = "zqneedle"


def fill(b, s, turns: int = 6, words: int = 300, needle: str = "",
         every: int = 40):
    """Build a transcript of known size, optionally seeded with a needle."""
    planted = 0
    for i in range(turns):
        chunks = []
        for j in range(words // 20):
            line = prose(b.rng, 20)
            if needle and j % every == 0:
                line += " " + needle
                planted += 1
            chunks.append(line)
        b.ctx.scenario(Scenario(text="\n\n".join(chunks), chunk=8))
        s.submit(f"turn {i}")
        s.wait_turn_done(timeout=120.0)
    return planted


@needs("proc")
def bench_scroll_a_long_transcript(b):
    """PageUp through the whole transcript and back down again."""
    s = b.spawn()
    turns = b.scale(8, floor=2)
    fill(b, s, turns=turns, words=b.scale(300, floor=100))
    pages = b.scale(30, floor=6)
    b.keys("pageup", s, ["pageup"] * pages, budget_ms=6.0)
    b.keys("pagedown", s, ["pagedown"] * pages, budget_ms=6.0)
    b.alive(s)


@needs("proc")
def bench_wheel_scroll(b):
    """The wheel moves one row at a time: the finest-grained repaint there is."""
    s = b.spawn()
    fill(b, s, turns=b.scale(6, floor=2), words=b.scale(300, floor=100))
    ticks = b.scale(40, floor=10)
    start = b.probe.read()
    for _ in range(ticks):
        s.mouse("wheel-up", 5, 10)
    s.sync()
    d = b.probe.since(start)
    b.row("wheel up", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=ticks,
          unit="tick", priv_kb=d.priv_kb, peak_kb=d.peak_kb, budget_ms=3.0)
    b.alive(s)


@needs("proc")
def bench_search_the_transcript(b):
    """Opening find, typing a query, then walking every match.

    The query is matched over the whole transcript on every keystroke, so
    typing eight characters searches it eight times.
    """
    s = b.spawn()
    planted = fill(b, s, turns=b.scale(6, floor=2), words=b.scale(300, floor=100),
                   needle=NEEDLE, every=4)
    b.row("matches", units=planted, unit="match")

    s.key("ctrl-r").sync()
    b.keys("query", s, list(NEEDLE), literal=True, budget_ms=8.0)
    index, count = find_counts(s)
    b.check(count == planted, f"find counted {count} of {planted} matches")

    steps = min(planted, b.scale(20, floor=5))
    b.keys("next match", s, ["enter"] * steps, budget_ms=8.0)
    s.key("esc").sync()
    b.alive(s)


@needs("proc")
def bench_resize_storm(b):
    """Every width change re-wraps the whole transcript from the source."""
    s = b.spawn()
    fill(b, s, turns=b.scale(6, floor=2), words=b.scale(300, floor=100))
    widths = [60, 100, 72, 132, 48, 80]
    rounds = b.scale(3, floor=1)
    start = b.probe.read()
    changes = 0
    for _ in range(rounds):
        for w in widths:
            s.resize(w, 24)
            s.sync()
            changes += 1
    d = b.probe.since(start)
    b.row("resize", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=changes,
          unit="resize", priv_kb=d.priv_kb, peak_kb=d.peak_kb, budget_ms=25.0)
    b.check(s.cols == 80, "the harness left the terminal at another width")
    b.alive(s)


@needs("proc")
def bench_select_and_copy(b):
    """Dragging a selection repaints per motion report and then copies."""
    s = b.spawn()
    fill(b, s, turns=b.scale(3, floor=1), words=b.scale(200, floor=80))
    drags = b.scale(20, floor=5)
    start = b.probe.read()
    for i in range(drags):
        row = 4 + (i % 8)
        s.mouse("down", row, 3)
        for col in range(6, 70, 8):
            s.mouse("drag", row, col)
        s.mouse("up", row, 70)
        s.sync()
    d = b.probe.since(start)
    b.row("drag select", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=drags,
          unit="drag", priv_kb=d.priv_kb, peak_kb=d.peak_kb, budget_ms=20.0)
    b.check(bool(s.screen.clipboard), "a drag copied nothing")
    b.alive(s)


@needs("proc")
def bench_markdown_transcript_scroll(b):
    """Scrolling a transcript of rendered markdown, code blocks included."""
    s = b.spawn()
    for i in range(b.scale(4, floor=1)):
        b.ctx.scenario(Scenario(text=markdown_doc(b.rng, b.scale(8, floor=2)),
                                chunk=12))
        s.submit(f"document {i}")
        s.wait_turn_done(timeout=120.0)
    pages = b.scale(24, floor=6)
    b.keys("pageup", s, ["pageup"] * pages, budget_ms=8.0)
    b.alive(s)


@slow
@needs("proc")
def bench_scroll_does_not_grow_with_use(b):
    """Scrolling forever must not accumulate: the same page, over and over."""
    s = b.spawn()
    fill(b, s, turns=b.scale(6, floor=2), words=b.scale(300, floor=100))
    s.key("pageup").sync()
    start = b.probe.read()
    rounds = b.scale(60, floor=10)
    for _ in range(rounds):
        s.key("pageup")
        s.key("pagedown")
    s.sync()
    d = b.probe.since(start)
    b.row("scroll churn", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=rounds * 2,
          unit="key", growth_kb=d.priv_growth_kb, priv_kb=d.priv_kb,
          peak_kb=d.peak_kb, budget_ms=6.0)
    b.check(d.priv_growth_kb < 2048,
            f"scrolling alone grew private dirty by {d.priv_growth_kb}K")
    b.alive(s)
