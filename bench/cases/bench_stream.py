"""Streaming a reply: the cost per delta, per turn, and per glyph.

A turn arrives as hundreds of small SSE deltas, each one re-wrapping and
repainting the tail of the transcript. Cost per delta is therefore the number
that decides whether a long answer stays smooth, and it is the first thing a
quadratic re-wrap ruins.
"""

from __future__ import annotations

from bench.case import needs, slow
from bench.fixtures import markdown_doc, paragraphs, prose, wide_text
from tests.mockprovider import Scenario


def stream(b, s, scenario, prompt="write it", timeout=180.0):
    b.ctx.scenario(scenario)
    s.submit(prompt)
    s.wait_turn_done(timeout=timeout)


@needs("proc")
def bench_long_reply(b):
    """Two thousand words in four-word deltas: the ordinary long answer."""
    words, chunk = b.scale(2000, floor=200), 4
    s = b.spawn()
    with b.step("stream", units=words // chunk, unit="delta", budget_ms=1.5):
        stream(b, s, f"words={words},paragraphs=8,chunk={chunk}")
    b.check(s.status_kind() == "ready", f"turn ended as {s.status_kind()!r}")


@needs("proc")
def bench_single_word_deltas(b):
    """One word per delta, the shape a slow provider actually streams."""
    words = b.scale(600, floor=100)
    s = b.spawn()
    with b.step("stream", units=words, unit="delta", budget_ms=2.0):
        stream(b, s, f"words={words},paragraphs=6,chunk=1")
    b.alive(s)


@needs("proc")
def bench_one_unbroken_line(b):
    """A reply with no line breaks: every delta re-wraps the same paragraph."""
    words = b.scale(1200, floor=150)
    text = prose(b.rng, words)
    s = b.spawn()
    with b.step("stream", units=words // 2, unit="delta", budget_ms=2.5):
        stream(b, s, Scenario(text=text, chunk=2))
    b.alive(s)


@needs("proc")
def bench_markdown_reply(b):
    """Headings, lists, tables and fenced code: every renderer at once.

    Fenced C blocks reach the syntax highlighter, which is a second process,
    so this is also the measure of how much a code-heavy answer costs.
    """
    sections = b.scale(24, floor=4)
    doc = markdown_doc(b.rng, sections)
    deltas = max(1, len(doc.split()) // 6)
    s = b.spawn()
    with b.step("stream", units=deltas, unit="delta", budget_ms=3.0):
        stream(b, s, Scenario(text=doc, chunk=6))
    b.row("document", units=len(doc), unit="byte",
          note=f"{sections} sections, {deltas} deltas")
    b.alive(s)


@needs("proc")
def bench_wide_glyph_reply(b):
    """CJK, emoji and combining marks: the width tables on the hot path."""
    words = b.scale(500, floor=80)
    text = wide_text(b.rng, words)
    s = b.spawn()
    with b.step("stream", units=words // 4, unit="delta", budget_ms=3.0):
        stream(b, s, Scenario(text=text, chunk=4))
    b.alive(s)


@needs("proc")
def bench_many_turns(b):
    """Twenty turns in one session: per-turn cost and per-turn growth.

    The transcript only grows, so a per-turn cost that climbs is a replay
    doing the whole conversation again, and steady growth in private pages is
    an arena that is never reused.
    """
    turns = b.scale(20, floor=4)
    s = b.spawn()
    b.ctx.scenario("words=60,paragraphs=2,chunk=4")
    first = b.probe.read()

    def one_turn():
        s.submit("again")
        s.wait_turn_done(timeout=60.0)

    b.sample("turn", one_turn, repeat=turns, unit="turn", budget_ms=90.0)
    d = b.probe.since(first)
    b.row("growth", units=turns, unit="turn", growth_kb=d.priv_growth_kb,
          priv_kb=d.priv_kb, peak_kb=d.peak_kb,
          note=f"private dirty grew {d.priv_growth_kb}K over {turns} turns")
    b.check(d.priv_growth_kb < 40 * 1024,
            f"private dirty grew {d.priv_growth_kb}K over {turns} turns")


@needs("proc")
def bench_reasoning_then_answer(b):
    """A reasoning stream followed by the answer: two panels, one turn."""
    words = b.scale(400, floor=60)
    thoughts = "\n\n".join(paragraphs(b.rng, words))
    s = b.spawn()
    with b.step("stream", units=words // 4, unit="delta", budget_ms=3.0):
        stream(b, s, Scenario(reasoning=thoughts, words=80, chunk=4))
    b.alive(s)


@needs("proc")
def bench_interrupted_stream(b):
    """Esc mid-stream: the cost of stopping, and that it does stop."""
    s = b.spawn()
    b.ctx.scenario("words=4000,paragraphs=20,chunk=2,delay=0.002")
    s.submit("write forever")
    s.wait_for(lambda t: s.status_kind() != "ready", "the turn to start",
               timeout=30.0)
    with b.step("interrupt", budget_ms=40.0):
        s.key("esc")
        s.wait_status("ready", timeout=30.0)
    b.alive(s)


@slow
@needs("proc")
def bench_paced_stream_is_mostly_idle(b):
    """A slow provider must leave the process asleep between deltas."""
    s = b.spawn()
    deltas = b.scale(120, floor=20)
    b.ctx.scenario(f"words={deltas * 2},chunk=2,delay=0.01")
    start = b.probe.read()
    s.submit("slowly")
    s.wait_turn_done(timeout=180.0)
    d = b.probe.since(start)
    b.row("paced stream", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms, units=deltas,
          unit="delta", priv_kb=d.priv_kb, peak_kb=d.peak_kb,
          note=f"busy {d.busy * 100:.1f}% of the window")
    b.check(d.busy < 0.5, f"on CPU for {d.busy * 100:.0f}% of a paced stream")
