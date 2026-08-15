#!/usr/bin/env python3
"""Test runner for the arqan TUI suite.

    python3 tests/run.py                 # failures and one final summary
    python3 tests/run.py -k composer     # run matching cases
    python3 tests/run.py --list          # show what exists
    python3 tests/run.py --update        # rewrite golden screens
    python3 tests/run.py -v --keep       # verbose, keep temp dirs on failure
    python3 tests/run.py -j 1            # one at a time (default: auto)

Cases are almost entirely idle, waiting on a pty and on a loopback socket, so
they run in a thread pool. Every case owns its temp dir, its mock provider
and its pty, so nothing is shared but the golden files, which are read-only
outside `--update` and written per case.

No third-party dependencies: discovery, isolation and reporting are all here
so `make test` works on a bare checkout with nothing but Python 3 and a built
`bin/arqan-test`.

ARQAN_TEST_BIN selects the binary under test and `make test` sets it to
`bin/arqan-test`, the -DAGENT_TESTING build. The trust store and the web
endpoints reach their fixtures through hooks compiled in only there.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import subprocess
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

    That is not free on a shared runner, where the cores are slower than a
    workstation's and are not the only thing on the host. Set
    ARQAN_TEST_JOBS there rather than lowering this for everyone.
    """
    if count <= 1:
        return 1
    return max(2, min(count, (os.cpu_count() or 4) * 3, 48))


def load_cases():
    """Import every tests/cases/test_*.py and collect its test_* callables."""
    found = []
    for path in sorted(CASES_DIR.glob("test_*.py")):
        spec = importlib.util.spec_from_file_location(f"arqancases.{path.stem}", path)
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


def testing_build(binary) -> bool:
    """Whether `binary` carries the -DAGENT_TESTING hooks the cases need.

    `--ca-trust` exists only in that build and needs no pty, mock or temp dir.
    A binary that cannot be executed fails the probe.
    """
    try:
        done = subprocess.run(
            [str(binary), "--ca-trust"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return done.returncode == 0


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
        print(c.red(f"missing {BIN}, run `make` first"))
        return 2
    if not testing_build(BIN):
        print(c.red(f"{BIN} is not a test build"))
        print(
            "The cases reach the mock provider, the trust store and the web\n"
            "endpoints through hooks compiled in only under -DAGENT_TESTING.\n"
            "Run `make test`, or build bin/arqan-test and set\n"
            "ARQAN_TEST_BIN=bin/arqan-test."
        )
        return 2

    env_jobs = int(os.environ.get("ARQAN_TEST_JOBS", "0") or 0)
    if args.jobs > 0:
        jobs = args.jobs
    elif env_jobs > 0:
        jobs = max(1, min(env_jobs, len(cases) * args.repeat))
    else:
        jobs = auto_jobs(len(cases) * args.repeat)

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
        except Exception as exc:  # noqa: BLE001, a test failure is any throw
            failed, error, tb = True, exc, traceback.format_exc()
        finally:
            dt = time.monotonic() - t0
            ctx.cleanup(failed=failed)
        with lock:
            if failed:
                print(
                    f"{c.red('FAIL')} {name:<44} "
                    f"{c.dim(f'{dt * 1000:6.0f}ms')} {c.dim(summary)}",
                    flush=True,
                )
                failures.append((name, error, tb))
            else:
                passed += 1
                if args.verbose:
                    print(
                        f"{c.green('PASS')} {name:<44} "
                        f"{c.dim(f'{dt * 1000:6.0f}ms')} {c.dim(summary)}",
                        flush=True,
                    )
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
