#!/usr/bin/env python3
"""Benchmark and stress runner for arqan.

    python3 -m bench.run                     # every quick case
    python3 -m bench.run --slow              # including the soak cases
    python3 -m bench.run -k transcript       # matching cases only
    python3 -m bench.run --list              # show what exists
    python3 -m bench.run --scale 0.25        # smoke-sized workloads
    python3 -m bench.run --json out.json     # machine-readable report
    python3 -m bench.run --baseline out.json # compare against an earlier run

Cases run one at a time and never in a thread pool: a benchmark measures the
process it spawned, and a machine running forty other sessions measures those
instead. Everything else (isolation, the mock provider, the pty) is the test
harness, so a case is an ordinary session with a probe attached.

Exit status is non-zero when a case blew a budget, failed a stress assertion,
threw, or regressed past `--tolerance` or `--mem-tolerance` against a baseline.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from bench.case import Bench, Options, Skip  # noqa: E402
from bench.metrics import have_proc  # noqa: E402
from bench.report import Colours, Run  # noqa: E402
from tests.context import BIN, HIGHLIGHT_BIN, Ctx  # noqa: E402

CASES_DIR = HERE / "cases"


def load_cases():
    """Import every bench/cases/bench_*.py and collect its bench_* callables."""
    found = []
    for path in sorted(CASES_DIR.glob("bench_*.py")):
        spec = importlib.util.spec_from_file_location(f"arqanbench.{path.stem}", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        group = path.stem[len("bench_"):]
        for attr in sorted(vars(module)):
            if not attr.startswith("bench_"):
                continue
            fn = getattr(module, attr)
            if callable(fn):
                found.append((f"{group}.{attr[len('bench_'):]}", fn))
    return found


def summary_of(fn) -> str:
    doc = (fn.__doc__ or "").strip().splitlines()
    return doc[0] if doc else ""


def unmet(fn) -> str:
    """Why this case cannot run here, or "" when it can."""
    for req in getattr(fn, "requires", ()):
        if req == "proc" and not have_proc():
            return "needs Linux /proc with smaps_rollup"
    return ""


def write_back(*paths: Path):
    """Flush the binaries to disk before anything measures them.

    smaps reports a file page as private dirty while the page cache still
    holds it dirty, so a binary mapped minutes after it was linked charges
    its own text to the process: about 0.8 MB of phantom memory, and only
    for whichever side of a comparison was built last. Fsync makes a fresh
    build and an old one measure alike.
    """
    for path in paths:
        try:
            fd = os.open(path, os.O_RDONLY)
        except OSError:
            continue
        try:
            os.fsync(fd)
        except OSError:
            pass
        finally:
            os.close(fd)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-k", "--filter", default="",
                    help="substring match on case name, comma-separated for several")
    ap.add_argument("--list", action="store_true", help="list cases and exit")
    ap.add_argument("--slow", action="store_true", help="also run the soak cases")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="multiply every workload size (0.25 = smoke run)")
    ap.add_argument("--budget-scale", type=float, default=1.0,
                    help="multiply every budget, for a slower machine")
    ap.add_argument("--no-budgets", action="store_true",
                    help="measure and report, but never fail on a budget")
    ap.add_argument("--seed", type=int, default=20240117,
                    help="seed for generated workloads")
    ap.add_argument("--repeat", type=int, default=1, help="run the set N times")
    ap.add_argument("--json", default="", help="write the report to this file")
    ap.add_argument("--baseline", default="",
                    help="compare cpu per operation against an earlier --json")
    ap.add_argument("--tolerance", type=float, default=1.4,
                    help="cpu ratio above which a baseline comparison fails")
    ap.add_argument("--mem-tolerance", type=float, default=1.15,
                    help="memory ratio above which a baseline comparison fails")
    ap.add_argument("--regressed", default="",
                    help="write the cases a comparison faulted to this file")
    ap.add_argument("--keep", action="store_true", help="keep temp dirs of failures")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    c = Colours(sys.stdout.isatty() and not os.environ.get("NO_COLOR"))
    cases = load_cases()
    if args.filter:
        wanted = [p for p in args.filter.split(",") if p]
        cases = [(n, f) for n, f in cases if any(p in n for p in wanted)]
    elif not args.slow:
        cases = [(n, f) for n, f in cases if not getattr(f, "slow", False)]
    if args.list:
        for name, fn in cases:
            mark = " (slow)" if getattr(fn, "slow", False) else ""
            print(f"{name:<40} {summary_of(fn)}{mark}")
        return 0
    if not cases:
        print("no cases matched")
        return 1
    if not BIN.exists():
        print(c.red(f"missing {BIN}, run `make` first"))
        return 2
    if not have_proc():
        print(c.yellow("no /proc: process cost cannot be measured here"))

    write_back(BIN, HIGHLIGHT_BIN)

    opts = Options(
        scale=args.scale,
        budget_scale=args.budget_scale,
        budgets=not args.no_budgets,
        verbose=args.verbose,
        keep=args.keep,
        seed=args.seed,
    )
    run = Run(c, verbose=args.verbose)

    queue = [item for _ in range(args.repeat) for item in cases]
    for name, fn in queue:
        result = run.begin(name, summary_of(fn))
        reason = unmet(fn)
        if reason:
            result.skipped = reason
            run.end(result)
            continue
        ctx = Ctx(f"bench.{name}", keep=args.keep)
        bench = Bench(name, ctx, run, result, opts)
        started = time.monotonic()
        failed = False
        try:
            fn(bench)
        except Skip as exc:
            result.skipped = str(exc)
        except Exception:  # noqa: BLE001, any throw is this case's failure
            result.error = traceback.format_exc()
            failed = True
        finally:
            result.seconds = time.monotonic() - started
            ctx.cleanup(failed=failed or not result.ok)
        run.end(result)

    status = run.summary()
    if args.json:
        path = Path(args.json)
        run.to_json(path, meta={
            "binary": str(BIN),
            "scale": args.scale,
            "seed": args.seed,
            "budgets": not args.no_budgets,
            "cpus": os.cpu_count(),
            "when": time.strftime("%Y-%m-%dT%H:%M:%S"),
        })
        print(c.dim(f"report written to {path}"))
    if args.baseline:
        status |= run.compare(Path(args.baseline), args.tolerance,
                              args.mem_tolerance)
        if args.regressed:
            Path(args.regressed).write_text(",".join(run.regressed))
    return status


if __name__ == "__main__":
    sys.exit(main())
