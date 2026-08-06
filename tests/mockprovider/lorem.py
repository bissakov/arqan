"""Deterministic lorem ipsum.

A self-contained LCG rather than `random` so the same seed produces the same
text on every Python build — golden screen snapshots depend on it.
"""

from __future__ import annotations

WORDS = (
    "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod "
    "tempor incididunt ut labore et dolore magna aliqua enim ad minim veniam "
    "quis nostrud exercitation ullamco laboris nisi aliquip ex ea commodo "
    "consequat duis aute irure in reprehenderit voluptate velit esse cillum "
    "eu fugiat nulla pariatur excepteur sint occaecat cupidatat non proident "
    "sunt culpa qui officia deserunt mollit anim id est laborum"
).split()


class Lcg:
    """Numerical Recipes constants; only used to pick word indices."""

    def __init__(self, seed: int):
        self.state = (seed * 2654435761 + 1) & 0xFFFFFFFF

    def next(self) -> int:
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state >> 8

    def below(self, n: int) -> int:
        return self.next() % n if n > 0 else 0


def words(count: int, seed: int = 7) -> list[str]:
    rng = Lcg(seed)
    return [WORDS[rng.below(len(WORDS))] for _ in range(count)]


def sentence(word_count: int, seed: int = 7) -> str:
    w = words(word_count, seed)
    if not w:
        return ""
    w[0] = w[0].capitalize()
    return " ".join(w) + "."


def paragraph(word_count: int = 24, seed: int = 7) -> str:
    """One paragraph of roughly `word_count` words, split into sentences."""
    rng = Lcg(seed ^ 0x5A5A)
    out: list[str] = []
    left = word_count
    n = 0
    while left > 0:
        take = min(left, 6 + rng.below(7))
        out.append(sentence(take, seed + n))
        left -= take
        n += 1
    return " ".join(out)


def text(word_count: int = 24, paragraphs: int = 1, seed: int = 7) -> str:
    return "\n\n".join(
        paragraph(word_count, seed + i * 101) for i in range(max(1, paragraphs))
    )
