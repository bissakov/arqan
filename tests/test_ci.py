"""Checks for CI sharding without launching the TUI."""

from __future__ import annotations

import argparse
import subprocess
import sys
import unittest
from collections import Counter
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from scripts.ci_run import main, parse_shard, select_cases


class ShardTests(unittest.TestCase):
    def setUp(self):
        self.cases = [(f"group.case_{i}", object()) for i in range(100)]

    def test_parse_shard(self):
        self.assertEqual(parse_shard("1/4"), (0, 4))
        self.assertEqual(parse_shard("4/4"), (3, 4))

    def test_invalid_shards(self):
        for value in ("", "1", "one/two", "1/2/3", "0/4", "5/4", "1/0", "-1/4"):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    parse_shard(value)

    def test_every_case_runs_once(self):
        for count in (1, 2, 4):
            with self.subTest(count=count):
                shards = [select_cases(self.cases, (i, count)) for i in range(count)]
                names = [name for shard in shards for name, _ in shard]
                self.assertEqual(Counter(names), Counter(name for name, _ in self.cases))
                self.assertTrue(all(shards))

    def test_preserves_order_and_functions(self):
        selected = select_cases(self.cases, (0, 4))
        self.assertEqual(selected, [case for case in self.cases if case in selected])
        for name, fn in selected:
            self.assertIs(fn, dict(self.cases)[name])

    def test_assignment_survives_added_removed_and_reordered_cases(self):
        changed = list(reversed(self.cases[10:] + [("new.case", object())]))
        for index in range(4):
            original = {name for name, _ in select_cases(self.cases, (index, 4))}
            current = {name for name, _ in select_cases(changed, (index, 4))}
            common = {name for name, _ in self.cases[10:]}
            self.assertEqual(original & common, current & common)

    def test_preserves_slow_marker(self):
        slow = SimpleNamespace(slow=True)
        self.assertEqual(select_cases([("group.slow", slow)], (0, 1)), [("group.slow", slow)])

    def test_runner_receives_options_and_returns_status(self):
        discovered = lambda: self.cases
        observed = []

        def run(options):
            observed.append((options, runner.load_cases()))
            return 7

        runner = SimpleNamespace(load_cases=discovered, main=run)
        with patch("scripts.ci_run.importlib.import_module", return_value=runner) as load:
            status = main(["bench", "2/4", "--no-budgets", "--json", "report.json"])
        load.assert_called_once_with("bench.run")
        self.assertEqual(status, 7)
        self.assertEqual(observed, [
            (["--no-budgets", "--json", "report.json"], select_cases(self.cases, (1, 4)))
        ])
        self.assertIs(runner.load_cases, discovered)

    def test_test_runner(self):
        runner = SimpleNamespace(load_cases=lambda: self.cases, main=lambda options: 0)
        with patch("scripts.ci_run.importlib.import_module", return_value=runner) as load:
            self.assertEqual(main(["test", "1/2", "--list"]), 0)
        load.assert_called_once_with("tests.run")

    def test_restores_discovery_after_exception(self):
        discovered = lambda: self.cases
        runner = SimpleNamespace(load_cases=discovered, main=lambda options: 1 / 0)
        with patch("scripts.ci_run.importlib.import_module", return_value=runner):
            with self.assertRaises(ZeroDivisionError):
                main(["bench", "1/4"])
        self.assertIs(runner.load_cases, discovered)


class RunnerTests(unittest.TestCase):
    def listed(self, *args, cwd=None):
        output = subprocess.check_output([sys.executable, *args], text=True, cwd=cwd)
        return [line.split()[0] for line in output.splitlines()]

    def test_real_runners_cover_each_case_once(self):
        root = Path(__file__).resolve().parent.parent
        for suite, count, command in (
            ("bench", 4, ["-m", "bench.run"]),
            ("test", 2, ["tests/run.py"]),
        ):
            with self.subTest(suite=suite):
                expected = self.listed(*command, "--list", cwd=root)
                shards = [
                    self.listed("scripts/ci_run.py", suite, f"{i + 1}/{count}", "--list", cwd=root)
                    for i in range(count)
                ]
                actual = [name for shard in shards for name in shard]
                self.assertEqual(Counter(actual), Counter(expected))
                self.assertTrue(all(shards))

    def test_benchmark_slow_option_is_unchanged(self):
        root = Path(__file__).resolve().parent.parent
        expected = self.listed("-m", "bench.run", "--slow", "--list", cwd=root)
        actual = self.listed("scripts/ci_run.py", "bench", "1/1", "--slow", "--list", cwd=root)
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
