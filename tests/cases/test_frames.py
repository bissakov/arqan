"""Frames reach the terminal whole.

Every repaint is wrapped in synchronized output, so a reader that lands in
the middle of one has a marker telling it so. The harness uses that to hold
an unfinished frame back, which is what keeps an assertion taken right after
a wait from reading a row that is still being painted.
"""

import re

from tests.harness.session import BSU, ESU, Session

# Chunk sizes that share no factor with anything the UI writes, so the cuts
# land inside frames rather than on their edges.
CHUNKS = (7, 97, 1013)


def busy_capture(ctx):
    """Bytes of a session that queues a message while a turn is running.

    This is the frame the flake was found on: the notice and the emptied
    composer are painted together, so a torn read shows the placeholder
    half-written.
    """
    ctx.scenario("hold,text=done")
    s = ctx.spawn()
    s.submit("go on")
    s.wait_activity("thinking")
    s.type("and then this")
    s.key("enter")
    s.wait_text("message queued")
    ctx.mock.release()
    s.wait_turn_done()
    return bytes(s.raw)


def test_every_frame_is_wrapped_in_synchronized_output(ctx):
    """The marks alternate, so no frame is left open on the terminal."""
    raw = busy_capture(ctx)
    marks = re.findall(re.escape(BSU) + b"|" + re.escape(ESU), raw)
    assert marks, "no synchronized-output marks in the stream"
    assert marks[0] == BSU and marks[-1] == ESU, marks[:2]
    for i, mark in enumerate(marks):
        want = BSU if i % 2 == 0 else ESU
        assert mark == want, f"mark {i} is {mark!r}, expected {want!r}"


def replay(raw: bytes, size: int) -> list[str]:
    """Composer text after each `size`-byte read of `raw`."""
    s = Session(["/nonexistent"])
    seen = []
    for i in range(0, len(raw), size):
        s.feed(raw[i : i + size])
        seen.append(s.composer_text())
    s.feed(b"", flush=True)
    seen.append(s.composer_text())
    return seen


def frame_screens(raw: bytes) -> set:
    """Composer text at every frame boundary: what a user can see."""
    s = Session(["/nonexistent"])
    seen = set()
    for part in raw.split(ESU):
        s.feed(part + ESU)
        seen.add(s.composer_text())
    s.feed(b"", flush=True)
    seen.add(s.composer_text())
    return seen


def test_a_read_that_lands_mid_frame_shows_the_last_finished_one(ctx):
    """Whatever the reads are cut into, the screens observed are the screens
    the frames end on: never "Me" or "Messag" from a placeholder in flight."""
    raw = busy_capture(ctx)

    settled = frame_screens(raw)
    for size in CHUNKS:
        torn = set(replay(raw, size))
        extra = torn - settled
        assert not extra, f"chunks of {size} showed half-painted {extra!r}"
