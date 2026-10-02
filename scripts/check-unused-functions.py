#!/usr/bin/env python3
"""Run unusedFunction lint with unit-test callers and compound-literal calls.

Cppcheck can mistake a call whose first argument is a compound literal for a
definition. Its function summaries still record the call, so use them to check
unusedFunction diagnostics. Unity-build modules share agent.h, so staticFunction
advice does not apply.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent


def summary_calls(summary):
    calls = set()
    for line in summary.splitlines():
        match = re.search(r"^(\S+).*? call:\[([^]]*)\]", line)
        if match:
            calls.update(name for name in match.group(2).split(",")
                         if name and name != match.group(1))
    return calls


def remaining_errors(xml, calls):
    errors = []
    for error in ET.fromstring(xml).findall("./errors/error"):
        name = re.search(r"The function '([^']+)'", error.get("msg", ""))
        if error.get("id") == "unusedFunction" and name and name.group(1) in calls:
            continue
        errors.append(error)
    return errors


def main():
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="unused-", dir=ROOT / "build") as build:
        result = subprocess.run([
            "cppcheck", "--enable=unusedFunction", "--inline-suppr", "--quiet",
            "--error-exitcode=1", "--suppress=staticFunction", "--xml", "--xml-version=2",
            f"--cppcheck-build-dir={build}",
            "-Isrc", *(sys.argv[1:] or ["src/main.c", "tests/unit/main.c"]),
        ], cwd=ROOT, capture_output=True, text=True)
        calls = set()
        for path in Path(build).glob("*.s[0-9]*"):
            calls.update(summary_calls(path.read_text()))
    if result.stdout:
        print(result.stdout, end="")
    try:
        errors = remaining_errors(result.stderr, calls)
    except ET.ParseError:
        print(result.stderr, end="", file=sys.stderr)
        return 1
    for error in errors:
        location = error.find("location")
        path = location.get("file", "cppcheck") if location is not None else "cppcheck"
        line = location.get("line", "0") if location is not None else "0"
        print(f"{path}:{line}: {error.get('severity')}: "
              f"{error.get('msg')} [{error.get('id')}]", file=sys.stderr)
    if result.returncode not in (0, 1):
        print(result.stderr, end="", file=sys.stderr)
        return 1
    if result.returncode and not ET.fromstring(result.stderr).findall("./errors/error"):
        print(result.stderr, end="", file=sys.stderr)
        return 1
    return int(bool(errors))


if __name__ == "__main__":
    sys.exit(main())
