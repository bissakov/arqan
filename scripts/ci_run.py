#!/usr/bin/env python3
"""Shard the existing runner in the working directory, including a reference tree."""

from __future__ import annotations

import argparse
import hashlib
import importlib
import sys
from pathlib import Path


def parse_shard(value: str) -> tuple[int, int]:
    try:
        index, count = (int(part) for part in value.split("/"))
    except ValueError:
        raise argparse.ArgumentTypeError("shard must be INDEX/COUNT") from None
    if not 1 <= index <= count:
        raise argparse.ArgumentTypeError("shard must satisfy 1 <= INDEX <= COUNT")
    return index - 1, count


def select_cases(cases, shard: tuple[int, int]):
    index, count = shard
    return [
        (name, fn) for name, fn in cases
        if int.from_bytes(hashlib.sha256(name.encode()).digest()[:8], "big") % count == index
    ]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("suite", choices=("bench", "test"))
    ap.add_argument("shard", type=parse_shard)
    args, options = ap.parse_known_args(argv)

    sys.path.insert(0, str(Path.cwd()))
    if args.suite == "test":
        sys.path.insert(1, str(Path.cwd() / "tests"))
    runner = importlib.import_module("bench.run" if args.suite == "bench" else "tests.run")
    discover = runner.load_cases
    runner.load_cases = lambda: select_cases(discover(), args.shard)
    try:
        return runner.main(options)
    finally:
        runner.load_cases = discover


if __name__ == "__main__":
    sys.exit(main())
