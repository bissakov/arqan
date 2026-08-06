#!/usr/bin/env python3
"""Test runner for the yoke TUI suite.

    python3 tests/run.py                 # run everything
    python3 tests/run.py -k composer     # run matching cases
    python3 tests/run.py --list          # show what exists
    python3 tests/run.py --update        # rewrite golden screens
    python3 tests/run.py -v --keep       # verbose, keep temp dirs on failure
    python3 tests/run.py -j 1            # one at a time (default: auto)

Cases are almost entirely idle — they wait on a pty and on a loopback socket —
so they run in a thread pool. Every case owns its temp dir, its mock provider
and its pty, so nothing is shared but the golden files, which are read-only
outside `--update` and written per case.

No third-party dependencies: discovery, isolation and reporting are all here
so `make test` works on a bare checkout with nothing but Python 3 and a built
`bin/yoke`.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import sys
import threading
import time
import traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))

from tests.context import BIN, Ctx  # noqa: E402

CASES_DIR = HERE / "cases"


class Colours:
    def __init__(self, enabled: bool):
        self.on = enabled

    def _wrap(self, code, s):
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


def auto_jobs(count: int) -> int:
    """Workers for a suite of `count` cases.

    A case burns about 1% of a core; the rest is waiting on the child's pty.
    Oversubscribing the CPUs is therefore free, but not unbounded: the waits
    are quiet-window based, so a machine buried under processes could see a
    stall look like a settled screen.
    """
    if count <= 1:
        return 1
    return max(2, min(count, (os.cpu_count() or 4) * 2, 32))


def load_cases():
    """Import every tests/cases/test_*.py and collect its test_* callables."""
    found = []
    for path in sorted(CASES_DIR.glob("test_*.py")):
        spec = importlib.util.spec_from_file_location(f"yokecases.{path.stem}", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        group = path.stem[len("test_") :]
        for attr in sorted(vars(module)):
            if not attr.startswith("test_"):
                continue
            fn = getattr(module, attr)
            if callable(fn):
                found.append((f"{group}.{attr[len('test_'):]}", fn))
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-k", "--filter", default="", help="substring match on case name")
    ap.add_argument("--list", action="store_true", help="list cases and exit")
    ap.add_argument("--update", action="store_true", help="rewrite golden files")
    ap.add_argument("--keep", action="store_true", help="keep temp dirs of failures")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-x", "--exitfirst", action="store_true")
    ap.add_argument("--repeat", type=int, default=1, help="run the suite N times")
    ap.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=0,
        help="cases to run in parallel (0 = auto, 1 = sequential)",
    )
    args = ap.parse_args(argv)

    c = Colours(sys.stdout.isatty() and not os.environ.get("NO_COLOR"))

    cases = load_cases()
    if args.filter:
        cases = [(n, f) for n, f in cases if args.filter in n]
    if args.list:
        for name, fn in cases:
            doc = (fn.__doc__ or "").strip().splitlines()
            print(f"{name:<44} {doc[0] if doc else ''}")
        return 0
    if not cases:
        print("no cases matched")
        return 1

    if not BIN.exists():
        print(c.red(f"missing {BIN} — run `make` first"))
        return 2

    jobs = args.jobs if args.jobs > 0 else auto_jobs(len(cases) * args.repeat)

    failures = []
    passed = 0
    stop = threading.Event()
    lock = threading.Lock()
    started = time.monotonic()

    def run_case(item):
        nonlocal passed
        name, fn = item
        if stop.is_set():
            return
        doc = (fn.__doc__ or "").strip().splitlines()
        summary = doc[0] if doc else ""
        if args.verbose:
            with lock:
                print(f"{c.dim('····')} {name:<44} {c.dim(summary)}", flush=True)
        ctx = Ctx(name, update=args.update, keep=args.keep)
        t0 = time.monotonic()
        failed = False
        tb = None
        error: Exception | None = None
        try:
            fn(ctx)
        except Exception as exc:  # noqa: BLE001 — a test failure is any throw
            failed, error, tb = True, exc, traceback.format_exc()
        finally:
            dt = time.monotonic() - t0
            ctx.cleanup(failed=failed)
        with lock:
            tag = c.red("FAIL") if failed else c.green("PASS")
            print(
                f"{tag} {name:<44} {c.dim(f'{dt * 1000:6.0f}ms')} {c.dim(summary)}",
                flush=True,
            )
            if failed:
                failures.append((name, error, tb))
            else:
                passed += 1
        if failed and args.exitfirst:
            stop.set()

    queue = [item for _ in range(args.repeat) for item in cases]
    if jobs <= 1:
        for item in queue:
            run_case(item)
            if stop.is_set():
                break
    else:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            list(pool.map(run_case, queue))

    elapsed = time.monotonic() - started
    print()
    for name, exc, tb in sorted(failures, key=lambda f: f[0]):
        print(c.bold(c.red(f"── {name} " + "─" * max(0, 60 - len(name)))))
        print(tb.rstrip())
        print()

    total = passed + len(failures)
    suffix = f" (-j{jobs})" if jobs > 1 else ""
    line = f"{passed}/{total} passed in {elapsed:.1f}s{suffix}"
    print(c.green(line) if not failures else c.red(line))
    if args.update:
        print(c.yellow("golden files rewritten (--update)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
