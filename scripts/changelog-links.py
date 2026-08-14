#!/usr/bin/env python3
"""Link CHANGELOG.md entries to the commits that introduced them.

    python3 scripts/changelog-links.py            # rewrite in place
    python3 scripts/changelog-links.py --check    # fail if any entry is bare

A commit cannot contain its own hash, so entries are written bare and linked
later, during release prep. An entry is attributed to the oldest commit that
added its opening line to the file; one that is not committed yet is reported
and left alone. The initial release predates the file and is skipped.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

CHANGELOG = Path("CHANGELOG.md")
FALLBACK_REMOTE = "https://github.com/bissakov/arqan"
SHORT_LEN = 7
WRAP = 79

HEADING_RE = re.compile(r"^## \[([^\]]+)\]")
ITEM_RE = re.compile(r"^(\s*)- ")
LINKED_RE = re.compile(r"\(\[`[0-9a-f]{7,40}`\]\)|/commit/[0-9a-f]{7,40}")
DEF_RE = re.compile(r"^\[`([0-9a-f]{7,40})`\]: (\S+)\s*$")


def git(*args: str) -> str:
    out = subprocess.run(
        ["git", *args], capture_output=True, text=True, check=True
    )
    return out.stdout


def commit_base_url() -> str:
    try:
        url = git("remote", "get-url", "origin").strip()
    except subprocess.CalledProcessError:
        url = ""
    if not url:
        url = FALLBACK_REMOTE
    if url.startswith("git@"):
        host, _, path = url[4:].partition(":")
        url = f"https://{host}/{path}"
    if url.endswith(".git"):
        url = url[:-4]
    return url.rstrip("/") + "/commit/"


def line_origins(path: Path) -> dict[str, str]:
    """Map an added changelog line to the oldest commit that added it."""
    log = git(
        "log", "--format=%x00%H", "--patch", "--follow", "--", str(path)
    )
    origins: dict[str, str] = {}
    commit = ""
    for line in log.splitlines():
        if line.startswith("\x00"):
            commit = line[1:].strip()
        elif line.startswith("+") and not line.startswith("+++"):
            text = line[1:].rstrip()
            if text:
                origins[text] = commit  # newest first, so the last wins
    return origins


class Entry:
    def __init__(self, start: int, indent: str, section: str) -> None:
        self.start = start
        self.end = start
        self.indent = indent
        self.section = section


def parse_entries(lines: list[str]) -> list[Entry]:
    entries: list[Entry] = []
    section = ""
    current: Entry | None = None
    for i, raw in enumerate(lines):
        line = raw.rstrip("\n")
        heading = HEADING_RE.match(line)
        if heading:
            section = heading.group(1)
            current = None
            continue
        if DEF_RE.match(line) or line.startswith("["):
            current = None
            continue
        item = ITEM_RE.match(line)
        if item:
            current = Entry(i, item.group(1), section)
            entries.append(current)
        elif current is not None:
            if line.strip() and line.startswith(current.indent + " "):
                current.end = i
            else:
                current = None
    return entries


def entry_text(lines: list[str], entry: Entry) -> str:
    body = [lines[i].strip() for i in range(entry.start, entry.end + 1)]
    return " ".join(body)


def append_link(lines: list[str], entry: Entry, short: str) -> None:
    ref = f"([`{short}`])"
    last = lines[entry.end].rstrip("\n")
    joined = f"{last} {ref}"
    if len(joined) <= WRAP:
        lines[entry.end] = joined + "\n"
    else:
        lines.insert(entry.end + 1, f"{entry.indent}  {ref}\n")


def rewrite_definitions(lines: list[str], base: str) -> list[str]:
    kept = [ln for ln in lines if not DEF_RE.match(ln.rstrip("\n"))]
    while kept and not kept[-1].strip():
        kept.pop()
    refs: list[str] = []
    seen: set[str] = set()
    for line in kept:
        for short in re.findall(r"\(\[`([0-9a-f]{7,40})`\]\)", line):
            if short not in seen:
                seen.add(short)
                refs.append(short)
    if not refs:
        return kept + ["\n"] if kept else kept
    full = {}
    for short in refs:
        full[short] = git("rev-parse", short).strip()
    kept.append("\n")
    kept += [f"[`{s}`]: {base}{full[s]}\n" for s in refs]
    return kept


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--file", type=Path, default=CHANGELOG)
    ap.add_argument(
        "--check",
        action="store_true",
        help="report unlinked entries and exit nonzero, changing nothing",
    )
    ap.add_argument(
        "--skip-section",
        action="append",
        default=None,
        metavar="VERSION",
        help="changelog section to leave unlinked (default: 0.1.0)",
    )
    args = ap.parse_args()
    skip = set(args.skip_section if args.skip_section is not None else ["0.1.0"])

    path = args.file
    if not path.is_file():
        print(f"{path}: not found", file=sys.stderr)
        return 2
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)

    origins = line_origins(path)
    entries = parse_entries(lines)

    pending: list[tuple[Entry, str]] = []
    unresolved: list[str] = []
    for entry in entries:
        if entry.section in skip:
            continue
        text = entry_text(lines, entry)
        if LINKED_RE.search(text):
            continue
        head = lines[entry.start].rstrip("\n")
        commit = origins.get(head)
        if not commit:
            unresolved.append(f"{path}:{entry.start + 1}: {text[:60]}")
            continue
        pending.append((entry, commit[:SHORT_LEN]))

    if args.check:
        for entry, short in pending:
            text = entry_text(lines, entry)
            print(f"{path}:{entry.start + 1}: missing ({short}): {text[:60]}")
        for miss in unresolved:
            print(f"{miss} (not committed yet)")
        return 1 if pending or unresolved else 0

    for entry, short in sorted(pending, key=lambda p: -p[0].start):
        append_link(lines, entry, short)

    updated = rewrite_definitions(lines, commit_base_url())
    text = "".join(updated)
    if text != path.read_text(encoding="utf-8"):
        path.write_text(text, encoding="utf-8")
    for entry, short in pending:
        print(f"linked {short}")
    for miss in unresolved:
        print(f"skipped, not committed yet: {miss}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
