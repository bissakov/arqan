"""Minimal xterm-compatible screen emulator.

Only the escape repertoire tui.c actually emits is implemented, which is
enough to turn the byte stream on a pty into a deterministic screen snapshot:
CUP, ED, EL, SGR, DEC private modes (alt screen / cursor / autowrap / mouse)
and the OSC writes it makes: 52 for the clipboard, 9 for a desktop
notification.

Unknown sequences are consumed and ignored rather than printed, so a stray
escape shows up as missing output instead of corrupting every later cell.
"""

from __future__ import annotations

import base64
import unicodedata
import re

# ---- character cell widths ------------------------------------------------
# Mirrors what wcwidth(3) reports for the glyphs the UI can paint, so the
# emulator's column accounting matches tui.c's.


def _width_of(ch: str) -> int:
    if ch == "\0":
        return 1
    cat = unicodedata.category(ch)
    if cat in ("Mn", "Me", "Cf"):
        return 0
    if unicodedata.combining(ch):
        return 0
    if unicodedata.east_asian_width(ch) in ("W", "F"):
        return 2
    if cat == "Cc":
        return 0
    return 1


# The screen is repainted whole on every frame, so the same few hundred
# glyphs are measured millions of times per suite. The table is pure, so it
# is memoized once and read from there.
_WIDTHS: dict[str, int] = {chr(c): 1 for c in range(0x20, 0x7F)}


def char_width(ch: str) -> int:
    w = _WIDTHS.get(ch)
    if w is None:
        w = _WIDTHS[ch] = _width_of(ch)
    return w


def text_width(s: str) -> int:
    return sum(char_width(c) for c in s)


class Attr:
    """Style flags of one cell, kept comparable and hashable.

    Instances are immutable and interned by `attr_of`, so a cell holds a
    shared reference rather than a copy: painting a row is a slice assignment
    of one object instead of eighty constructions. Build a changed style with
    `replace`; assigning to a field raises, since the instance is shared with
    every other cell that carries the same style.
    """

    __slots__ = ("fg", "bg", "bold", "reverse", "underline", "_key", "_frozen")

    def __init__(self, fg=None, bg=None, bold=False, reverse=False,
                 underline=False):
        set_ = object.__setattr__
        set_(self, "fg", fg)
        set_(self, "bg", bg)
        set_(self, "bold", bold)
        set_(self, "reverse", reverse)
        set_(self, "underline", underline)
        set_(self, "_key", (fg, bg, bold, reverse, underline))
        set_(self, "_frozen", True)

    def __setattr__(self, name, value):
        raise AttributeError(
            f"Attr is shared between cells and immutable; "
            f"use replace({name}=...)"
        )

    def replace(self, **changes) -> "Attr":
        fg, bg, bold, reverse, underline = self._key
        return attr_of(
            changes.get("fg", fg),
            changes.get("bg", bg),
            changes.get("bold", bold),
            changes.get("reverse", reverse),
            changes.get("underline", underline),
        )

    def copy(self) -> "Attr":
        return self

    def key(self):
        return self._key

    def __eq__(self, other):
        if self is other:
            return True
        return isinstance(other, Attr) and self._key == other._key

    def __hash__(self):
        return hash(self._key)

    def __repr__(self):
        bits = []
        if self.fg is not None:
            bits.append(f"fg={self.fg}")
        if self.bg is not None:
            bits.append(f"bg={self.bg}")
        if self.bold:
            bits.append("bold")
        if self.reverse:
            bits.append("reverse")
        if self.underline:
            bits.append("underline")
        return "Attr(" + ",".join(bits) + ")"


_ATTRS: dict[tuple, Attr] = {}


def attr_of(fg=None, bg=None, bold=False, reverse=False,
            underline=False) -> Attr:
    """The one `Attr` carrying this style; cells share it."""
    key = (fg, bg, bold, reverse, underline)
    got = _ATTRS.get(key)
    if got is None:
        got = _ATTRS[key] = Attr(*key)
    return got


DEFAULT_ATTR = attr_of()

BLANK = " "

# Placeholder occupying the right half of a double-width glyph.
CONT = "\uffff"

# The ground state consumes printable ASCII a run at a time; this finds where
# the run ends, which is the next control byte, ESC, or non-ASCII lead.
_NOT_PLAIN = re.compile(rb"[^\x20-\x7e]")

