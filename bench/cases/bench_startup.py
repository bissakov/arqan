"""Startup: what it costs to reach the first frame, and to sit there.

Startup is the only latency a user measures with a stopwatch, and idle cost
is the one a laptop measures with its fan. Both are read from the child's own
counters, so a busy machine changes the wall figures and not the verdict.
"""

from __future__ import annotations

import time

from bench.case import needs
from bench.fixtures import markdown_doc, source_tree
from bench.metrics import Stat


def _wait_first_frame(s):
    """Wait for visible composer chrome without waiting for output to settle."""
    s.wait_for(
        lambda t: t.contains("Message arqan") or t.contains("\u203a "),
        "first frame",
    )


@needs("proc")
def bench_first_frame(b):
    """Cold start to a painted composer, and what the idle process holds."""
    started = time.perf_counter()
    s = b.spawn(wait=False)
    _wait_first_frame(s)
    wall = (time.perf_counter() - started) * 1000.0
    b.lifetime("cold start to first frame", wall_ms=wall, budget_ms=20.0)
    b.check(wall < 120.0, f"first frame took {wall:.1f}ms wall")

    with b.step("settle", budget_ms=20.0):
        s.settle()
    b.note(f"first frame in {wall:.2f}ms wall")

    # The bulk TUI buffers are static storage that startup must not touch;
    # anything that moved them into the cleared block shows up here.
    now = b.probe.read()
    b.check(now.priv_kb < 12 * 1024,
            f"idle private dirty is {now.priv_kb}K, above 12M")
    b.check(now.fds < 32, f"{now.fds} open descriptors at idle")


@needs("proc")
def bench_idle_is_free(b):
    """A session nobody is typing at must not burn a CPU waiting."""
    s = b.spawn()
    seconds = b.scale(3, floor=1)
    with b.step("idle", units=seconds, unit="s", budget_ms=15.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            s.pump(0.05)
    b.alive(s)


@needs("proc")
def bench_project_context(b):
    """Startup with a deep project: AGENTS.md at every level, nested configs.

    Prompt discovery walks the directory chain, and every AGENTS.md on it is
    appended to the system prompt, so a monorepo pays this before its first
    frame.
    """
    depth = b.scale(8, floor=2)
    doc = markdown_doc(b.rng, b.scale(6, floor=2))
    here = b.ctx.work
    for i in range(depth):
        here = here / f"level{i}"
        here.mkdir(parents=True, exist_ok=True)
        (here / "AGENTS.md").write_text(f"# level {i}\n\n{doc}\n")
        (here / ".arqan").mkdir(exist_ok=True)
        (here / ".arqan" / "config.toml").write_text("verbose_tools = true\n")
    b.row("project depth", units=depth, unit="level",
          note=f"{len(doc)} bytes of AGENTS.md per level")

    started = time.perf_counter()
    s = b.spawn(cwd=str(here), wait=False)
    _wait_first_frame(s)
    wall = (time.perf_counter() - started) * 1000.0
    b.lifetime("start inside the tree", wall_ms=wall, budget_ms=40.0)
    b.check(wall < 200.0, f"first frame took {wall:.1f}ms wall")
    b.alive(s)


@needs("proc")
def bench_start_in_a_large_tree(b):
    """A working directory of thousands of files must not be walked at boot."""
    shape = source_tree(b.ctx.work / "repo", dirs=b.scale(40, floor=4),
                        files_per_dir=b.scale(12, floor=2), lines=40)
    b.row("tree", units=shape["files"], unit="file")

    started = time.perf_counter()
    s = b.spawn(cwd=str(b.ctx.work / "repo"), wait=False)
    _wait_first_frame(s)
    wall = (time.perf_counter() - started) * 1000.0
    b.lifetime("start in the tree", wall_ms=wall, budget_ms=40.0)
    b.check(wall < 200.0, f"first frame took {wall:.1f}ms wall")
    b.alive(s)


@needs("proc")
def bench_repeated_starts(b):
    """Start and quit repeatedly: the distribution, not one lucky run."""
    b.ctx.scenario("text=ok")
    runs = b.scale(20, floor=5)
    walls, cpus = [], []
    for _ in range(runs):
        t0 = time.perf_counter()
        s = b.ctx.spawn(wait=False)
        b.track(s)
        _wait_first_frame(s)
        walls.append((time.perf_counter() - t0) * 1000.0)
        cpus.append(b.probe.read().cpu * 1000.0)
        s.submit("/exit")
        s.wait_exit()
    wall_stat = Stat(walls)
    b.row("start", units=runs, unit="run", wall_ms=sum(walls),
          cpu_ms=sum(cpus),
          note=f"wall {wall_stat}")
    b.check(max(walls) < 1500.0, f"a start took {max(walls):.0f}ms")
