"""Images drawn in the transcript with the kitty graphics protocol.

kitty and Ghostty get PNG images as Unicode placeholders: the image is sent
once with `a=T,U=1` and drawn by U+10EEEE cells whose foreground color carries
its id. Every other terminal, and every other format, keeps the text line
alone. The pty reports no pixel size, so a cell counts as 10 by 20 pixels.
"""

import base64
import json
import re

from .test_editor import editors, spawn as spawn_with_editor
from .test_image import attach, png

PLACEHOLDER = "\U0010EEEE"
_APC = re.compile(rb"\x1b_G([^;\x1b]*)(?:;([^\x1b]*))?\x1b\\")

NOT_PNG = [
    ("jpeg", bytes.fromhex("ffd8ffc00011080003000403012200021101031100ffd9")),
    ("gif", b"GIF89a\x04\x00\x03\x00" + bytes(20)),
    ("webp", b"RIFF" + bytes(4) + b"WEBPVP8X" + bytes(8)
     + b"\x03\x00\x00\x02\x00\x00"),
]


def graphics(s) -> list[tuple[dict, bytes]]:
    """Each kitty graphics command the child wrote, as (keys, payload)."""
    out = []
    for m in _APC.finditer(bytes(s.raw)):
        keys = {}
        for pair in m.group(1).split(b","):
            if b"=" in pair:
                k, v = pair.split(b"=", 1)
                keys[k.decode()] = v.decode()
        out.append((keys, m.group(2) or b""))
    return out


def transmitted(s) -> list[tuple[dict, bytes]]:
    """The images sent, as (keys of the first chunk, decoded bytes)."""
    images, current = [], None
    for keys, payload in graphics(s):
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
    data = png(200, 100)
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
    data = png(200, 100)
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


# ---- not drawn ------------------------------------------------------------
def test_other_terminals_keep_the_text_line(ctx):
    """No protocol support assumed: no escape, no placeholder."""
    s = send_attached(ctx, png(200, 100))
    assert graphics(s) == []
    assert PLACEHOLDER not in s.text()
    assert "[Image #1] shot.png - png 200x100" in s.text(), s.text()


def test_tmux_keeps_the_text_line(ctx):
    """tmux drops the graphics commands, so nothing is drawn inside it."""
    s = send_attached(ctx, png(200, 100), TERM="tmux-256color",
                      KITTY_WINDOW_ID="1", TMUX="/tmp/tmux-1000/default,1,0")
    assert graphics(s) == []
    assert PLACEHOLDER not in s.text()


def test_formats_other_than_png_keep_the_text_line(ctx):
    """The terminal decodes only PNG, and nothing here decodes pixels."""
    for kind, data in NOT_PNG:
        s = read_file(ctx, f"pic.{kind}", data, TERM="xterm-kitty")
        assert graphics(s) == [], kind
        assert PLACEHOLDER not in s.text(), kind
        s.close()
