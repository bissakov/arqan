"""Lines longer than the Markdown line buffer.

Past MD_LINE_MAX the renderer stops holding a line and writes it through as
it arrives. That path is fed a run at a time rather than a byte at a time, so
these cover the two things batching can break: where a run stops, and the
control bytes that end one.
"""

import time
from pathlib import Path

from tests.mockprovider import Scenario


def long_line(words: int = 900) -> str:
    """One line with no newline in it, far past MD_LINE_MAX."""
    return " ".join(f"w{i:04d}" for i in range(words))


def cpu_seconds(pid: int) -> float:
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    hz = 100  # SC_CLK_TCK; only ratios and coarse bounds are read from this
    return (int(fields[11]) + int(fields[12])) / hz


def test_long_line_is_independent_of_chunking(ctx):
    """The same long line renders the same however the stream splits it."""
    text = long_line()
    screens = []
    for chunk in (1, 7, 200):
        ctx.scenario(Scenario(text=text, chunk=chunk))
        s = ctx.spawn()
        s.submit("go")
        s.wait_turn_done()
        s.settle()
        screens.append(s.text())
    assert screens[0] == screens[1] == screens[2], "chunking changed the render"
    # And it is the line itself on screen, not a truncation of it.
    assert "w0899" in screens[0], screens[0][-400:]


def test_long_line_survives_carriage_returns(ctx):
    """A CR inside a long line is dropped without disturbing the run.

    The halves are short enough to share one screen, since the assertion is
    about the seam between them.
    """
    head = " ".join(f"w{i:04d}" for i in range(60))   # past MD_LINE_MAX
    tail = " ".join(f"x{i:04d}" for i in range(20))
    ctx.scenario(Scenario(text=head + "\r" + tail, chunk=5))
    s = ctx.spawn()
    s.submit("go")
    s.wait_turn_done()
    s.settle()

    out = s.text()
    assert "\r" not in out
    # Wrapping decides where rows break, so the seam is checked on the text
    # with the layout taken back out of it.
    flat = "".join(ch for ch in out if ch.isalnum())
    assert "w0059x0000" in flat, out[-400:]
    assert "x0019" in out, out[-400:]


def test_long_line_is_drained_per_run_not_per_byte(ctx):
    """A megabyte on one line costs a fraction of a second, not half of one.

    Handing the write-through path a byte at a time reached the transcript
    once per character, about 0.5 us each: this line cost 0.55 s of CPU
    before it was batched and 0.01 s after. The bound sits between the two,
    an order of magnitude clear of each, and CPU rather than wall time so a
    loaded test run cannot move it.
    """
    if not Path("/proc/self/stat").exists():
        return
    words = 150000
    ctx.scenario(Scenario(text=long_line(words), chunk=500))
    s = ctx.spawn()
    pid = s.proc.pid
    before = cpu_seconds(pid)
    s.submit("go")
    s.wait_turn_done(timeout=120)
    s.settle()
    spent = cpu_seconds(pid) - before

    assert f"w{words - 1:04d}" in s.text(), "the line did not finish rendering"
    assert spent < 0.15, f"1 MB on one line cost {spent:.2f} s CPU"
