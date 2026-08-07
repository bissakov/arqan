"""Minimal xterm-compatible screen emulator.

Only the escape repertoire tui.c actually emits is implemented, which is
enough to turn the byte stream on a pty into a deterministic screen snapshot:
CUP, ED, EL, SGR, DEC private modes (alt screen / cursor / autowrap / mouse)
and OSC 52 clipboard writes.

Unknown sequences are consumed and ignored rather than printed, so a stray
escape shows up as missing output instead of corrupting every later cell.
"""

from __future__ import annotations

import base64
import unicodedata

# ---- character cell widths ------------------------------------------------
# Mirrors what wcwidth(3) reports for the glyphs the UI can paint, so the
# emulator's column accounting matches tui.c's.


def char_width(ch: str) -> int:
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


def text_width(s: str) -> int:
    return sum(char_width(c) for c in s)


class Attr:
    """Style flags of one cell, kept comparable and hashable."""

    __slots__ = ("fg", "bg", "bold", "reverse", "underline")

    def __init__(self, fg=None, bg=None, bold=False, reverse=False,
                 underline=False):
        self.fg = fg
        self.bg = bg
        self.bold = bold
        self.reverse = reverse
        self.underline = underline

    def copy(self) -> "Attr":
        return Attr(self.fg, self.bg, self.bold, self.reverse, self.underline)

    def key(self):
        return (self.fg, self.bg, self.bold, self.reverse, self.underline)

    def __eq__(self, other):
        return isinstance(other, Attr) and self.key() == other.key()

    def __hash__(self):
        return hash(self.key())

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


BLANK = " "

# Placeholder occupying the right half of a double-width glyph.
CONT = "\uffff"


class Buffer:
    def __init__(self, cols: int, rows: int):
        self.cols = cols
        self.rows = rows
        self.chars = [[BLANK] * cols for _ in range(rows)]
        self.attrs = [[Attr() for _ in range(cols)] for _ in range(rows)]

    def clear(self):
        for r in range(self.rows):
            for c in range(self.cols):
                self.chars[r][c] = BLANK
                self.attrs[r][c] = Attr()

    def resize(self, cols: int, rows: int):
        old_chars, old_attrs = self.chars, self.attrs
        old_rows, old_cols = self.rows, self.cols
        self.cols, self.rows = cols, rows
        self.chars = [[BLANK] * cols for _ in range(rows)]
        self.attrs = [[Attr() for _ in range(cols)] for _ in range(rows)]
        for r in range(min(rows, old_rows)):
            for c in range(min(cols, old_cols)):
                self.chars[r][c] = old_chars[r][c]
                self.attrs[r][c] = old_attrs[r][c]


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
        self.attr = Attr()
        self.cursor_visible = True
        self.autowrap = True
        self.modes: dict[int, bool] = {}
        self.clipboard: str | None = None
        self.clipboard_writes: list[str] = []
        self.bell_count = 0
        self.title: str | None = None
        self.unknown: list[str] = []
        # decoder state
        self._pending = b""
        self._state = "ground"
        self._params = ""
        self._intermediate = ""
        self._osc = ""

    # ---- geometry ---------------------------------------------------------
    def resize(self, cols: int, rows: int):
        self.cols, self.rows = cols, rows
        self.primary.resize(cols, rows)
        self.alt.resize(cols, rows)
        self.row = min(self.row, rows - 1)
        self.col = min(self.col, cols - 1)

    # ---- feeding ----------------------------------------------------------
    def feed(self, data: bytes):
        data = self._pending + data
        self._pending = b""
        i = 0
        n = len(data)
        while i < n:
            b = data[i]
            if self._state == "ground":
                if b == 0x1B:
                    self._state = "esc"
                    i += 1
                    continue
                if b < 0x80:
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
                i += 1
                if b == 0x07:  # BEL terminator
                    self._end_string()
                elif b == 0x1B:
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
        self.attr = Attr()

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
        b.attrs.append([Attr() for _ in range(b.cols)])

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
        b.attrs[self.row][self.col] = self.attr.copy()
        for k in range(1, w):
            if self.col + k < self.cols:
                b.chars[self.row][self.col + k] = CONT
                b.attrs[self.row][self.col + k] = self.attr.copy()
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
        for r in rng:
            for c in range(self.cols):
                b.chars[r][c] = BLANK
                b.attrs[r][c] = Attr()

    def _erase_line(self, mode: int):
        b = self.buf
        if mode == 0:
            rng = range(self.col, self.cols)
        elif mode == 1:
            rng = range(0, min(self.col + 1, self.cols))
        else:
            rng = range(0, self.cols)
        for c in rng:
            b.chars[self.row][c] = BLANK
            b.attrs[self.row][c] = self.attr.copy() if False else Attr()

    def _sgr(self, params):
        if not params:
            params = [0]
        i = 0
        while i < len(params):
            v = params[i]
            if v == 0:
                self.attr = Attr()
            elif v == 1:
                self.attr.bold = True
            elif v == 22:
                self.attr.bold = False
            elif v == 4:
                self.attr.underline = True
            elif v == 24:
                self.attr.underline = False
            elif v == 7:
                self.attr.reverse = True
            elif v == 27:
                self.attr.reverse = False
            elif v == 39:
                self.attr.fg = None
            elif v == 49:
                self.attr.bg = None
            elif 30 <= v <= 37:
                self.attr.fg = v - 30
            elif 90 <= v <= 97:
                self.attr.fg = v - 90 + 8
            elif 40 <= v <= 47:
                self.attr.bg = v - 40
            elif 100 <= v <= 107:
                self.attr.bg = v - 100 + 8
            elif v in (38, 48):
                if i + 1 < len(params) and params[i + 1] == 5:
                    colour = params[i + 2] if i + 2 < len(params) else 0
                    if v == 38:
                        self.attr.fg = colour
                    else:
                        self.attr.bg = colour
                    i += 2
                elif i + 1 < len(params) and params[i + 1] == 2:
                    i += 4
            i += 1

    # ---- OSC --------------------------------------------------------------
    def _end_string(self):
        payload = self._osc
        self._osc = ""
        self._state = "ground"
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
        elif payload.startswith(("0;", "2;")):
            self.title = payload.split(";", 1)[1]

    # ---- inspection -------------------------------------------------------
    def row_text(self, r: int) -> str:
        return "".join(c for c in self.buf.chars[r] if c != CONT)

    def lines(self, strip=True) -> list[str]:
        out = []
        for r in range(self.rows):
            line = self.row_text(r)
            out.append(line.rstrip() if strip else line)
        return out

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
