"""Images drawn in the transcript with the kitty graphics protocol.

kitty and Ghostty get PNG images as Unicode placeholders: the image is sent
once with `a=T,U=1` and drawn by U+10EEEE cells whose foreground color carries
its id. Every other terminal, and every other format, keeps the text line
alone. The pty reports no pixel size, so a cell counts as 10 by 20 pixels.

Inside tmux the commands go out through its passthrough, once tmux says it
allows passthrough, has 24-bit color and runs in kitty or Ghostty.
"""

import base64
import json
import re
import struct
import zlib

from .test_editor import editors, spawn as spawn_with_editor
from .test_image import attach, png

PLACEHOLDER = "\U0010EEEE"
_APC = re.compile(rb"\x1b_G([^;\x1b]*)(?:;([^\x1b]*))?\x1b\\")
_TMUX = re.compile(rb"\x1bPtmux;((?:[^\x1b]|\x1b\x1b)*)\x1b\\")
TMUX = "/tmp/tmux-1000/default,4242,0"
TMUX_GHOSTTY = "xterm-ghostty\nghostty 1.3.1\nall\n256,RGB,sync\n"

NOT_PNG = [
    ("jpeg", bytes.fromhex("ffd8ffc00011080003000403012200021101031100ffd9")),
    ("gif", b"GIF89a\x04\x00\x03\x00" + bytes(20)),
    ("webp", b"RIFF" + bytes(4) + b"WEBPVP8X" + bytes(8)
     + b"\x03\x00\x00\x02\x00\x00"),
]


def stored_png(w: int, h: int) -> bytes:
    """A PNG whose bytes do not depend on the zlib build.

    zlib and zlib-ng compress the same pixels to different bytes, and a golden
    screen shows the file size and the context estimate drawn from it. Stored
    deflate blocks are the same everywhere."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + b"\xff\x00\x00" * w for _ in range(h))
    blocks = [raw[i:i + 0xFFFF] for i in range(0, len(raw), 0xFFFF)] or [b""]
    deflate = b"\x78\x01" + b"".join(
        bytes([i == len(blocks) - 1])
        + struct.pack("<HH", len(b), len(b) ^ 0xFFFF) + b
        for i, b in enumerate(blocks)
    ) + struct.pack(">I", zlib.adler32(raw))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", deflate)
            + chunk(b"IEND", b""))


def passed_through(s) -> bytes:
    """What the child handed to tmux to pass on, unwrapped."""
    return b"".join(m.group(1).replace(b"\x1b\x1b", b"\x1b")
                    for m in _TMUX.finditer(bytes(s.raw)))


def graphics(s, tmux: bool = False) -> list[tuple[dict, bytes]]:
    """Each kitty graphics command the child wrote, as (keys, payload).

    With `tmux`, the commands it wrote inside tmux passthrough."""
    out = []
    for m in _APC.finditer(passed_through(s) if tmux else bytes(s.raw)):
        keys = {}
        for pair in m.group(1).split(b","):
            if b"=" in pair:
                k, v = pair.split(b"=", 1)
                keys[k.decode()] = v.decode()
        out.append((keys, m.group(2) or b""))
    return out


def transmitted(s, tmux: bool = False) -> list[tuple[dict, bytes]]:
    """The images sent, as (keys of the first chunk, decoded bytes)."""
    images, current = [], None
    for keys, payload in graphics(s, tmux):
        if keys.get("a") == "T":
            current = [keys, bytearray()]
            images.append(current)
        elif current is None or set(keys) != {"m"}:
            continue
        current[1] += payload
        if keys.get("m", "0") == "0":
            current = None
    return [(k, base64.b64decode(bytes(b))) for k, b in images]


def placements(s) -> list[dict]:
    return [keys for keys, _ in graphics(s) if keys.get("a") == "p"]


def image_rows(s, cols: int) -> int:
    """Screen rows holding a run of `cols` placeholders."""
    return s.text().count(PLACEHOLDER * cols)


def fake_tmux(ctx, reply: str, exit_code: int = 0) -> dict:
    """Env that puts a stub `tmux` first on PATH and points TMUX at a server.

    The stub appends its arguments to `tmux.args` under the work directory,
    prints `reply` and exits with `exit_code`. It keeps a case from reaching
    the developer's own tmux server."""
    bindir = ctx.work / "tmuxbin"
    bindir.mkdir(exist_ok=True)
    (ctx.work / "tmux.reply").write_text(reply)
    script = bindir / "tmux"
    script.write_text(
        "#!/bin/sh\n"
        "PATH=/usr/bin:/bin\n"
        f'printf "%s\\n" "$@" >> "{ctx.work}/tmux.args"\n'
        f'cat "{ctx.work}/tmux.reply"\n'
        f"exit {exit_code}\n"
    )
    script.chmod(0o755)
    return {"TMUX": TMUX, "TERM": "tmux-256color", "KITTY_WINDOW_ID": "1",
            "PATH": f"{bindir}:/usr/bin:/bin"}


