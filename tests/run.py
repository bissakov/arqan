#!/usr/bin/env python3
"""Test runner for the ah TUI suite.

    python3 tests/run.py                 # run everything
    python3 tests/run.py -k composer     # run matching cases
    python3 tests/run.py --list          # show what exists
    python3 tests/run.py --update        # rewrite golden screens
    python3 tests/run.py -v --keep       # verbose, keep temp dirs on failure

No third-party dependencies: discovery, isolation and reporting are all here
so `make test` works on a bare checkout with nothing but Python 3 and a built
`bin/ah`.
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


def load_cases():
    """Import every tests/cases/test_*.py and collect its test_* callables."""
    found = []
    for path in sorted(CASES_DIR.glob("test_*.py")):
        spec = importlib.util.spec_from_file_location(f"ahcases.{path.stem}", path)
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

    failures = []
    passed = 0
    started = time.monotonic()

    for _ in range(args.repeat):
        for name, fn in cases:
            doc = (fn.__doc__ or "").strip().splitlines()
            summary = doc[0] if doc else ""
            if args.verbose:
                print(f"{c.dim('····')} {name:<44} {c.dim(summary)}", flush=True)
            ctx = Ctx(name, update=args.update, keep=args.keep)
            t0 = time.monotonic()
            failed = False
            try:
                fn(ctx)
                passed += 1
                dt = time.monotonic() - t0
                print(
                    f"{c.green('PASS')} {name:<44} {c.dim(f'{dt * 1000:6.0f}ms')} "
                    f"{c.dim(summary)}",
                    flush=True,
                )
            except Exception as exc:  # noqa: BLE001 — a test failure is any throw
                failed = True
                dt = time.monotonic() - t0
                print(
                    f"{c.red('FAIL')} {name:<44} {c.dim(f'{dt * 1000:6.0f}ms')} "
                    f"{c.dim(summary)}",
                    flush=True,
                )
                failures.append((name, exc, traceback.format_exc()))
            finally:
                ctx.cleanup(failed=failed)
            if failed and args.exitfirst:
                break
        if failures and args.exitfirst:
            break

    elapsed = time.monotonic() - started
    print()
    for name, exc, tb in failures:
        print(c.bold(c.red(f"── {name} " + "─" * max(0, 60 - len(name)))))
        print(tb.rstrip())
        print()

    total = passed + len(failures)
    line = f"{passed}/{total} passed in {elapsed:.1f}s"
    print(c.green(line) if not failures else c.red(line))
    if args.update:
        print(c.yellow("golden files rewritten (--update)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
