"""Rows, budgets and the run report.

A benchmark is only useful if a regression fails something, so a row may
carry a budget: CPU milliseconds per operation, above which the run reports a
failure and the runner exits non-zero. Budgets are deliberately loose. They
catch a path that became quadratic, not a machine that is 20% slower than the
one the number was first read on.

Rows print as they are measured, because a case can take minutes and a
silent terminal is indistinguishable from a hang.
"""

from __future__ import annotations

import json
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .metrics import Delta, Stat


class Colours:
    def __init__(self, enabled: bool):
        self.on = enabled

    def _wrap(self, code: str, s: str) -> str:
        return f"\033[{code}m{s}\033[0m" if self.on else s

    def green(self, s):
        return self._wrap("32", s)

    def red(self, s):
        return self._wrap("31", s)

    def yellow(self, s):
        return self._wrap("33", s)

    def dim(self, s):
        return self._wrap("2", s)

    def bold(self, s):
        return self._wrap("1", s)


def human_kb(kb: int) -> str:
    if kb >= 10 * 1024:
        return f"{kb / 1024:.0f}M"
    if kb >= 1024:
        return f"{kb / 1024:.1f}M"
    return f"{kb}K"


@dataclass
class Row:
    """One measured step of one case."""

    case: str
    label: str
    wall_ms: float
    cpu_ms: float
    units: int = 1
    unit: str = "op"
    priv_kb: int = 0
    peak_kb: int = 0
    growth_kb: int = 0
    majflt: int = 0
    read_kb: int = 0
    write_kb: int = 0
    budget_ms: float | None = None
    stat: Stat | None = None
    note: str = ""

    @property
    def cpu_per_unit(self) -> float:
        return self.cpu_ms / self.units if self.units else self.cpu_ms

    @property
    def wall_per_unit(self) -> float:
        return self.wall_ms / self.units if self.units else self.wall_ms

    @property
    def worst_ms(self) -> float:
        """The figure a budget is judged on: the worst sample when there is one."""
        return self.stat.worst if self.stat else self.cpu_per_unit

    @property
    def over_budget(self) -> bool:
        return self.budget_ms is not None and self.worst_ms > self.budget_ms

    def as_dict(self) -> dict:
        out = {
            "case": self.case,
            "label": self.label,
            "wall_ms": round(self.wall_ms, 3),
            "cpu_ms": round(self.cpu_ms, 3),
            "units": self.units,
            "unit": self.unit,
            "cpu_per_unit_ms": round(self.cpu_per_unit, 4),
            "priv_kb": self.priv_kb,
            "peak_kb": self.peak_kb,
            "growth_kb": self.growth_kb,
            "majflt": self.majflt,
        }
        if self.budget_ms is not None:
            out["budget_ms"] = self.budget_ms
        if self.stat is not None:
            out["stat"] = self.stat.as_dict()
        if self.note:
            out["note"] = self.note
        return out


@dataclass(frozen=True)
class Metric:
    """One figure a baseline comparison judges.

    `floor` is the absolute value below which a step is left alone: the
    difference between 0.1 ms and 0.2 ms is the machine, not the code.
    """

    key: str
    attr: str
    floor: float
    timed: bool
    worse: str
    better: str

    def of_row(self, row: "Row") -> float:
        return float(getattr(row, self.attr))

    def of_dict(self, row: dict) -> float:
        return float(row.get(self.key, 0.0))

    def fmt(self, value: float, row: "Row") -> str:
        if self.timed:
            return f"{value:.2f} ms/{row.unit}"
        return human_kb(int(value))


METRICS = (
    Metric("cpu_per_unit_ms", "cpu_per_unit", 0.5, True, "SLOWER", "faster"),
    Metric("priv_kb", "priv_kb", 1024, False, "HEAVIER", "lighter"),
    Metric("growth_kb", "growth_kb", 512, False, "GROWS", "holds"),
)

# A stress case runs its one hostile operation once and asserts that the
# process survived it. Measured against itself that single sample swings by
# half again, so its cost is judged by a budget and never by a baseline;
# its memory, which does not swing, still is.
UNTIMED_GROUPS = ("stress.",)


@dataclass
class CaseResult:
    name: str
    summary: str = ""
    rows: list[Row] = field(default_factory=list)
    failures: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    skipped: str = ""
    seconds: float = 0.0
    error: str = ""

    @property
    def ok(self) -> bool:
        return not self.failures and not self.error


HEADER = (
    f"  {'step':<34}{'wall ms':>10}{'cpu ms':>10}"
    f"{'per op':>10}{'priv':>8}{'peak':>8}"
)