def send_attached(ctx, data: bytes, **env):
    (ctx.work / "shot.png").write_bytes(data)
    ctx.scenario("text=a+red+rectangle")
    s = ctx.spawn(**env)
    attach(s, "shot.png")
    s.submit("what is this")
    s.wait_text("a red rectangle")
    s.wait_turn_done()
    return s


def read_file(ctx, name: str, data: bytes, **env):
    (ctx.work / name).write_bytes(data)
    ctx.scenario("tool=read:" + json.dumps({"path": name})
                 + ",final_text=inspected")
    s = ctx.spawn(**env)
    s.submit("look at it")
    s.wait_text("inspected")
    s.wait_turn_done()
    return s


# ---- drawn ----------------------------------------------------------------
def test_an_attached_png_is_drawn_in_kitty(ctx):
    """The image goes over once, whole, quietly, with a virtual placement.

    The drawn image takes the place of the caption: the message already names
    it, so a second `[Image #1]` line would only repeat it."""
    data = stored_png(200, 100)
    s = send_attached(ctx, data, TERM="xterm-kitty")
    [(keys, body)] = transmitted(s)
    assert body == data
    assert keys["U"] == "1" and keys["f"] == "100" and keys["q"] == "2", keys
    assert (keys["c"], keys["r"]) == ("20", "5"), keys
    assert image_rows(s, 20) == 5, s.text()
    assert "[Image #1] what is this" in s.text(), s.text()
    assert "shot.png - png 200x100" not in s.text(), s.text()
    ctx.check_screen(s)


def test_a_jpeg_attachment_keeps_its_caption(ctx):
    """In kitty, a JPEG attachment is still named, since nothing is drawn."""
    jpeg = dict(NOT_PNG)["jpeg"]
    (ctx.work / "shot.jpg").write_bytes(jpeg)
    ctx.scenario("text=seen")
    s = ctx.spawn(TERM="xterm-kitty")
    attach(s, "shot.jpg")
    s.submit("what is this")
    s.wait_text("seen")
    s.wait_turn_done()
    assert graphics(s) == []
    assert "[Image #1] shot.jpg - jpeg 4x3" in s.text(), s.text()


def test_a_png_the_model_reads_is_drawn_in_ghostty(ctx):
    """A read that loads an image draws it below the result."""
    data = stored_png(200, 100)
    s = read_file(ctx, "chart.png", data, TERM="xterm-ghostty")
    [(keys, body)] = transmitted(s)
    assert body == data
    assert image_rows(s, 20) == 5, s.text()
    ctx.check_screen(s)


def test_ghostty_is_known_by_term_program(ctx):
    """TERM is often left as xterm-256color; TERM_PROGRAM still says ghostty."""
    s = send_attached(ctx, png(200, 100), TERM_PROGRAM="ghostty")
    assert len(transmitted(s)) == 1, graphics(s)


