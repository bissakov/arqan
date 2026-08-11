"""The object a benchmark case is handed, and the marks it can carry.

A case gets a `Bench`: an isolated `Ctx` from the test harness, a probe on the
process under measurement, and `step`/`keys` to measure work. Everything a
case prints goes through the run so that a machine-readable report and the
terminal never disagree.

Cases are ordinary functions named `bench_*` in `bench/cases/bench_*.py`, the
way test cases are discovered. They may carry marks:

    @slow                 excluded unless --slow or -k names it
    @needs("proc")        skipped where /proc is not readable
"""

from __future__ import annotations

import contextlib
import random
import time

from tests.context import Ctx
from tests.harness.session import Session

from .metrics import Probe, Stat, have_proc
from .report import Row, delta_row


class Skip(Exception):
    """Raised by a case that cannot run here; the runner reports it as skipped."""


def slow(fn):
    fn.slow = True
    return fn


def needs(*requirements):
    def deco(fn):
        fn.requires = tuple(requirements)
        return fn

    return deco


class Options:
    """Runner knobs a case can read; sizes must go through `Bench.scale`."""

    def __init__(self, scale: float = 1.0, budget_scale: float = 1.0,
                 budgets: bool = True, verbose: bool = False,
                 keep: bool = False, seed: int = 20240117):
        self.scale = scale
        self.budget_scale = budget_scale
        self.budgets = budgets
        self.verbose = verbose
        self.keep = keep
        self.seed = seed


class Bench:
    """One case's fixture, measurement and reporting surface."""

    def __init__(self, name: str, ctx: Ctx, run, result, opts: Options):
        self.name = name
        self.ctx = ctx
        self.run = run
        self.result = result
        self.opts = opts
        self.rng = random.Random(opts.seed)
        self._probe: Probe | None = None

    # ---- sizing -----------------------------------------------------------
    def scale(self, n: int, floor: int = 1) -> int:
        """Scale a workload by --scale, never below `floor`.

        Every size in a case goes through here so that `--scale 0.1` gives a
        smoke run with the same shape and `--scale 4` a soak run.
        """
        return max(floor, int(n * self.opts.scale))

    def skip(self, reason: str):
        raise Skip(reason)

    def require_proc(self):
        if not have_proc():
            self.skip("needs Linux /proc with smaps_rollup")

    # ---- process ----------------------------------------------------------
    def spawn(self, **kw) -> Session:
        """Spawn a session and measure it from here on."""
        s = self.ctx.spawn(**kw)
        self.track(s)
        return s

    def track(self, session: Session):
        if session.proc is None:
            raise AssertionError("session not started")
        self._probe = Probe(session.proc.pid)

    @property
    def probe(self) -> Probe:
        if self._probe is None:
            raise AssertionError("no process tracked; call spawn() or track()")
        return self._probe

    # ---- measuring --------------------------------------------------------
    def _budget(self, budget_ms: float | None) -> float | None:
        if budget_ms is None or not self.opts.budgets:
            return None
        return budget_ms * self.opts.budget_scale

    @contextlib.contextmanager
    def step(self, label: str, units: int = 1, unit: str = "op",
             budget_ms: float | None = None):
        """Measure the block, then print and record it as one row.

        `units` divides the cost, so a block that presses twenty keys reports
        per-key numbers and can carry a per-key budget.
        """
        start = self.probe.read()
        yield
        d = self.probe.since(start)
        self.run.row(
            self.result,
            delta_row(self.name, label, d, units=units, unit=unit,
                      budget_ms=self._budget(budget_ms)),
        )

    def measure(self, label: str, fn, units: int = 1, unit: str = "op",
                budget_ms: float | None = None):
        with self.step(label, units, unit, budget_ms):
            out = fn()
        return out

    def sample(self, label: str, fn, repeat: int = 5,
               budget_ms: float | None = None, unit: str = "op") -> Stat:
        """Run `fn` `repeat` times, reporting the distribution of one run.

        The worst run is what a budget judges, since an occasional stall is
        exactly the failure mode a mean hides.
        """
        cpus, walls = [], []
        first = self.probe.read()
        for _ in range(repeat):
            start = self.probe.read()
            fn()
            d = self.probe.since(start)
            cpus.append(d.cpu_ms)
            walls.append(d.wall_ms)
        total = self.probe.since(first)
        stat = Stat(cpus)
        self.run.row(
            self.result,
            delta_row(self.name, label, total, units=repeat, unit=unit,
                      budget_ms=self._budget(budget_ms), stat=stat,
                      note=f"cpu {stat}"),
        )
        return stat

    def keys(self, label: str, session: Session, presses,
             budget_ms: float | None = None, sync: bool = True,
             literal: bool = False) -> Stat:
        """Send keys one at a time, timing the frame each one asks for.

        `presses` is a sequence of key names, or of characters when `literal`.
        Wall time per key is dominated by the harness's quiet window, so the
        distribution reported is of CPU: a key that costs more than a
        millisecond or two is a repaint doing more work than the screen did.
        """
        presses = list(presses)
        cpus = []
        start = self.probe.read()
        for p in presses:
            before = self.probe.read()
            if literal:
                session.send(p)
            else:
                session.key(p)
            if sync:
                session.sync()
            cpus.append(self.probe.since(before).cpu_ms)
        if not sync:
            session.sync()
        total = self.probe.since(start)
        stat = Stat(cpus)
        self.run.row(
            self.result,
            delta_row(self.name, label, total, units=len(presses), unit="key",
                      budget_ms=self._budget(budget_ms), stat=stat,
                      note=f"cpu {stat}"),
        )
        return stat

    def row(self, label: str, wall_ms: float = 0.0, cpu_ms: float = 0.0,
            **kw) -> Row:
        """Record a figure this class did not time itself (a count, a size)."""
        row = Row(case=self.name, label=label, wall_ms=wall_ms, cpu_ms=cpu_ms,
                  **kw)
        self.run.row(self.result, row)
        return row

    def lifetime(self, label: str, wall_ms: float = 0.0,
                 budget_ms: float | None = None) -> Row:
        """Record everything the tracked process has cost since it was exec'd.

        Startup is the one window a probe cannot bracket, since the pid does
        not exist before it: what the counters hold at the first frame is the
        whole cost of getting there.
        """
        now = self.probe.read()
        return self.row(label, wall_ms=wall_ms, cpu_ms=now.cpu * 1000.0,
                        priv_kb=now.priv_kb, peak_kb=now.peak_kb,
                        budget_ms=self._budget(budget_ms))

    # ---- assertions -------------------------------------------------------
    def check(self, ok: bool, message: str):
        """Record a failure without abandoning the rest of the case.

        A benchmark that stops at the first surprise measures nothing else, so
        checks accumulate and the runner reports them together.
        """
        if not ok:
            self.result.failures.append(message)
            self.run.note(self.result, f"FAIL {message}")
        return ok

    def note(self, text: str):
        self.run.note(self.result, text)

    def alive(self, session: Session, what: str = "process"):
        """The stress-test assertion: it survived and still paints."""
        rc = session.proc.poll() if session.proc else -1
        return self.check(rc is None, f"{what} exited with {rc}")

    def timed(self, fn) -> float:
        t0 = time.perf_counter()
        fn()
        return (time.perf_counter() - t0) * 1000.0