# A whole CSI, taken in one match instead of a byte at a time. The byte ranges
# are the state machine's: parameters, then intermediates, then the final. A
# sequence split across two reads does not match and falls back to it.
_CSI = re.compile(rb"\x1b\[([\x30-\x3f]*)([\x20-\x2f]*)([\x40-\x7e])")

# Likewise for a string sequence's terminator: BEL or ST. OSC 52 carries the
# whole clipboard, so the payload is worth taking whole.
_STR_END = re.compile(rb"\x07|\x1b\\")

# The test build's idle beacon, as an APC payload. See `idle_beacon` in
# src/tui.c: the agent writes one every time it blocks for input with a
# painted frame behind it, carrying a sequence number and the number of
# input bytes it has consumed by then.
IDLE_PREFIX = "agent;idle;"


class Buffer:
    def __init__(self, cols: int, rows: int):
        self.cols = cols
        self.rows = rows
        self.chars = [[BLANK] * cols for _ in range(rows)]
        self.attrs = [[DEFAULT_ATTR] * cols for _ in range(rows)]

    def clear(self):
        cols = self.cols
        for r in range(self.rows):
            self.chars[r][:] = [BLANK] * cols
            self.attrs[r][:] = [DEFAULT_ATTR] * cols

    def resize(self, cols: int, rows: int):
        old_chars, old_attrs = self.chars, self.attrs
        old_rows, old_cols = self.rows, self.cols
        self.cols, self.rows = cols, rows
        self.chars = [[BLANK] * cols for _ in range(rows)]
        self.attrs = [[DEFAULT_ATTR] * cols for _ in range(rows)]
        keep = min(cols, old_cols)
        for r in range(min(rows, old_rows)):
            self.chars[r][:keep] = old_chars[r][:keep]
            self.attrs[r][:keep] = old_attrs[r][:keep]