def test_a_large_png_is_sent_in_chunks(ctx):
    """Every chunk but the last says more follows and holds 4096 bytes."""
    data = png(600, 400) + bytes(20000)
    s = send_attached(ctx, data, TERM="xterm-kitty")
    [(_, body)] = transmitted(s)
    assert body == data
    chunks = [(k, p) for k, p in graphics(s) if k.get("a") == "T" or set(k) == {"m"}]
    assert len(chunks) > 1, chunks
    assert all(k["m"] == "1" and len(p) == 4096 for k, p in chunks[:-1])
    assert chunks[-1][0]["m"] == "0"


def test_a_tall_image_is_held_to_half_the_screen(ctx):
    """A portrait screenshot does not push the conversation off the screen."""
    s = send_attached(ctx, png(100, 2000), TERM="xterm-kitty")
    [(keys, _)] = transmitted(s)
    assert keys["r"] == "12", keys
    assert keys["c"] == "2", keys


def test_a_width_change_places_the_image_again_without_sending_it(ctx):
    """A narrower transcript shrinks the image; the bytes stay sent."""
    s = send_attached(ctx, png(600, 100), TERM="xterm-kitty")
    [(keys, _)] = transmitted(s)
    assert (keys["c"], keys["r"]) == ("60", "5"), keys
    s.resize(50, 24)
    s.wait_for(lambda t: image_rows(s, 46) == 4, "the narrower image")
    assert len(transmitted(s)) == 1, graphics(s)
    assert (placements(s)[-1]["c"], placements(s)[-1]["r"]) == ("46", "4")


def test_the_editor_hands_back_a_screen_with_the_image_sent_again(ctx):
    """Leaving the screen deletes what was sent; coming back sends it again."""
    data = png(200, 100)
    (ctx.work / "shot.png").write_bytes(data)
    ctx.scenario("text=a+red+rectangle")
    bin_dir = editors(ctx, "vim")
    s = spawn_with_editor(ctx, bin_dir, "write:edited", EDITOR=bin_dir / "vim",
                          TERM="xterm-kitty")
    attach(s, "shot.png")
    s.submit("what is this")
    s.wait_text("a red rectangle")
    s.wait_turn_done()
    s.key("ctrl-g").sync()
    s.wait_for(lambda t: s.composer_text() == "edited", "the saved text")
    s.wait_for(lambda t: len(transmitted(s)) == 2, "the image sent again")
    deletes = [k for k, _ in graphics(s) if k.get("a") == "d"]
    assert deletes and deletes[0]["d"] == "I", deletes
    assert all(body == data for _, body in transmitted(s))


def test_exit_deletes_the_images_it_sent(ctx):
    """The terminal keeps no image memory once the agent is gone."""
    s = send_attached(ctx, png(200, 100), TERM="xterm-kitty")
    [(keys, _)] = transmitted(s)
    s.submit("/exit")
    s.wait_exit()
    deletes = [k for k, _ in graphics(s) if k.get("a") == "d"]
    assert any(k["d"] == "I" and k["i"] == keys["i"] for k in deletes), deletes


def test_a_resumed_round_draws_each_image_under_its_call(ctx):
    """Two reads in one round replay with each image below its own call,
    though the session saves both calls before either result."""
    (ctx.work / "one.png").write_bytes(png(200, 100))
    (ctx.work / "two.png").write_bytes(png(300, 100))
    ctx.scenario('tool=read:{"path":"one.png"},tool=read:{"path":"two.png"},'
                 "final_text=seen+both")
    s = ctx.spawn(rows=50, TERM="xterm-kitty")
    s.submit("look at them")
    s.wait_text("seen both")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    s = ctx.spawn(rows=50, TERM="xterm-kitty")
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("seen both")
    lines = s.text().splitlines()
    one = next(i for i, line in enumerate(lines) if "read one.png" in line)
    two = next(i for i, line in enumerate(lines) if "read two.png" in line)
    narrow = [i for i, line in enumerate(lines)
              if PLACEHOLDER * 20 in line and PLACEHOLDER * 21 not in line]
    wide = [i for i, line in enumerate(lines) if PLACEHOLDER * 30 in line]
    assert len(narrow) == 5 and len(wide) == 5, s.text()
    assert one < narrow[0] and narrow[-1] < two < wide[0], s.text()