class Run:
    """Collects rows and prints them as they arrive."""

    def __init__(self, colours: Colours, verbose: bool = False):
        self.c = colours
        self.verbose = verbose
        self.results: list[CaseResult] = []
        self.started = time.monotonic()

    # ---- case lifecycle ---------------------------------------------------
    def begin(self, name: str, summary: str) -> CaseResult:
        result = CaseResult(name=name, summary=summary)
        self.results.append(result)
        print(self.c.bold(f"\n{name}") + self.c.dim(f"  {summary}"), flush=True)
        print(self.c.dim(HEADER), flush=True)
        return result

    def end(self, result: CaseResult):
        if result.skipped:
            print(self.c.yellow(f"  skipped: {result.skipped}"), flush=True)
            return
        for failure in result.failures:
            print(self.c.red(f"  FAIL  {failure}"), flush=True)
        if result.error:
            print(self.c.red(f"  ERROR {result.error}"), flush=True)
        print(self.c.dim(f"  ({result.seconds:.1f}s)"), flush=True)

    # ---- output -----------------------------------------------------------
    def row(self, result: CaseResult, row: Row):
        result.rows.append(row)
        per = f"{row.cpu_per_unit:>9.2f}" if row.units > 1 else " " * 9
        line = (
            f"  {row.label:<34}{row.wall_ms:>10.1f}{row.cpu_ms:>10.1f}"
            f"{per} {human_kb(row.priv_kb):>7}{human_kb(row.peak_kb):>8}"
        )
        if row.over_budget:
            line += self.c.red(
                f"   OVER {row.budget_ms:.1f}ms/{row.unit}"
            )
            result.failures.append(
                f"{row.label}: {row.worst_ms:.1f}ms per {row.unit} "
                f"exceeds {row.budget_ms:.1f}ms"
            )
        print(line, flush=True)
        if row.stat is not None and self.verbose:
            print(self.c.dim(f"      {row.stat}"), flush=True)
        if row.note:
            print(self.c.dim(f"      {row.note}"), flush=True)

    def note(self, result: CaseResult, text: str):
        result.notes.append(text)
        print(self.c.dim(f"      {text}"), flush=True)

    # ---- summary ----------------------------------------------------------
    def summary(self) -> int:
        elapsed = time.monotonic() - self.started
        ran = [r for r in self.results if not r.skipped]
        bad = [r for r in ran if not r.ok]
        print()
        for r in bad:
            print(self.c.bold(self.c.red(f"── {r.name} " + "─" * max(0, 56 - len(r.name)))))
            if r.error:
                print(r.error.rstrip())
            for f in r.failures:
                print(f"  {f}")
        skipped = [r for r in self.results if r.skipped]
        line = (
            f"{len(ran) - len(bad)}/{len(ran)} cases within budget in "
            f"{elapsed:.1f}s"
        )
        if skipped:
            line += f", {len(skipped)} skipped"
        print(self.c.green(line) if not bad else self.c.red(line))
        return 1 if bad else 0

    def to_json(self, path: Path, meta: dict):
        payload = {
            "meta": meta,
            "cases": [
                {
                    "name": r.name,
                    "summary": r.summary,
                    "seconds": round(r.seconds, 3),
                    "skipped": r.skipped,
                    "failures": r.failures,
                    "notes": r.notes,
                    "rows": [row.as_dict() for row in r.rows],
                }
                for r in self.results
            ],
        }
        path.write_text(json.dumps(payload, indent=2) + "\n")

    # ---- baselines --------------------------------------------------------
    def compare(self, baseline: Path, tolerance: float,
                mem_tolerance: float = 1.15) -> int:
        """Report rows that grew past a tolerance against an earlier report.

        CPU per operation is judged at `tolerance`, private dirty memory and
        per-step growth at the tighter `mem_tolerance`: pages are counted, not
        sampled, so they carry far less noise than a shared machine's clock.
        Rows are matched by case, label and position among the steps that
        share that label, so renaming or reordering a step drops it from the
        comparison rather than reporting a phantom regression.
        """
        try:
            old = json.loads(baseline.read_text())
        except (OSError, ValueError) as exc:
            print(self.c.red(f"cannot read baseline {baseline}: {exc}"))
            return 1
        previous: dict[tuple[str, str, int], dict] = {}
        seen: dict[tuple[str, str], int] = {}
        for case in old.get("cases", []):
            for row in case.get("rows", []):
                key = (case.get("name", ""), row.get("label", ""))
                nth = seen.get(key, 0)
                seen[key] = nth + 1
                previous[(*key, nth)] = row

        regressions, improvements, missing = [], [], 0
        seen.clear()
        for result in self.results:
            for row in result.rows:
                key = (result.name, row.label)
                nth = seen.get(key, 0)
                seen[key] = nth + 1
                before = previous.get((*key, nth))
                if before is None:
                    missing += 1
                    continue
                for metric in METRICS:
                    if metric.timed and result.name.startswith(UNTIMED_GROUPS):
                        continue
                    was = metric.of_dict(before)
                    now = metric.of_row(row)
                    # Below the floor a step is noise on any machine; only
                    # compare figures worth arguing about.
                    if was < metric.floor and now < metric.floor:
                        continue
                    ratio = now / was if was > 0 else 0.0
                    limit = tolerance if metric.timed else mem_tolerance
                    line = (
                        f"{result.name} / {row.label}: "
                        f"{metric.fmt(was, row)} -> {metric.fmt(now, row)}"
                        f" (x{ratio:.2f})"
                    )
                    if ratio > limit:
                        regressions.append((metric.worse, line))
                    elif ratio and ratio < 1.0 / limit:
                        improvements.append((metric.better, line))

        print()
        print(self.c.bold(
            f"baseline {baseline} (cpu x{tolerance:g}, memory x{mem_tolerance:g})"
        ))
        for word, line in improvements:
            print(self.c.green(f"  {word:<7} {line}"))
        for word, line in regressions:
            print(self.c.red(f"  {word:<7} {line}"))
        if missing:
            print(self.c.dim(f"  {missing} step(s) absent from the baseline"))
        if not regressions:
            print(self.c.green("  no regressions"))
        return 1 if regressions else 0


def stderr(msg: str):
    print(msg, file=sys.stderr, flush=True)


def delta_row(case: str, label: str, d: Delta, **kw) -> Row:
    """Build a row from a measured window."""
    return Row(
        case=case,
        label=label,
        wall_ms=d.wall_ms,
        cpu_ms=d.cpu_ms,
        priv_kb=d.priv_kb,
        peak_kb=d.peak_kb,
        growth_kb=d.priv_growth_kb,
        majflt=d.majflt,
        read_kb=d.read_kb,
        write_kb=d.write_kb,
        **kw,
    )


def dataclass_dict(obj) -> dict:
    return asdict(obj)
