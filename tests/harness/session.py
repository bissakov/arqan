"""Drive `yoke` inside a pseudo-terminal and observe the rendered screen.

The pty gives the binary a real tty (so it takes the fullscreen path and
enables raw mode), and every byte it writes is replayed into the emulator in
`vt.py`. Tests therefore assert against what a user would see rather than
against escape sequences.

Reproducibility comes from waiting on states, never on wall-clock sleeps:
`wait_for` polls the emulated screen until a predicate holds, and `wait_idle`
returns once the process has stopped emitting for a quiet period.
"""

from __future__ import annotations

import errno
import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import termios
import time

from . import keys as K
from .vt import Terminal

# A quiet window has to outlast the largest gap the frames it waits on can
# have; below that floor we are only bridging one repaint, which `yoke` emits as
# a single write. Scenarios that pace their deltas widen it (see `Ctx.quiet`),
# and YOKE_TEST_QUIET raises the floor for machines too slow for the default.
QUIET = max(0.06, float(os.environ.get("YOKE_TEST_QUIET") or 0))

# `yoke` keeps ISIG on, so Ctrl-C is a signal from the line discipline, not a
# byte, which only happens if the child owns the pty as its controlling
# terminal. setsid(1) does the setsid + TIOCSCTTY dance before exec with no
# Python in between, which matters because cases run in parallel threads and
# a preexec_fn runs the interpreter in a forked, threaded child.
SETSID = shutil.which("setsid")


class TimeoutError_(AssertionError):
    """Raised as an assertion so the runner reports it as a test failure."""


