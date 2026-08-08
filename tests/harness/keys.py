"""Symbolic key names to the bytes an xterm would send.

Tests spell input as `session.key("ctrl-a", "end", "enter")` so the intent
survives a change of terminal encoding, and mouse reports are built from the
SGR (1006) form the UI enables at startup.
"""

from __future__ import annotations

ESC = "\x1b"

KEYS = {
    "enter": "\r",
    "return": "\r",
    "newline": ESC + "\r",      # alt-enter: insert a line break, do not submit
    "tab": "\t",
    "shift-tab": ESC + "[Z",
    "esc": ESC,
    "backspace": "\x7f",
    "bs": "\x7f",
    "delete": "\x04",           # the composer treats ^D as forward-delete
    "space": " ",
    "up": ESC + "[A",
    "down": ESC + "[B",
    "right": ESC + "[C",
    "left": ESC + "[D",
    # SS3, what a terminal left in application cursor key mode sends
    "ss3-up": ESC + "OA",
    "ss3-down": ESC + "OB",
    "ss3-right": ESC + "OC",
    "ss3-left": ESC + "OD",
    "home": ESC + "[H",
    "end": ESC + "[F",
    "home~": ESC + "[1~",
    "end~": ESC + "[4~",
    "pageup": ESC + "[5~",
    "pagedown": ESC + "[6~",
    "ctrl-left": ESC + "[1;5D",
    "ctrl-right": ESC + "[1;5C",
    "wheel-up": ESC + "[<64;1;1M",
    "wheel-down": ESC + "[<65;1;1M",
}


def encode(name: str) -> str:
    """One key name (or literal text) to the bytes a terminal would send."""
    low = name.lower()
    if low in KEYS:
        return KEYS[low]
    if low.startswith("ctrl-") and len(low) == 6:
        c = low[5]
        if "a" <= c <= "z":
            return chr(ord(c) - 96)
        if c == "[":
            return ESC
    if low.startswith("alt-") and len(low) == 5:
        return ESC + name[4]
    return name


def encode_all(*names: str) -> bytes:
    return "".join(encode(n) for n in names).encode("utf-8")


def mouse(event: str, row: int, col: int, button: int = 0) -> bytes:
    """An SGR (1006) mouse report; row/col are 1-based screen cells."""
    if event == "down":
        code, final = button, "M"
    elif event == "drag":
        code, final = button + 32, "M"
    elif event == "move":
        # motion with no button held: 3 (no button) plus the motion bit
        code, final = 35, "M"
    elif event == "up":
        code, final = button, "m"
    elif event == "wheel-up":
        code, final = 64, "M"
    elif event == "wheel-down":
        code, final = 65, "M"
    else:
        raise ValueError(f"unknown mouse event {event!r}")
    return f"{ESC}[<{code};{col};{row}{final}".encode()
