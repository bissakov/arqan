"""The composer: cost per keystroke, and what a large draft does to it.

A keystroke is the one interaction whose latency a user feels directly, and
the composer repaints the panel on every one of them. Cost per key must not
depend on how much has already been typed, so each case here types into a
draft that is already large.
"""

from __future__ import annotations

from bench.case import needs, slow
from bench.fixtures import prose, wide_text


def draft(b, s, words: int):
    """Paste a draft in, so the measured typing starts from a large one."""
    s.paste(prose(b.rng, words))
    s.sync()
    return s


@needs("proc")
def bench_typing(b):
    """Typing into an empty composer: the floor every other case is judged by."""
    s = b.spawn()
    text = prose(b.rng, b.scale(12, floor=4))[:b.scale(120, floor=40)]
    b.keys("type", s, list(text), literal=True, budget_ms=2.0)
    b.alive(s)


@needs("proc")
def bench_typing_into_a_large_draft(b):
    """The same keys with a thousand words already in the composer."""
    s = b.spawn()
    draft(b, s, b.scale(1000, floor=100))
    text = prose(b.rng, b.scale(12, floor=4))[:b.scale(120, floor=40)]
    b.keys("type", s, list(text), literal=True, budget_ms=3.0)
    b.keys("backspace", s, ["backspace"] * b.scale(40, floor=10), budget_ms=3.0)
    b.alive(s)


@needs("proc")
def bench_motion_over_a_large_draft(b):
    """Word motion, line motion and kills over a draft of many lines."""
    s = b.spawn()
    for _ in range(b.scale(20, floor=4)):
        s.type(prose(b.rng, 12))
        s.key("newline")
    s.sync()
    b.keys("word left", s, ["ctrl-left"] * b.scale(30, floor=8), budget_ms=3.0)
    b.keys("line ends", s, ["ctrl-a", "ctrl-e"] * b.scale(15, floor=4),
           budget_ms=3.0)
    b.keys("up and down", s, ["up", "down"] * b.scale(15, floor=4),
           budget_ms=3.0)
    b.alive(s)


@needs("proc")
def bench_paste(b):
    """One bracketed paste of a large block arrives as a single write."""
    s = b.spawn()
    for words in (b.scale(200, floor=50), b.scale(2000, floor=200),
                  b.scale(20000, floor=1000)):
        text = prose(b.rng, words)
        with b.step(f"paste {len(text) // 1024}K", budget_ms=250.0):
            s.paste(text)
            s.sync(timeout=60.0)
        s.key("ctrl-u")
        s.sync()
    b.alive(s)


@needs("proc")
def bench_paste_of_wide_glyphs(b):
    """A paste of CJK and emoji: the width tables, on a draft this time."""
    s = b.spawn()
    text = wide_text(b.rng, b.scale(2000, floor=200))
    with b.step("paste", units=len(text), unit="char", budget_ms=0.5):
        s.paste(text)
        s.sync(timeout=60.0)
    b.alive(s)


@needs("proc")
def bench_completion_popup(b):
    """Slash completion filters the command list on every keystroke."""
    s = b.spawn()
    b.keys("open and filter", s, list("/settings"), literal=True, budget_ms=3.0)
    b.keys("delete back", s, ["backspace"] * 9, budget_ms=3.0)
    b.alive(s)


@needs("proc")
def bench_file_picker_in_a_large_tree(b):
    """The '@' picker over a directory with thousands of entries.

    Every keystroke after '@' re-filters the listing, so a flat directory of
    many files is the shape that hurts.
    """
    flat = b.ctx.work / "many"
    flat.mkdir(parents=True, exist_ok=True)
    count = b.scale(2000, floor=200)
    for i in range(count):
        (flat / f"file{i:05d}.txt").write_text("x\n")
    b.row("directory", units=count, unit="entry")

    s = b.spawn()
    b.keys("open picker", s, ["@"], literal=True, budget_ms=60.0)
    b.keys("filter", s, list("many/file0123"), literal=True, budget_ms=15.0)
    b.keys("walk", s, ["down"] * b.scale(10, floor=3), budget_ms=10.0)
    s.key("esc").sync()
    b.alive(s)


@slow
@needs("proc")
def bench_key_storm(b):
    """Thousands of keys with no pause: the input path with no repaint budget.

    Nothing is synchronised here, so the child sees a full pipe and must
    coalesce; what is measured is whether it drains per read rather than per
    byte, and that it is still alive and painting afterwards.
    """
    s = b.spawn()
    keys = b.scale(4000, floor=500)
    text = prose(b.rng, keys // 4)[:keys]
    start = b.probe.read()
    s.send(text)
    s.sync(timeout=60.0)
    d = b.probe.since(start)
    b.row("unsynchronised keys", wall_ms=d.wall_ms, cpu_ms=d.cpu_ms,
          units=len(text), unit="key", priv_kb=d.priv_kb, peak_kb=d.peak_kb,
          budget_ms=0.5)
    s.key("ctrl-u").sync()
    b.check(s.composer_text() == "", "the composer did not clear after the storm")
    b.alive(s)
