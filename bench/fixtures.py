"""Workloads a benchmark runs against: sessions, trees, files and prose.

Nothing here measures anything. It builds the inputs that make a measurement
mean something: a session of a known token count with a known number of
planted markers, a source tree of a known shape, a file of a known width.
Sizes are the caller's, so a case scales its own workload.
"""

from __future__ import annotations

import json
import random
import re
from pathlib import Path

# The generated text is ordinary prose, so a token is about four characters.
CHARS_PER_TOKEN = 4

WORDS = (
    "context window token stream arena transcript provider session buffer "
    "render wrap scroll resume persist scratch message tool result patch "
    "grep find bash prompt config settings endpoint history telemetry"
).split()

CODE_WORDS = (
    "static void size_t buf arena str agent tool json http conf render tui "
    "return if else while for const struct enum typedef"
).split()


def prose(rng: random.Random, words: int, vocab=WORDS) -> str:
    return " ".join(rng.choice(vocab) for _ in range(words))


def paragraphs(rng: random.Random, words: int, vocab=WORDS,
               low: int = 40, high: int = 90) -> list[str]:
    """Split `words` words into paragraphs of a realistic spread."""
    out, left = [], words
    while left > 0:
        take = min(left, rng.randint(low, high))
        out.append(prose(rng, take, vocab))
        left -= take
    return out


# ---- sessions -------------------------------------------------------------


def session_dir(ctx) -> Path:
    """The per-cwd session directory, reusing the slug arqan itself made.

    A case that ran a real turn first has one already; otherwise the slug is
    rebuilt the same way so that /resume finds what is planted here.
    """
    base = ctx.home / ".local" / "share" / "arqan" / "sessions"
    base.mkdir(parents=True, exist_ok=True)
    subs = [p for p in base.iterdir() if p.is_dir()]
    target = subs[0] if subs else base / str(ctx.work).replace("/", "%2f")
    target.mkdir(parents=True, exist_ok=True)
    return target


class PlantedSession:
    def __init__(self, path: Path, tokens: int, markers: int, tag: str):
        self.path = path
        self.tokens = tokens
        self.markers = markers
        self.tag = tag

    @property
    def megabytes(self) -> float:
        return self.path.stat().st_size / 1e6

    def describe(self) -> str:
        return (f"{self.path.name}: {self.megabytes:.1f} MB, "
                f"~{self.tokens // 1000}k tokens, {self.markers} markers")


def plant_session(ctx, name: str, tokens: int, turns: int = 40,
                  tag: str = "", marker: str = "", markers: int = 0,
                  seed: int = 1234, one_line: bool = False) -> PlantedSession:
    """Write a session file of roughly `tokens` tokens, as arqan would.

    `tag` is salted through the text so a resumed screen says which session is
    on it: a benchmark that measures a resume which quietly did nothing is
    worthless. `marker`/`markers` plant a countable needle spread evenly over
    the turns, so the oldest and newest both carry one and a search has to
    walk everything to count them.
    """
    target = session_dir(ctx)
    rng = random.Random(seed)
    vocab = WORDS + [tag] * 4 if tag else WORDS
    avg_word = sum(len(w) for w in vocab) / len(vocab) + 1
    words_per_msg = max(8, int(tokens * CHARS_PER_TOKEN / turns / avg_word))
    per_turn = markers // turns if turns else 0

    path = target / f"{name}.jsonl"
    planted = 0
    with path.open("w") as f:
        for i in range(turns):
            user = f"turn {i} {tag}: " + prose(rng, 12, vocab)
            f.write(json.dumps({"role": "user", "content": user}) + "\n")
            if one_line:
                # One unbroken line is a different wrap path from real replies.
                lines = [prose(rng, words_per_msg, vocab)]
            else:
                lines = paragraphs(rng, words_per_msg, vocab)
            for k in range(min(per_turn, len(lines))):
                lines[k] += " " + marker
                planted += 1
            if tag:
                # The tail of the last reply is what a resumed screen shows,
                # so every paragraph carries the tag: a check that the resume
                # painted this session must not depend on the word order.
                lines = [f"{tag} {line}" for line in lines]
            reply = "\n".join(lines) if one_line else "\n\n".join(lines)
            f.write(json.dumps({"role": "assistant", "content": reply}) + "\n")
    return PlantedSession(path, tokens, planted, tag)


def seed_session(ctx, s=None):
    """Run one real turn so arqan creates its own session directory."""
    ctx.scenario("text=ok")
    s = s or ctx.spawn()
    s.submit("seed")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()
    return s


def resume_at(s, index: int, timeout: float = 180.0):
    """Open /resume and pick the entry `index` rows down, newest first.

    Selection is by position rather than by typing: the picker lists sessions
    under formatted timestamps, and stepping to a known row keeps a benchmark
    independent of how the filter treats them.
    """
    s.submit("/resume")
    s.wait_text("pick a session", timeout=timeout)
    for _ in range(index):
        s.key("down")
    s.settle()
    s.key("enter")
    s.wait_gone("pick a session", timeout=timeout)
    s.settle(timeout=timeout)
    return s