class Terminal:
    """Feed it pty bytes, ask it what the screen looks like."""

    def __init__(self, cols: int = 80, rows: int = 24):
        self.cols = cols
        self.rows = rows
        self.primary = Buffer(cols, rows)
        self.alt = Buffer(cols, rows)
        self.buf = self.primary
        self.alt_active = False
        self.row = 0
        self.col = 0
        self.saved_cursor = (0, 0)
        self.attr = DEFAULT_ATTR
        self.cursor_visible = True
        self.autowrap = True
        self.modes: dict[int, bool] = {}
        self.clipboard: str | None = None
        self.clipboard_writes: list[str] = []
        self.notifications: list[str] = []
        self.bell_count = 0
        self.title: str | None = None
        self.unknown: list[str] = []
        # Sequence number of the last idle beacon the test build emitted. It
        # only ever grows, and each bump is one settled frame.
        self.idle_seq = 0
        # Input bytes the child had consumed when it last parked.
        self.idle_consumed = 0
        # (cols, rows) the frame behind that park was painted for, which is
        # what says a resize has been taken up. None until a beacon says so.
        self.idle_size: tuple[int, int] | None = None
        # decoder state
        self._pending = b""
        self._state = "ground"
        self._params = ""
        self._intermediate = ""
        self._osc = ""
        # Rendered rows, valid until the next feed. `wait_for` polls the
        # screen far more often than bytes arrive, and every poll used to
        # rebuild all of it.
        self._lines: list[str] | None = None

    # ---- geometry ---------------------------------------------------------
    def resize(self, cols: int, rows: int):
        self.cols, self.rows = cols, rows
        self.primary.resize(cols, rows)
        self.alt.resize(cols, rows)
        self.row = min(self.row, rows - 1)
        self.col = min(self.col, cols - 1)
        self._lines = None

    # ---- feeding ----------------------------------------------------------
    def feed(self, data: bytes):
        # Only a feed can move the screen, so one invalidation here covers
        # every mutation below.
        self._lines = None
        data = self._pending + data
        self._pending = b""
        i = 0
        n = len(data)
        while i < n:
            b = data[i]
            if self._state == "ground":
                if b == 0x1B:
                    m = _CSI.match(data, i)
                    if m is not None:
                        self._params = m.group(1).decode("ascii")
                        self._intermediate = m.group(2).decode("ascii")
                        self._csi(m.group(3).decode("ascii"))
                        i = m.end()
                        continue
                    self._state = "esc"
                    i += 1
                    continue
                if b < 0x80:
                    # A frame is mostly runs of plain text between escapes;
                    # take the whole run rather than dispatching per byte.
                    if 0x20 <= b <= 0x7E:
                        m = _NOT_PLAIN.search(data, i + 1)
                        end = m.start() if m else n
                        self._print_run(data[i:end].decode("ascii"))
                        i = end
                        continue
                    self._control_or_print(chr(b))
                    i += 1
                    continue
                # UTF-8 sequence; hold back an incomplete tail for next feed
                length = _utf8_len(b)
                if length == 0:
                    i += 1  # invalid lead byte: drop it
                    continue
                if i + length > n:
                    self._pending = data[i:]
                    return
                try:
                    ch = data[i : i + length].decode("utf-8")
                except UnicodeDecodeError:
                    i += 1
                    continue
                self._print(ch)
                i += length
                continue

            ch = chr(b)
            if self._state == "esc":
                i += 1
                if ch == "[":
                    self._state = "csi"
                    self._params = ""
                    self._intermediate = ""
                elif ch == "]":
                    self._state = "osc"
                    self._osc = ""
                elif ch in "PX^_":
                    self._state = "dcs"
                    self._osc = ""
                elif ch == "7":
                    self.saved_cursor = (self.row, self.col)
                    self._state = "ground"
                elif ch == "8":
                    self.row, self.col = self.saved_cursor
                    self._state = "ground"
                elif ch == "c":
                    self._reset()
                    self._state = "ground"
                elif ch in "()*+":
                    self._state = "charset"
                else:
                    self.unknown.append("ESC " + ch)
                    self._state = "ground"
                continue

            if self._state == "charset":
                i += 1
                self._state = "ground"
                continue

            if self._state == "csi":
                i += 1
                if 0x30 <= b <= 0x3F:
                    self._params += ch
                elif 0x20 <= b <= 0x2F:
                    self._intermediate += ch
                else:
                    self._csi(ch)
                    self._state = "ground"
                continue

            if self._state in ("osc", "dcs"):
                # OSC 52 carries a whole selection, so the payload is taken in
                # one slice up to its terminator. A bare ESC at the end of the
                # read may be the head of an ST split across two feeds, so it
                # is left for the byte-at-a-time path below.
                m = _STR_END.search(data, i)
                if m is not None:
                    self._osc += data[i:m.start()].decode("latin-1")
                    self._end_string()
                    i = m.end()
                    continue
                tail = n - 1 if data[n - 1] == 0x1B else n
                if tail > i:
                    self._osc += data[i:tail].decode("latin-1")
                    i = tail
                    continue
                i += 1
                if b == 0x1B:
                    self._state = self._state + "_esc"
                else:
                    self._osc += ch
                continue

            if self._state in ("osc_esc", "dcs_esc"):
                i += 1
                if ch == "\\":
                    self._state = self._state[:-4]
                    self._end_string()
                else:
                    self._osc += "\x1b" + ch
                    self._state = self._state[:-4]
                continue

            i += 1  # unreachable, but never spin

    # ---- primitives -------------------------------------------------------
    def _reset(self):
        self.primary.clear()
        self.alt.clear()
        self.buf = self.primary
        self.alt_active = False
        self.row = self.col = 0
        self.attr = DEFAULT_ATTR

    def _control_or_print(self, ch: str):
        if ch == "\n":
            self.row += 1
            if self.row >= self.rows:
                self._scroll_up()
                self.row = self.rows - 1
        elif ch == "\r":
            self.col = 0
        elif ch == "\b":
            self.col = max(0, self.col - 1)
        elif ch == "\t":
            self.col = min(self.cols - 1, (self.col // 8 + 1) * 8)
        elif ch == "\x07":
            self.bell_count += 1
        elif ch == "\x0f" or ch == "\x0e":
            pass
        elif ord(ch) < 0x20:
            pass
        else:
            self._print(ch)

    def _scroll_up(self):
        b = self.buf
        b.chars.pop(0)
        b.attrs.pop(0)
        b.chars.append([BLANK] * b.cols)
        b.attrs.append([DEFAULT_ATTR] * b.cols)

    def _print_run(self, s: str):
        """Paint a run of printable ASCII, which is every glyph one cell wide.

        Same rules as `_print`, applied to as much of the run as fits the
        current row at a time. The UI paints with DECAWM off, so that path is
        the one that matters: everything past the right edge lands on the last
        cell, where only the final glyph of the run survives.
        """
        b = self.buf
        cols = self.cols
        if cols <= 0:
            return
        i, n = 0, len(s)
        while i < n:
            if self.row >= self.rows:
                self.row = self.rows - 1
            if self.col + 1 > cols:
                if self.autowrap:
                    self.col = 0
                    self.row += 1
                    if self.row >= self.rows:
                        self._scroll_up()
                        self.row = self.rows - 1
                else:
                    # Every remaining glyph overwrites the last cell in turn,
                    # so only the last one is observable.
                    b.chars[self.row][cols - 1] = s[-1]
                    b.attrs[self.row][cols - 1] = self.attr
                    self.col = cols
                    return
            take = min(cols - self.col, n - i)
            end = self.col + take
            b.chars[self.row][self.col:end] = s[i:i + take]
            b.attrs[self.row][self.col:end] = [self.attr] * take
            self.col = end
            i += take

    def _print(self, ch: str):
        w = char_width(ch)
        if w == 0:
            return  # combining marks are not tracked separately
        if self.col + w > self.cols:
            if self.autowrap:
                self.col = 0
                self.row += 1
                if self.row >= self.rows:
                    self._scroll_up()
                    self.row = self.rows - 1
            else:
                # DECAWM off: the glyph overwrites the last cell(s)
                self.col = max(0, self.cols - w)
        if self.row >= self.rows:
            self.row = self.rows - 1
        b = self.buf
        b.chars[self.row][self.col] = ch
        b.attrs[self.row][self.col] = self.attr
        for k in range(1, w):
            if self.col + k < self.cols:
                b.chars[self.row][self.col + k] = CONT
                b.attrs[self.row][self.col + k] = self.attr
        self.col += w
        if self.col > self.cols:
            self.col = self.cols

    # ---- CSI --------------------------------------------------------------
    def _nums(self, default=0):
        raw = self._params.lstrip("?<>=")
        if raw == "":
            return []
        out = []
        for part in raw.split(";"):
            try:
                out.append(int(part) if part else default)
            except ValueError:
                out.append(default)
        return out

    def _csi(self, final: str):
        private = self._params.startswith("?")
        params = self._nums()

        def p(i, default=1):
            return params[i] if len(params) > i and params[i] != 0 else default

        if private and final in "hl":
            on = final == "h"
            for m in params:
                self.modes[m] = on
                if m == 1049:
                    self._switch_alt(on)
                elif m == 25:
                    self.cursor_visible = on
                elif m == 7:
                    self.autowrap = on
            return
        if final in ("H", "f"):
            self.row = min(self.rows - 1, max(0, p(0) - 1))
            self.col = min(self.cols - 1, max(0, p(1) - 1))
        elif final == "A":
            self.row = max(0, self.row - p(0))
        elif final == "B":
            self.row = min(self.rows - 1, self.row + p(0))
        elif final == "C":
            self.col = min(self.cols - 1, self.col + p(0))
        elif final == "D":
            self.col = max(0, self.col - p(0))
        elif final == "G":
            self.col = min(self.cols - 1, max(0, p(0) - 1))
        elif final == "d":
            self.row = min(self.rows - 1, max(0, p(0) - 1))
        elif final == "J":
            self._erase_display(params[0] if params else 0)
        elif final == "K":
            self._erase_line(params[0] if params else 0)
        elif final == "m":
            self._sgr(params)
        elif final in ("s",):
            self.saved_cursor = (self.row, self.col)
        elif final in ("u",):
            self.row, self.col = self.saved_cursor
        elif final in ("M", "m"):
            pass
        else:
            self.unknown.append(f"CSI {self._params}{final}")

    def _switch_alt(self, on: bool):
        if on and not self.alt_active:
            self.saved_cursor = (self.row, self.col)
            self.alt.clear()
            self.buf = self.alt
            self.alt_active = True
            self.row = self.col = 0
        elif not on and self.alt_active:
            self.buf = self.primary
            self.alt_active = False
            self.row, self.col = self.saved_cursor

    def _erase_display(self, mode: int):
        b = self.buf
        if mode == 2 or mode == 3:
            b.clear()
            return
        if mode == 0:
            self._erase_line(0)
            rng = range(self.row + 1, self.rows)
        else:
            self._erase_line(1)
            rng = range(0, self.row)
        blank = [BLANK] * self.cols
        plain = [DEFAULT_ATTR] * self.cols
        for r in rng:
            b.chars[r][:] = blank
            b.attrs[r][:] = plain

    def _erase_line(self, mode: int):
        b = self.buf
        if mode == 0:
            lo, hi = self.col, self.cols
        elif mode == 1:
            lo, hi = 0, min(self.col + 1, self.cols)
        else:
            lo, hi = 0, self.cols
        if hi <= lo:
            return
        b.chars[self.row][lo:hi] = [BLANK] * (hi - lo)
        b.attrs[self.row][lo:hi] = [DEFAULT_ATTR] * (hi - lo)

    def _sgr(self, params):
        if not params:
            params = [0]
        fg, bg, bold, reverse, underline = self.attr.key()
        i = 0
        while i < len(params):
            v = params[i]
            if v == 0:
                fg = bg = None
                bold = reverse = underline = False
            elif v == 1:
                bold = True
            elif v == 22:
                bold = False
            elif v == 4:
                underline = True
            elif v == 24:
                underline = False
            elif v == 7:
                reverse = True
            elif v == 27:
                reverse = False
            elif v == 39:
                fg = None
            elif v == 49:
                bg = None
            elif 30 <= v <= 37:
                fg = v - 30
            elif 90 <= v <= 97:
                fg = v - 90 + 8
            elif 40 <= v <= 47:
                bg = v - 40
            elif 100 <= v <= 107:
                bg = v - 100 + 8
            elif v in (38, 48):
                if i + 1 < len(params) and params[i + 1] == 5:
                    colour = params[i + 2] if i + 2 < len(params) else 0
                    if v == 38:
                        fg = colour
                    else:
                        bg = colour
                    i += 2
                elif i + 1 < len(params) and params[i + 1] == 2:
                    i += 4
            i += 1
        self.attr = attr_of(fg, bg, bold, reverse, underline)

    # ---- OSC --------------------------------------------------------------
    def _end_string(self):
        payload = self._osc
        self._osc = ""
        self._state = "ground"
        if payload.startswith(IDLE_PREFIX):
            try:
                fields = payload[len(IDLE_PREFIX):].split(";")
                seq = fields[0]
                consumed = fields[1] if len(fields) > 1 else ""
                size = fields[2] if len(fields) > 2 else ""
                self.idle_seq = int(seq)
                self.idle_consumed = int(consumed) if consumed else 0
                if size:
                    cols, _, rows = size.partition("x")
                    self.idle_size = (int(cols), int(rows))
            except ValueError:
                pass
            return
        if payload.startswith("52;"):
            parts = payload.split(";", 2)
            if len(parts) == 3:
                try:
                    text = base64.b64decode(parts[2] + "==").decode(
                        "utf-8", "replace"
                    )
                except Exception:
                    text = ""
                self.clipboard = text
                self.clipboard_writes.append(text)
        elif payload.startswith("9;"):
            self.notifications.append(payload[2:])
        elif payload.startswith(("0;", "2;")):
            self.title = payload.split(";", 1)[1]

    # ---- inspection -------------------------------------------------------
    def row_text(self, r: int) -> str:
        line = "".join(self.buf.chars[r])
        return line.replace(CONT, "") if CONT in line else line

    def lines(self, strip=True) -> list[str]:
        rows = self._lines
        if rows is None:
            rows = self._lines = [self.row_text(r) for r in range(self.rows)]
        if not strip:
            return list(rows)
        return [line.rstrip() for line in rows]

    def text(self, strip=True) -> str:
        return "\n".join(self.lines(strip))

    def find_row(self, needle: str) -> int:
        for r, line in enumerate(self.lines()):
            if needle in line:
                return r
        return -1

    def contains(self, needle: str) -> bool:
        return self.find_row(needle) >= 0

    def attr_at(self, row: int, col: int) -> Attr:
        return self.buf.attrs[row][col]

    def snapshot(self, label: str = "") -> str:
        """Stable textual dump used for golden comparisons."""
        head = f"--- screen {self.cols}x{self.rows}"
        if label:
            head += f" {label}"
        head += " ---"
        body = []
        for line in self.lines():
            body.append("|" + line)
        tail = (
            f"--- cursor row={self.row + 1} col={self.col + 1} "
            f"visible={'yes' if self.cursor_visible else 'no'} ---"
        )
        return "\n".join([head] + body + [tail]) + "\n"


def _utf8_len(lead: int) -> int:
    if lead < 0x80:
        return 1
    if 0xC2 <= lead <= 0xDF:
        return 2
    if 0xE0 <= lead <= 0xEF:
        return 3
    if 0xF0 <= lead <= 0xF4:
        return 4
    return 0
