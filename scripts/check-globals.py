#!/usr/bin/env python3
"""Fail on a loose global scalar in src/.

AGENTS.md, "Global state": every global belongs to a named static state struct
that owns one concern. A bare `static i32 g_thing;` beside such a struct is the
shape that regrows the coupling, so this rejects it by type: a global whose
declared type is a primitive, a pointer, or a function pointer.

Two exceptions pass, matching the doctrine: a `volatile sig_atomic_t` a signal
handler writes, and anything inside an `#ifdef AGENT_TESTING` block, which the
shipped binary does not contain.

`static const` data is immutable and couples nothing, so it passes too.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"

# A global whose type is one of these holds no structure and owns no concern.
SCALAR = r"(?:b8|i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|size_t|ssize_t|char|" \
         r"int|short|long|unsigned|signed|float|double|pid_t|time_t|void)"

LOOSE = re.compile(
    r"^static\s+(?!const\b)" + SCALAR + r"(?:\s+(?:int|long|char))*\s*"
    r"\**\s*(?:\(\s*\*\s*)?(g_\w+)"
)

# static volatile sig_atomic_t g_name  ->  written by a signal handler.
SIGNAL = re.compile(r"^static\s+volatile\s+sig_atomic_t\s+\**\s*(g_\w+)")


def offenders(path):
    testing = 0
    for n, line in enumerate(path.read_text().splitlines(), 1):
        bare = line.strip()
        if re.match(r"^#\s*if(def)?\b", bare):
            testing += 1 if (testing or "AGENT_TESTING" in bare) else 0
        elif testing and re.match(r"^#\s*endif\b", bare):
            testing -= 1
        if testing or SIGNAL.match(line):
            continue
        m = LOOSE.match(line)
        if m:
            yield n, m.group(1), bare


def main():
    bad = [(p, n, name, text)
           for p in sorted(SRC.glob("*.c"))
           for n, name, text in offenders(p)]
    for p, n, name, text in bad:
        print(f"{p.relative_to(SRC.parent.parent)}:{n}: loose global {name}: {text}")
    if bad:
        print()
        print(f"{len(bad)} loose global(s). Add the field to the state struct "
              f"that owns the concern; see AGENTS.md, \"Global state\".")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