class Session:
    def __init__(
        self,
        argv,
        env=None,
        cwd=None,
        cols=80,
        rows=24,
        name="yoke",
        quiet=QUIET,
    ):
        self.argv = list(argv)
        self.env = dict(env or {})
        self.cwd = cwd
        self.cols = cols
        self.rows = rows
        self.name = name
        self.quiet = max(quiet, QUIET)
        self.term = Terminal(cols, rows)
        self.raw = bytearray()
        self.proc: subprocess.Popen | None = None
        self.master = -1
        self._eof = False
        self._exit_status: int | None = None

    # ---- lifecycle --------------------------------------------------------
    def start(self) -> "Session":
        master, slave = pty.openpty()
        _set_winsize(master, self.cols, self.rows)
        _set_winsize(slave, self.cols, self.rows)

        argv = [SETSID, "--ctty", *self.argv] if SETSID else list(self.argv)
        self.proc = subprocess.Popen(
            argv,
            stdin=slave,
            stdout=slave,
            stderr=slave,
            cwd=self.cwd,
            env=self.env,
            preexec_fn=None if SETSID else _child_setup,
            close_fds=True,
        )
        os.close(slave)
        self.master = master
        flags = fcntl.fcntl(master, fcntl.F_GETFL)
        fcntl.fcntl(master, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        return self

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.close()

    # ---- output -----------------------------------------------------------
    def _read_once(self, timeout: float) -> bytes:
        if self.master < 0 or self._eof:
            return b""
        try:
            r, _, _ = select.select([self.master], [], [], timeout)
        except (OSError, ValueError):
            self._eof = True
            return b""
        if not r:
            return b""
        try:
            data = os.read(self.master, 65536)
        except OSError as e:
            # Linux reports the child's exit on a pty master as EIO.
            if e.errno in (errno.EIO, errno.EBADF):
                self._eof = True
                return b""
            raise
        if not data:
            self._eof = True
            return b""
        self.raw += data
        self.term.feed(data)
        return data

    def pump(self, timeout: float = 0.02) -> bytes:
        return self._read_once(timeout)

    def wait_idle(
        self,
        quiet: float | None = None,
        timeout: float = 10.0,
        require_output: bool = False,
    ) -> "Session":
        """Read until nothing arrives for `quiet` seconds.

        With `require_output`, at least one byte must arrive first, which is
        what makes "send a key, then assert" safe: the screen we inspect is
        never the one from before the key.
        """
        quiet = self.quiet if quiet is None else quiet
        deadline = time.monotonic() + timeout
        last = time.monotonic()
        seen = not require_output
        while time.monotonic() < deadline:
            got = self._read_once(min(0.02, quiet / 3))
            now = time.monotonic()
            if got:
                seen = True
                last = now
                continue
            if seen and now - last >= quiet:
                return self
            if self._eof:
                return self
        raise TimeoutError_(
            f"{self.name}: no quiet {quiet}s window within {timeout}s\n"
            + self.debug_dump()
        )

    def wait_for(self, pred, what: str = "condition", timeout: float = 10.0):
        """Poll the emulated screen until `pred(term)` is true."""
        deadline = time.monotonic() + timeout
        while True:
            if pred(self.term):
                return self
            if time.monotonic() >= deadline:
                raise TimeoutError_(
                    f"{self.name}: timed out waiting for {what}\n"
                    + self.debug_dump()
                )
            if self._eof and not self._read_once(0.01):
                # process is gone; give the predicate one last look
                if pred(self.term):
                    return self
                raise TimeoutError_(
                    f"{self.name}: process exited while waiting for {what}\n"
                    + self.debug_dump()
                )
            self._read_once(0.02)

    def wait_text(self, needle: str, timeout: float = 10.0):
        return self.wait_for(
            lambda t: t.contains(needle), f"text {needle!r}", timeout
        )

    def wait_gone(self, needle: str, timeout: float = 10.0):
        return self.wait_for(
            lambda t: not t.contains(needle), f"{needle!r} to disappear", timeout
        )

    def wait_status(self, status: str, timeout: float = 10.0):
        return self.wait_for(
            lambda t: self.status_kind() == status,
            f"status {status!r} (now {self.status_kind()!r})",
            timeout,
        )

    def settle(self, quiet: float | None = None, timeout: float = 10.0):
        """Idle wait used right before taking a golden snapshot."""
        return self.wait_idle(quiet, timeout)

    def sync(self, quiet: float | None = None, timeout: float = 10.0):
        """Wait for the repaint caused by the input just sent, then settle."""
        return self.wait_idle(quiet, timeout, require_output=True)

    # ---- input ------------------------------------------------------------
    def send(self, data) -> "Session":
        if isinstance(data, str):
            data = data.encode("utf-8")
        if self.master < 0:
            raise AssertionError("session not started")
        off = 0
        while off < len(data):
            try:
                off += os.write(self.master, data[off:])
            except BlockingIOError:
                select.select([], [self.master], [], 0.1)
            except OSError as e:
                if e.errno == errno.EIO:
                    break
                raise
        return self

    def key(self, *names: str) -> "Session":
        return self.send(K.encode_all(*names))

    def type(self, text: str, per_key_settle: bool = False) -> "Session":
        """Type literal text. Bytes go out one at a time, like a real user."""
        for ch in text:
            self.send(ch)
            if per_key_settle:
                self.pump(0.01)
        return self

    def paste(self, text: str) -> "Session":
        """Bracketed paste, delivered in one write the way a terminal sends it."""
        return self.send(K.encode("paste-start") + text + K.encode("paste-end"))

    def mouse(self, event: str, row: int, col: int, button: int = 0):
        return self.send(K.mouse(event, row, col, button))

    def submit(self, text: str | None = None, timeout: float = 10.0) -> "Session":
        """Type a message and press Enter, returning once it was accepted.

        Submitting is what empties the composer, so that is the signal a turn
        actually started; waiting on the status would race the repaint that
        follows it.
        """
        if text is not None:
            self.type(text).sync()
        self.key("enter")
        return self.wait_for(
            lambda t: self.composer_text() == "", "the composer to clear", timeout
        )

    # ---- the settings screen ---------------------------------------------
    def popup_selected(self) -> str:
        """The highlighted row of an open popup, picker or settings screen.

        The composer carries the same marker, so this is the first such row:
        every overlay is painted above it.
        """
        marker = "\u203a "
        rows = [l.strip() for l in self.term.lines() if l.lstrip().startswith(marker)]
        return rows[0] if rows else ""

    def open_settings(self, timeout: float = 10.0) -> "Session":
        self.submit("/settings")
        return self.wait_text("Verbose tool output", timeout)

    def settings_select(self, label: str) -> "Session":
        """Move the selection onto the row holding `label`."""
        for _ in range(16):
            if label in self.popup_selected():
                return self
            self.key("down").sync()
        raise AssertionError(f"no settings row for {label!r}\n{self.text()}")

    def settings_act(self, label: str) -> "Session":
        """Open the settings, act on `label` with Space, and leave it open."""
        return self.open_settings().settings_select(label).key("space").sync()

    def settings_toggle(self, label: str) -> "Session":
        """Open, flip the checkbox on `label`, wait for it, close with Escape.

        The box is the screen's own answer, so it is also what says the toggle
        landed; nothing here waits on a message.
        """
        self.open_settings().settings_select(label)
        want = "[ ] " if "[x] " in self.popup_selected() else "[x] "
        self.key("space").sync()
        self.wait_for(
            lambda t: t.contains(want + label), f"{label} to read {want!r}"
        )
        self.key("esc")
        return self.wait_gone("Verbose tool output")

    def wait_turn_done(self, timeout: float = 30.0) -> "Session":
        """Wait for the agent loop to go back to idle.

        The leading settle lets the busy/thinking repaints that immediately
        follow a submit land, so 'ready' cannot be observed from before the
        turn began.
        """
        self.settle()
        self.wait_for(lambda t: self.status_kind() == "ready", "turn to finish", timeout)
        return self.settle()

    def resize(self, cols: int, rows: int) -> "Session":
        self.cols, self.rows = cols, rows
        _set_winsize(self.master, cols, rows)
        self.term.resize(cols, rows)
        if self.proc and self.proc.poll() is None:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGWINCH)
        return self

    def signal(self, sig: int) -> "Session":
        if self.proc and self.proc.poll() is None:
            os.killpg(os.getpgid(self.proc.pid), sig)
        return self

    # ---- inspection -------------------------------------------------------
    @property
    def screen(self) -> Terminal:
        return self.term

    def text(self) -> str:
        return self.term.text()

    def row(self, i: int) -> str:
        return self.term.row_text(i if i >= 0 else self.term.rows + i)

    def status_line(self) -> str:
        return self.row(-1).strip()

    # The status line reads "●  <state>  ·  <model>  ·  <provider>  · ...".
    def status_field(self, index: int = 0) -> str:
        parts = [p.strip() for p in self.status_line().split("\u00b7")]
        if not parts:
            return ""
        parts[0] = parts[0].lstrip("\u25cf ").strip()
        return parts[index] if index < len(parts) else ""

    def status_kind(self) -> str:
        return self.status_field(0)

    # The bullet repeats the state as a colour, which is what a glance at the
    # screen actually picks up.
    STATUS_COLOURS = {
        114: "ready",      # S_GREEN
        177: "thinking",   # S_PURPLE
        203: "error",      # S_RED
        75: "working",     # S_BLUE, any other status string
    }

    def status_colour(self) -> str | None:
        fg = self.term.attr_at(self.term.rows - 1, self.gutter()).fg
        return self.STATUS_COLOURS.get(fg)

    # The frame's geometry is fixed relative to the bottom: status line, a
    # blank separator, the composer's lower padding row, then the composer
    # itself. That makes the composer addressable without guessing at colours,
    # which NO_COLOR strips from golden runs anyway.
    def gutter(self) -> int:
        return 2 if self.term.cols >= 24 else 1

    def composer_lines(self, count: int = 1) -> list[str]:
        last = self.term.rows - 4
        return [
            self.term.row_text(last - count + 1 + i).rstrip() for i in range(count)
        ]

    PLACEHOLDER = "Message yoke..."

    def transcript_height(self, composer_rows: int = 1, popup_rows: int = 0) -> int:
        """Rows the transcript occupies, from the fixed bottom chrome.

        Chrome is the status line, the blank row above it, the composer's two
        padding rows and the blank row that keeps the transcript off them, all
        of which collapse on a very short screen.
        """
        padding = 1 if self.term.rows >= 6 else 0
        chrome = 1 + padding * 4
        return max(1, self.term.rows - composer_rows - chrome - popup_rows)

    def scrollbar(self, composer_rows: int = 1, popup_rows: int = 0) -> list[str]:
        """The scrollbar column drawn beside the transcript."""
        col = self.term.cols - 1
        rows = self.transcript_height(composer_rows, popup_rows)
        return [self.term.buf.chars[r][col] for r in range(rows)]

    def composer_body(self, count: int = 1) -> list[str]:
        """Composer rows with the gutter and the '› ' prompt removed.

        The composer panel pads every row to the full width, so trailing
        spaces in the typed text are not recoverable from the screen; rows
        come back right-stripped. The empty-composer placeholder is chrome and
        reads back as an empty row.
        """
        g = self.gutter()
        out = []
        for i, line in enumerate(self.composer_lines(count)):
            skip = g + 2 if i == 0 else g
            body = line[skip:] if len(line) > skip else ""
            if i == 0 and body == self.PLACEHOLDER:
                body = ""
            out.append(body)
        return out

    def composer_text(self, count: int = 1) -> str:
        """Composer content, wrapped rows joined back into one string."""
        return "".join(self.composer_body(count))

    @property
    def cursor(self) -> tuple[int, int]:
        """Cursor as 1-based (row, col), matching the CUP the UI emits."""
        return (self.term.row + 1, self.term.col + 1)

    def composer_rows(self) -> list[str]:
        return [line for line in self.term.lines() if line.lstrip().startswith("\u203a ")]

    def snapshot(self, label: str = "") -> str:
        return self.term.snapshot(label)

    def debug_dump(self) -> str:
        return (
            "----- screen -----\n"
            + self.term.snapshot()
            + "----- last raw bytes -----\n"
            + repr(bytes(self.raw[-1500:]))
            + "\n"
        )

    # ---- shutdown ---------------------------------------------------------
    def wait_exit(self, timeout: float = 5.0) -> int:
        if not self.proc:
            return -1
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._read_once(0.02)
            rc = self.proc.poll()
            if rc is not None:
                self._exit_status = rc
                return rc
        raise TimeoutError_(
            f"{self.name}: did not exit within {timeout}s\n" + self.debug_dump()
        )

    def close(self):
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=2)
            except Exception:
                try:
                    self.proc.kill()
                    self.proc.wait(timeout=2)
                except Exception:
                    pass
        if self.master >= 0:
            try:
                os.close(self.master)
            except OSError:
                pass
            self.master = -1


def _child_setup():  # only without setsid(1); see SETSID above
    os.setsid()
    try:
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    except OSError:
        pass


def _set_winsize(fd: int, cols: int, rows: int):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
