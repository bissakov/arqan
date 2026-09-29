#!/usr/bin/env python3
"""Move sparse rows of a generated parser's dense parse table to the small table.

Tree-sitter stores its busiest parse states as dense LARGE_STATE_COUNT x
SYMBOL_COUNT rows and every other state as grouped (value, symbols) lists.
The runtime reads any state at or above LARGE_STATE_COUNT from the small
table, so moving a row changes the size of the parser, not what it parses.
States 0 and 1 stay dense, as the generator keeps them.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path


DENSE_HEAD = "static const uint16_t ts_parse_table[LARGE_STATE_COUNT][SYMBOL_COUNT] = {\n"
SMALL_HEAD = "static const uint16_t ts_small_parse_table[] = {\n"
MAP_HEAD = "static const uint32_t ts_small_parse_table_map[] = {\n"
TABLE_END = "\n};\n"
ROW_START = re.compile(r"^  \[(?:STATE\()?(\d+)\)?\] = \{$")
ENTRY = re.compile(r"^    \[([A-Za-z0-9_]+)\] = (ACTIONS|STATE)\((\d+)\),$")
MAP_ENTRY = re.compile(r"^  \[SMALL_STATE\((\d+)\)\] = (\d+),$")
DESIGNATOR = re.compile(r"\[\d+\]\s*=")
ALWAYS_DENSE = 2


def fail(path: Path, message: str) -> None:
    raise SystemExit(f"{path}: {message}")


def define(text: str, name: str, path: Path) -> int:
    match = re.search(rf"^#define {name} (\d+)$", text, re.MULTILINE)
    if not match:
        fail(path, f"no {name}")
    return int(match.group(1))


def table(text: str, head: str, path: Path) -> tuple[int, int, str]:
    start = text.find(head)
    if start < 0:
        fail(path, f"no table {head.strip()}")
    body_start = start + len(head)
    end = text.find(TABLE_END, body_start)
    if end < 0:
        fail(path, f"unterminated table {head.strip()}")
    return start, end + len(TABLE_END), text[body_start:end + 1]


def symbol_ids(text: str, path: Path) -> dict[str, int]:
    start = text.find("enum ts_symbol_identifiers {\n")
    end = text.find("\n};\n", start)
    if start < 0 or end < 0:
        fail(path, "no symbol enum")
    ids = {"ts_builtin_sym_end": 0}
    for name, value in re.findall(r"^  (\w+) = (\d+),$", text[start:end],
                                  re.MULTILINE):
        ids[name] = int(value)
    return ids


def dense_rows(body: str, path: Path) -> list[tuple[str, list[tuple[str, str, int]]]]:
    rows: list[tuple[str, list[tuple[str, str, int]]]] = []
    for line in body.splitlines():
        if ROW_START.match(line):
            if int(ROW_START.match(line).group(1)) != len(rows):
                fail(path, "dense rows out of order")
            rows.append((line + "\n", []))
            continue
        if line == "  },":
            rows[-1] = (rows[-1][0] + line + "\n", rows[-1][1])
            continue
        entry = ENTRY.match(line)
        if not entry or not rows:
            fail(path, f"unexpected dense row line: {line!r}")
        rows[-1][1].append((entry.group(1), entry.group(2), int(entry.group(3))))
        rows[-1] = (rows[-1][0] + line + "\n", rows[-1][1])
    return rows


def small_tokens(body: str) -> list[str]:
    tokens = [token.strip() for token in DESIGNATOR.sub("", body).split(",")]
    return [token for token in tokens if token]


def small_states(tokens: list[str], offsets: list[int], path: Path) -> list[list[tuple[str, list[str]]]]:
    states = []
    at = 0
    for offset in offsets:
        if offset != at:
            fail(path, "small table map does not match the table")
        group_count = int(tokens[at])
        at += 1
        groups = []
        for _ in range(group_count):
            value = tokens[at]
            count = int(tokens[at + 1])
            groups.append((value, tokens[at + 2:at + 2 + count]))
            at += 2 + count
        states.append(groups)
    if at != len(tokens):
        fail(path, "small table has trailing data")
    return states


def grouped(entries: list[tuple[str, str, int]], ids: dict[str, int],
            token_count: int, path: Path) -> list[tuple[str, list[str]]]:
    by_value: dict[tuple[int, str, int], list[str]] = {}
    for symbol, kind, value in entries:
        if symbol not in ids:
            fail(path, f"unknown symbol {symbol}")
        terminal = ids[symbol] < token_count
        if terminal != (kind == "ACTIONS"):
            fail(path, f"{symbol} has a {kind} entry")
        by_value.setdefault((0 if terminal else 1, kind, value), []).append(symbol)
    groups = []
    for (rank, kind, value), symbols in by_value.items():
        symbols.sort(key=lambda name: ids[name])
        groups.append(((len(symbols), rank, value, ids[symbols[0]]),
                       (f"{kind}({value})", symbols)))
    groups.sort(key=lambda item: item[0])
    return [group for _, group in groups]


def render_small(states: list[list[tuple[str, list[str]]]]) -> tuple[str, list[int]]:
    lines = []
    offsets = []
    at = 0
    for groups in states:
        offsets.append(at)
        lines.append(f"  [{at}] = {len(groups)},\n")
        at += 1
        for value, symbols in groups:
            lines.append(f"    {value}, {len(symbols)},\n")
            lines.extend(f"      {symbol},\n" for symbol in symbols)
            at += 2 + len(symbols)
    if at > 0xFFFFFFFF:
        raise SystemExit("small parse table too large for a uint32_t map")
    return "".join(lines), offsets


def compact(path: Path, max_entries: int) -> tuple[int, int]:
    text = path.read_text()
    large = define(text, "LARGE_STATE_COUNT", path)
    tokens_n = define(text, "TOKEN_COUNT", path)
    ids = symbol_ids(text, path)
    dense_start, dense_end, dense_body = table(text, DENSE_HEAD, path)
    small_start, small_end, small_body = table(text, SMALL_HEAD, path)
    map_start, map_end, map_body = table(text, MAP_HEAD, path)
    if not dense_end <= small_start < small_end <= map_start:
        fail(path, "tables are not in the expected order")

    rows = dense_rows(dense_body, path)
    if len(rows) != large:
        fail(path, "dense row count does not match LARGE_STATE_COUNT")
    keep = ALWAYS_DENSE
    while keep < large and len(rows[keep][1]) > max_entries:
        keep += 1
    if keep >= large:
        return large, large

    map_entries = [MAP_ENTRY.match(line) for line in map_body.splitlines()]
    if not all(map_entries):
        fail(path, "unexpected small table map line")
    if [int(m.group(1)) for m in map_entries] != list(
            range(large, large + len(map_entries))):
        fail(path, "small table map is not in state order")
    old_small = small_states(small_tokens(small_body),
                             [int(m.group(2)) for m in map_entries], path)

    moved = [grouped(entries, ids, tokens_n, path) for _, entries in rows[keep:]]
    small_text, offsets = render_small(moved + old_small)
    map_text = "".join(f"  [SMALL_STATE({keep + i})] = {offset},\n"
                       for i, offset in enumerate(offsets))
    dense_text = "".join(row_text for row_text, _ in rows[:keep])

    out = (text[:dense_start] + DENSE_HEAD + dense_text + "};\n"
           + text[dense_end:small_start] + SMALL_HEAD + small_text + "};\n"
           + text[small_end:map_start] + MAP_HEAD + map_text + "};\n"
           + text[map_end:])
    out, n = re.subn(r"^#define LARGE_STATE_COUNT \d+$",
                     f"#define LARGE_STATE_COUNT {keep}", out, count=1,
                     flags=re.MULTILINE)
    if n != 1:
        fail(path, "cannot rewrite LARGE_STATE_COUNT")

    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".compact-")
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(out)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise
    return large, keep


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--max-entries", type=int, default=1 << 30,
                        help="move rows with at most this many entries "
                             "(default: every row but the first two)")
    parser.add_argument("parsers", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.parsers:
        before, after = compact(path, args.max_entries)
        print(f"{path}: LARGE_STATE_COUNT {before} -> {after}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