# ---- the find box ---------------------------------------------------------


def find_row(s) -> str:
    for r in range(s.term.rows):
        if "find:" in s.row(r):
            return s.row(r).strip()
    return ""


def find_counts(s) -> tuple[int, int]:
    """(index, count) from the find box, (0, 0) when it reports no match."""
    m = re.search(r"(\d+) of (\d+)", find_row(s))
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


# ---- files and trees ------------------------------------------------------


def big_file(path: Path, lines: int, cols: int = 80, seed: int = 5,
             needle: str = "", every: int = 0) -> Path:
    """A text file of `lines` lines, optionally carrying a countable needle."""
    rng = random.Random(seed)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        for i in range(lines):
            line = f"{i:07d} " + prose(rng, max(1, cols // 7))
            if needle and every and i % every == 0:
                line += " " + needle
            f.write(line[:cols] + "\n")
    return path


def source_tree(root: Path, dirs: int, files_per_dir: int, lines: int,
                needle: str = "", hit_every: int = 7, seed: int = 11) -> dict:
    """A tree of small C-like files, returning its shape and needle count.

    Realistic enough to exercise the walk: nested directories, several
    extensions, and one file per directory that a glob must reject.
    """
    rng = random.Random(seed)
    exts = (".c", ".h", ".py", ".md", ".json")
    files = hits = 0
    for d in range(dirs):
        # Nest a third of the tree two levels deep.
        sub = root / f"pkg{d:03d}" if d % 3 else root / f"pkg{d:03d}" / "inner"
        sub.mkdir(parents=True, exist_ok=True)
        for i in range(files_per_dir):
            path = sub / f"mod{i:03d}{exts[i % len(exts)]}"
            with path.open("w") as f:
                for n in range(lines):
                    text = prose(rng, 8, CODE_WORDS)
                    if needle and (n % hit_every) == 0:
                        text += " " + needle
                        hits += 1
                    f.write(f"{text}\n")
            files += 1
    return {"files": files, "hits": hits, "dirs": dirs}


def markdown_doc(rng: random.Random, sections: int, words: int = 90) -> str:
    """A document that exercises every renderer: headings, lists, tables, code.

    Fenced blocks name a language, so this is also the syntax highlighter's
    workload.
    """
    out: list[str] = []
    for i in range(sections):
        out.append(f"## section {i}: " + prose(rng, 5))
        out.append(prose(rng, words))
        out.append("\n".join(f"- item {j}: " + prose(rng, 8) for j in range(4)))
        out.append(
            "| key | value | note |\n| --- | --- | --- |\n"
            + "\n".join(
                f"| {prose(rng, 1)} | {rng.randint(0, 9999)} | {prose(rng, 3)} |"
                for _ in range(4)
            )
        )
        out.append(
            "```c\n"
            + "\n".join(
                f"static size_t f{j}(Arena *a, Str s) {{ return s.n + {j}; }}"
                for j in range(6)
            )
            + "\n```"
        )
        out.append("> " + prose(rng, 20))
        out.append("Inline `code`, **bold**, _italic_ and a [link](http://x/y).")
    return "\n\n".join(out)


def wide_text(rng: random.Random, words: int) -> str:
    """Prose mixing CJK, emoji and combining marks, for the width tables."""
    pieces = []
    for i in range(words):
        pick = i % 4
        if pick == 0:
            pieces.append(prose(rng, 1))
        elif pick == 1:
            pieces.append("".join(chr(0x4E00 + rng.randint(0, 2000))
                                  for _ in range(3)))
        elif pick == 2:
            pieces.append("\U0001f600\U0001f680\u2764\ufe0f")
        else:
            pieces.append("e\u0301a\u0300o\u0308" * 2)
    return " ".join(pieces)


def unified_diff(path: Path, hunks: int, spacing: int = 40,
                 name: str = "") -> str:
    """A patch inserting a line at `hunks` places of an existing file.

    Context is copied out of the file itself, since hunks are located by
    their context lines: a diff built from invented text measures the
    rejection path and nothing else.
    """
    lines = path.read_text().splitlines()
    rel = name or path.name
    out = [f"--- a/{rel}", f"+++ b/{rel}"]
    spacing = max(3, spacing)
    for h in range(hunks):
        at = h * spacing + 1
        if at + 1 >= len(lines):
            break
        out.append(f"@@ -{at},2 +{at},3 @@")
        out.append(f" {lines[at - 1]}")
        out.append(f"+inserted line for hunk {h}")
        out.append(f" {lines[at]}")
    return "\n".join(out) + "\n"