# ---- tmux -----------------------------------------------------------------
def test_tmux_passes_the_image_on_to_ghostty(ctx):
    """Every graphics command is wrapped; none reaches tmux bare."""
    data = stored_png(200, 100)
    s = send_attached(ctx, data, **fake_tmux(ctx, TMUX_GHOSTTY))
    assert graphics(s) == [], graphics(s)
    [(keys, body)] = transmitted(s, tmux=True)
    assert body == data
    assert (keys["c"], keys["r"]) == ("20", "5"), keys
    assert image_rows(s, 20) == 5, s.text()
    assert "display-message" in (ctx.work / "tmux.args").read_text()
    s.submit("/exit")
    s.wait_exit()
    assert graphics(s) == [], graphics(s)
    deletes = [k for k, _ in graphics(s, tmux=True) if k.get("a") == "d"]
    assert any(k["i"] == keys["i"] for k in deletes), deletes


def test_tmux_knows_kitty_by_its_version_reply(ctx):
    """The client TERM may be generic; tmux still learned what answered."""
    reply = "xterm-256color\nkitty(0.42.1)\non\nRGB\n"
    s = send_attached(ctx, png(200, 100), **fake_tmux(ctx, reply))
    assert len(transmitted(s, tmux=True)) == 1, passed_through(s)


def test_tmux_is_not_asked_until_there_is_an_image(ctx):
    """Startup and plain turns run no tmux command."""
    ctx.scenario("text=plain+answer")
    s = ctx.spawn(**fake_tmux(ctx, TMUX_GHOSTTY))
    s.submit("hello")
    s.wait_text("plain answer")
    s.wait_turn_done()
    assert not (ctx.work / "tmux.args").exists()


# ---- not drawn ------------------------------------------------------------
def test_other_terminals_keep_the_text_line(ctx):
    """No protocol support assumed: no escape, no placeholder."""
    s = send_attached(ctx, png(200, 100))
    assert graphics(s) == []
    assert PLACEHOLDER not in s.text()
    assert "[Image #1] shot.png - png 200x100" in s.text(), s.text()


def test_tmux_that_cannot_pass_images_keeps_the_text_line(ctx):
    """Passthrough off, a client without images, no 24-bit color to carry
    the image id, or a tmux that fails: nothing is drawn."""
    cases = [
        ("xterm-ghostty\nghostty 1.3.1\noff\nRGB\n", 0),
        ("xterm-256color\nfoot(1.20.0)\nall\nRGB\n", 0),
        ("xterm-ghostty\nghostty 1.3.1\nall\n256,sync\n", 0),
        (TMUX_GHOSTTY, 1),
    ]
    for reply, code in cases:
        s = send_attached(ctx, png(200, 100),
                          **fake_tmux(ctx, reply, exit_code=code))
        assert graphics(s) == [], reply
        assert graphics(s, tmux=True) == [], reply
        assert PLACEHOLDER not in s.text(), reply
        assert "[Image #1] shot.png - png 200x100" in s.text(), s.text()
        s.close()


def test_formats_other_than_png_keep_the_text_line(ctx):
    """The terminal decodes only PNG, and nothing here decodes pixels."""
    for kind, data in NOT_PNG:
        s = read_file(ctx, f"pic.{kind}", data, TERM="xterm-kitty")
        assert graphics(s) == [], kind
        assert PLACEHOLDER not in s.text(), kind
        s.close()
