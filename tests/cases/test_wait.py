"""What the harness accepts as the screen that answers a keystroke.

These drive `Session.wait_idle` against a scripted child, so the timing the
cases depend on is decided here rather than by how loaded the machine is.
"""

import time

from tests.harness.session import Session


def beacon(seq: int, consumed: int) -> bytes:
    """A park: a settled frame, and the input count read so far."""
    return f"\x1b_agent;idle;{seq};{consumed};80x24\x1b\\".encode()


def notice(consumed: int) -> bytes:
    """The poll path reporting input taken, with no frame behind it."""
    return f"\x1b_agent;input;{consumed}\x1b\\".encode()


class Scripted(Session):
    """A session whose output is a script of (delay, bytes), not a child."""

    def __init__(self, script, sent):
        super().__init__(["/nonexistent"])
        self._script = list(script)
        self._sent = sent
        self._begin = time.monotonic()

    def _read_once(self, timeout: float) -> bytes:
        if self._script and time.monotonic() - self._begin >= self._script[0][0]:
            _, data = self._script.pop(0)
            self.feed(data)
            return data
        time.sleep(min(timeout, 0.005))
        return b""


def test_a_beacon_behind_the_bytes_sent_is_not_the_repaint(ctx):
    """The park before the read that takes a key must not end the wait.

    The child writes it on its way into that read, so it arrives after the
    key and moves no cell. Settling on it hands the case the frame that
    answers the previous keystroke.
    """
    late = 0.4
    s = Scripted(
        [(0.0, beacon(7, 1)), (late, b"\x1b[H[ ] Model" + beacon(8, 2))],
        sent=2,
    )
    began = time.monotonic()
    s.sync(timeout=5.0)
    assert "[ ] Model" in s.text(), s.text()
    assert time.monotonic() - began >= late


def test_input_taken_with_no_frame_still_settles(ctx):
    """A turn holds the loop and reads through the poll path, which parks in
    no read: the count arrives without a frame, and a key that paints
    nothing has nothing else to wait for."""
    s = Scripted([(0.0, beacon(7, 1)), (0.02, notice(2))], sent=2)
    began = time.monotonic()
    s.sync(timeout=5.0)
    assert time.monotonic() - began < 1.0


def test_a_park_naming_the_bytes_sent_ends_the_wait_at_once(ctx):
    """The unambiguous case stays fast: no quiet window is waited out."""
    s = Scripted([(0.0, b"\x1b[Hdone" + beacon(9, 4))], sent=4)
    began = time.monotonic()
    s.sync(timeout=5.0)
    assert "done" in s.text(), s.text()
    assert time.monotonic() - began < 0.05
