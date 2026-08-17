"""Images attached to a turn: the placeholder, the wire format and the sidecar.

An attachment is one entry in the media table and one `[Image #n]` in the
message, so these cases assert on the composer, on what went over the wire in
both request shapes, and on what a resumed session sends again.
"""

import base64
import json
import struct
import zlib

from .test_context import tokens


def png(w: int, h: int) -> bytes:
    """A real PNG of `w` by `h`, built from the standard library alone."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + b"\xff\x00\x00" * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


def shot(ctx, name: str = "shot.png", w: int = 4, h: int = 3) -> bytes:
    data = png(w, h)
    (ctx.work / name).write_bytes(data)
    return data


def attach(s, path: str, n: int = 1):
    """Run /attach and wait for it to answer.

    Not `submit`: the command hands the composer the placeholder back, so
    waiting for an empty composer would be waiting for a frame that is gone
    as soon as it is painted.
    """
    s.type(f"/attach {path}").sync()
    s.key("enter")
    return s.wait_text(f"attached [Image #{n}]")


def sessions_dir(ctx):
    root = ctx.home / ".local" / "share" / "arqan" / "sessions"
    dirs = [p for p in root.iterdir() if p.is_dir()] if root.exists() else []
    assert len(dirs) == 1, sorted(p.name for p in dirs)
    return dirs[0]


def wait_composer(s, want: str):
    """Wait for the composer to hold `want`.

    An answer and the composer it hands back are painted in whichever order
    the frames fall, so a case that has seen the notice has not necessarily
    seen the composer yet.
    """
    return s.wait_for(lambda t: s.composer_text() == want,
                      f"the composer to read {want!r}")


def openai_images(body) -> list[str]:
    """The data URLs of the last user message of a chat-completions body."""
    content = body["messages"][-1]["content"]
    if isinstance(content, str):
        return []
    return [p["image_url"]["url"] for p in content if p["type"] == "image_url"]


# ---- attaching ------------------------------------------------------------
def test_attach_puts_a_placeholder_in_the_composer(ctx):
    """/attach hands the composer back the marker the message is written around."""
    shot(ctx)
    s = ctx.spawn()
    attach(s, "shot.png")
    wait_composer(s, "[Image #1]")
    assert "png 4x3" in s.text(), s.text()


def test_a_missing_file_attaches_nothing(ctx):
    """A path that names no file is reported and leaves the composer alone."""
    s = ctx.spawn()
    s.type("/attach nope.png").sync()
    s.key("enter")
    s.wait_text("no such file")
    wait_composer(s, "")


def test_a_file_that_is_not_an_image_is_refused(ctx):
    """The header decides, not the extension."""
    ctx.write_file("fake.png", "this is text pretending to be a picture\n")
    s = ctx.spawn()
    s.type("/attach fake.png").sync()
    s.key("enter")
    s.wait_text("not a PNG, JPEG, GIF or WebP image")
    wait_composer(s, "")


def test_an_oversized_image_is_refused_with_its_limit(ctx):
    """Over the per-image cap is an answer naming the cap, never a resize."""
    big = png(8, 8) + b"\x00" * (6 << 20)
    (ctx.work / "big.png").write_bytes(big)
    s = ctx.spawn()
    s.type("/attach big.png").sync()
    s.key("enter")
    s.wait_text("the limit is 5.0 MB")
    wait_composer(s, "")


def test_a_turn_carries_at_most_four_images(ctx):
    """The fifth attachment is refused rather than quietly dropped later."""
    for i in range(5):
        shot(ctx, f"s{i}.png")
    s = ctx.spawn()
    for i in range(4):
        attach(s, f"s{i}.png", i + 1)
    s.type("/attach s4.png").sync()
    s.key("enter")
    s.wait_text("at most 4 images")
    wait_composer(s, "[Image #1] [Image #2] [Image #3] [Image #4]")


# ---- the wire -------------------------------------------------------------
def test_an_image_is_sent_as_a_data_url(ctx):
    """A chat-completions turn with an image is content blocks, base64 and all."""
    data = shot(ctx)
    ctx.scenario("text=a+red+rectangle")
    s = ctx.spawn()
    attach(s, "shot.png")
    s.submit("what is this")
    s.wait_text("a red rectangle")
    s.wait_turn_done()

    body = ctx.mock.requests[-1]
    content = body["messages"][-1]["content"]
    assert content[0] == {"type": "text", "text": "[Image #1] what is this"}, content
    urls = openai_images(body)
    assert len(urls) == 1, content
    head, _, b64 = urls[0].partition(",")
    assert head == "data:image/png;base64", urls[0]
    assert base64.b64decode(b64) == data, "the bytes must reach the model intact"


def test_a_turn_without_an_image_still_sends_a_plain_string(ctx):
    """Nothing attached is the shape every endpoint already accepted."""
    ctx.scenario("text=hi")
    s = ctx.spawn()
    s.submit("say hi")
    s.wait_text("hi")
    s.wait_turn_done()
    assert ctx.mock.requests[-1]["messages"][-1] == {
        "role": "user", "content": "say hi",
    }, ctx.mock.requests[-1]["messages"]


def test_anthropic_sends_an_image_block_before_the_text(ctx):
    """The Anthropic shape is an image block ahead of the question about it."""
    data = shot(ctx)
    ctx.scenario("text=a+red+rectangle")
    s = ctx.spawn(ARQAN_API="anthropic")
    attach(s, "shot.png")
    s.submit("what is this")
    s.wait_text("a red rectangle")
    s.wait_turn_done()

    blocks = ctx.mock.requests[-1]["messages"][-1]["content"]
    assert [b["type"] for b in blocks] == ["image", "text"], blocks
    assert blocks[0]["source"]["type"] == "base64", blocks[0]
    assert blocks[0]["source"]["media_type"] == "image/png", blocks[0]
    assert base64.b64decode(blocks[0]["source"]["data"]) == data
    assert blocks[1]["text"] == "[Image #1] what is this", blocks[1]


def test_a_second_image_keeps_its_own_number(ctx):
    """Two attachments are two blocks and two markers, in the order attached."""
    a = shot(ctx, "one.png", 4, 3)
    b = shot(ctx, "two.png", 6, 5)
    ctx.scenario("text=two+pictures")
    s = ctx.spawn()
    attach(s, "one.png")
    attach(s, "two.png", 2)
    s.submit("compare them")
    s.wait_text("two pictures")
    s.wait_turn_done()

    body = ctx.mock.requests[-1]
    urls = openai_images(body)
    assert len(urls) == 2, body["messages"][-1]
    assert [base64.b64decode(u.partition(",")[2]) for u in urls] == [a, b]
    assert body["messages"][-1]["content"][0]["text"] == (
        "[Image #1] [Image #2] compare them")


def test_deleting_a_placeholder_detaches_its_image(ctx):
    """The marker is the attachment: what the user removed is not sent, and
    what is left is renumbered so the text and the blocks agree."""
    shot(ctx, "one.png", 4, 3)
    second = shot(ctx, "two.png", 6, 5)
    ctx.scenario("text=one+picture")
    s = ctx.spawn()
    attach(s, "one.png")
    attach(s, "two.png", 2)
    s.key("ctrl-u").sync()
    s.submit("only [Image #2] please")
    s.wait_text("one picture")
    s.wait_turn_done()

    body = ctx.mock.requests[-1]
    urls = openai_images(body)
    assert len(urls) == 1, body["messages"][-1]
    assert base64.b64decode(urls[0].partition(",")[2]) == second
    assert body["messages"][-1]["content"][0]["text"] == "only [Image #1] please"


def test_an_image_is_replayed_on_the_next_round(ctx):
    """A later turn still carries the image the first one attached."""
    data = shot(ctx)
    ctx.scenario("text=ok")
    s = ctx.spawn()
    attach(s, "shot.png")
    s.submit("look")
    s.wait_text("ok")
    s.wait_turn_done()
    s.submit("and now")
    s.wait_turn_done()

    body = ctx.mock.requests[-1]
    first = body["messages"][1]["content"]
    assert [p["type"] for p in first] == ["text", "image_url"], first
    assert base64.b64decode(
        first[1]["image_url"]["url"].partition(",")[2]) == data


# ---- the transcript -------------------------------------------------------
def test_the_transcript_names_the_image(ctx):
    """A cell grid names an image rather than drawing it."""
    shot(ctx)
    ctx.scenario("text=seen")
    s = ctx.spawn()
    attach(s, "shot.png")
    s.submit("look at this")
    s.wait_text("seen")
    s.wait_turn_done()
    text = s.text()
    assert "[Image #1] look at this" in text, text
    assert "[Image #1] shot.png - png 4x3" in text, text


# ---- the context gauge ----------------------------------------------------
def test_an_attached_image_is_counted_in_the_context_field(ctx):
    """An image is billed by its pixels, so the estimate has to include it
    even though none of its bytes are conversation text."""
    shot(ctx, "wide.png", 900, 750)     # 675000 pixels, about 900 tokens
    ctx.scenario("text=ok,usage=5000/100")
    s = ctx.spawn()
    s.submit("start")
    s.wait_turn_done()
    base = tokens(s)

    # Parked before its first delta, the request states no usage, so the
    # field is still estimating the turn that just went out.
    ctx.scenario("text=ok,usage=5000/100,hold=1")
    attach(s, "wide.png")
    s.submit("look")
    s.wait_for(lambda t: tokens(s) > base + 700,
               "the attached image to reach the context field")
    assert tokens(s) < base + 1400, "an image is pixels, not its base64"
    ctx.mock.release()
    s.wait_turn_done()


# ---- sessions -------------------------------------------------------------
def test_an_image_is_saved_beside_the_session(ctx):
    """The line names a sidecar; the bytes are never inlined into the file."""
    data = shot(ctx)
    ctx.scenario("text=ok")
    s = ctx.spawn()
    attach(s, "shot.png")
    s.submit("look")
    s.wait_text("ok")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    d = sessions_dir(ctx)
    line = json.loads(sorted(d.glob("*.jsonl"))[0].read_text().splitlines()[0])
    assert line["content"] == "[Image #1] look", line
    assert len(line["media"]) == 1, line
    entry = line["media"][0]
    assert entry["mime"] == "image/png" and entry["label"] == "shot.png", entry
    assert entry["w"] == 4 and entry["h"] == 3, entry
    assert entry["file"].startswith("media/"), entry
    assert (d / entry["file"]).read_bytes() == data
    assert base64.b64encode(data)[:16].decode() not in (d / "").as_posix()


def test_a_resumed_session_sends_the_image_again(ctx):
    """The sidecar is read back, so the model sees what it saw before."""
    data = shot(ctx)
    ctx.scenario("text=ok")
    s = ctx.spawn()
    attach(s, "shot.png")
    s.submit("look")
    s.wait_text("ok")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    ctx.scenario("text=still+here")
    s2 = ctx.spawn()
    s2.submit("/resume")
    s2.wait_status("pick a session")
    s2.key("enter")
    s2.wait_for(lambda t: t.contains("[Image #1] shot.png - png 4x3"),
                "the replayed turn to name its image")
    assert "[Image #1] look" in s2.text(), s2.text()
    s2.submit("again")
    s2.wait_text("still here")
    s2.wait_turn_done()

    body = ctx.mock.requests[-1]
    first = body["messages"][1]["content"]
    assert base64.b64decode(
        first[1]["image_url"]["url"].partition(",")[2]) == data


def test_a_lost_sidecar_leaves_the_turn_sendable(ctx):
    """An image whose file is gone keeps its number and nothing else."""
    shot(ctx)
    ctx.scenario("text=ok")
    s = ctx.spawn()
    attach(s, "shot.png")
    s.submit("look")
    s.wait_text("ok")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    for f in (sessions_dir(ctx) / "media").iterdir():
        f.unlink()

    ctx.scenario("text=still+here")
    s2 = ctx.spawn()
    s2.submit("/resume")
    s2.wait_status("pick a session")
    s2.key("enter")
    s2.wait_for(lambda t: t.contains("shot.png - png - unavailable"),
                "the replayed turn to name the image it lost")
    assert "[Image #1] look" in s2.text(), s2.text()
    s2.submit("again")
    s2.wait_text("still here")
    s2.wait_turn_done()

    body = ctx.mock.requests[-1]
    assert openai_images(body) == [], body["messages"]
    assert body["messages"][1]["content"] == "[Image #1] look", body["messages"]


# ---- the images setting ---------------------------------------------------
def test_images_off_withdraws_the_command(ctx):
    """`images = off` is a connection that carries none: the command is not
    offered, and typed anyway it names the setting rather than failing."""
    shot(ctx)
    s = ctx.spawn(ARQAN_IMAGES="off")
    s.type("/att").sync()
    assert "/attach" not in s.text(), s.text()
    s.key("ctrl-u").sync()
    s.type("/attach shot.png").sync()
    s.key("enter")
    s.wait_text("images are off")
    wait_composer(s, "")


def test_images_off_sends_no_image_a_session_saved(ctx):
    """A session resumed with images off replays its text and none of its
    pictures: nothing reaches a model that was told it cannot see."""
    shot(ctx)
    ctx.scenario("text=ok")
    s = ctx.spawn()
    attach(s, "shot.png")
    s.submit("look")
    s.wait_text("ok")
    s.wait_turn_done()
    s.submit("/exit")
    s.wait_exit()

    ctx.scenario("text=still+here")
    s2 = ctx.spawn(ARQAN_IMAGES="off")
    s2.submit("/resume")
    s2.wait_status("pick a session")
    s2.key("enter")
    s2.wait_for(lambda t: t.contains("[Image #1] look"), "the replayed turn")
    assert "shot.png" not in s2.text(), s2.text()
    s2.submit("again")
    s2.wait_text("still here")
    s2.wait_turn_done()

    body = ctx.mock.requests[-1]
    assert openai_images(body) == [], body["messages"]
    assert body["messages"][1]["content"] == "[Image #1] look", body["messages"]


def test_a_project_file_cannot_turn_images_on(ctx):
    """The setting names what the user's connection carries, so a repository
    may not add image bytes to a request the user disabled."""
    shot(ctx)
    config = ctx.config_file()
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text("images = off\n")
    ctx.write_project_config("images = auto\n")

    s = ctx.spawn()
    s.type("/attach shot.png").sync()
    s.key("enter")
    s.wait_text("images are off")


# ---- the clipboard --------------------------------------------------------
def clipboard(ctx, data: bytes = None, types=("text/plain", "image/png")):
    """A PATH holding one fake `wl-paste`, and nothing else.

    The directory is the whole PATH so that no clipboard reader installed on
    the machine can answer instead: what a case says the clipboard holds is
    what arqan sees. `data` of None is a helper that lists types but hands
    over nothing.
    """
    bindir = ctx.work / "clipbin"
    bindir.mkdir(exist_ok=True)
    listing = " ".join(types)
    body = f'exec cat "{ctx.work}/clipboard.bin"\n' if data is not None else "exit 1\n"
    if data is not None:
        (ctx.work / "clipboard.bin").write_bytes(data)
    script = bindir / "wl-paste"
    script.write_text(
        "#!/bin/sh\n"
        "PATH=/usr/bin:/bin\n"
        f'[ "$1" = "--list-types" ] && {{ printf "%s\\n" {listing}; exit 0; }}\n'
        + body
    )
    script.chmod(0o755)
    return str(bindir)


def test_ctrl_v_attaches_the_clipboard_image(ctx):
    """A screenshot is the one image a user has no filename for."""
    path = clipboard(ctx, png(4, 3))
    s = ctx.spawn(PATH=path)
    s.key("ctrl-v")
    s.wait_text("attached [Image #1] clipboard - png 4x3")
    wait_composer(s, "[Image #1]")


def test_ctrl_v_keeps_what_was_already_typed(ctx):
    """The key is a command, not a paste: the draft it lands in survives and
    the marker joins it."""
    path = clipboard(ctx, png(4, 3))
    ctx.scenario("text=a+red+rectangle")
    s = ctx.spawn(PATH=path)
    s.type("what is this").sync()
    s.key("ctrl-v")
    s.wait_text("attached [Image #1]")
    wait_composer(s, "what is this [Image #1]")
    s.key("enter")
    s.wait_text("a red rectangle")
    s.wait_turn_done()

    body = ctx.mock.requests[-1]
    content = body["messages"][-1]["content"]
    assert content[0]["text"] == "what is this [Image #1]", content
    assert len(openai_images(body)) == 1, content


def test_attach_with_no_path_reads_the_clipboard(ctx):
    """The command and the key are one path: no argument means the clipboard."""
    path = clipboard(ctx, png(6, 5))
    s = ctx.spawn(PATH=path)
    s.type("/attach").sync()
    s.key("enter")
    s.wait_text("attached [Image #1] clipboard - png 6x5")


def test_a_clipboard_with_no_image_says_so(ctx):
    """Text on the clipboard is not an attachment."""
    path = clipboard(ctx, types=("text/plain", "text/html"))
    s = ctx.spawn(PATH=path)
    s.key("ctrl-v")
    s.wait_text("the clipboard holds no image")
    wait_composer(s, "")


def test_no_clipboard_reader_names_the_helpers(ctx):
    """Nothing installed is answered with what to install."""
    empty = ctx.work / "nobin"
    empty.mkdir()
    s = ctx.spawn(PATH=str(empty))
    s.key("ctrl-v")
    s.wait_text("no clipboard reader found")


def test_an_oversized_clipboard_image_is_refused_with_its_limit(ctx):
    """Over the cap is refused by naming the cap, as a file's would be."""
    path = clipboard(ctx, png(8, 8) + b"\x00" * (6 << 20))
    s = ctx.spawn(PATH=path)
    s.key("ctrl-v")
    s.wait_text("the clipboard image is over 5.0 MB")
    wait_composer(s, "")


def test_ctrl_v_during_a_turn_attaches_to_the_next_message(ctx):
    """An attachment belongs to the message after the running one, so the key
    works mid-turn and the marker waits in the composer."""
    path = clipboard(ctx, png(4, 3))
    ctx.scenario("text=ok,hold=1")
    s = ctx.spawn(PATH=path)
    s.submit("start")
    s.wait_activity("thinking")
    s.type("and this").sync()
    s.key("ctrl-v")
    wait_composer(s, "and this [Image #1]")
    ctx.mock.release()
    s.wait_turn_done()

    ctx.scenario("text=seen")
    s.key("enter")
    s.wait_text("seen")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert body["messages"][-1]["content"][0]["text"] == "and this [Image #1]"
    assert len(openai_images(body)) == 1, body["messages"][-1]


def test_picking_an_image_from_the_path_popup_attaches_it(ctx):
    """A path names a file the model can open; an image is not one of those,
    so the picker hands over the bytes instead of the name."""
    shot(ctx)
    ctx.write_file("notes.txt", "hello\n")
    s = ctx.spawn()
    s.type("look at @sho").sync()
    s.key("tab")
    s.wait_text("attached [Image #1] shot.png - png 4x3")
    wait_composer(s, "look at [Image #1]")
    assert "shot.png" not in s.composer_text(), s.composer_lines()


def test_enter_on_a_picked_image_attaches_without_sending(ctx):
    """Enter takes the entry the popup is on, and taking an image is an
    attachment rather than a message."""
    shot(ctx)
    s = ctx.spawn()
    s.type("@sho").sync()
    s.key("enter")
    s.wait_text("attached [Image #1]")
    wait_composer(s, "[Image #1]")
    assert s.proc.poll() is None
    assert not ctx.mock.requests, ctx.mock.requests


def test_a_picked_image_is_sent_with_the_message(ctx):
    """The attachment the picker made travels like any other."""
    shot(ctx)
    ctx.scenario("text=a+red+rectangle")
    s = ctx.spawn()
    s.type("what is @sho").sync()
    s.key("tab")
    s.wait_text("attached [Image #1]")
    wait_composer(s, "what is [Image #1]")
    s.key("enter")
    s.wait_text("a red rectangle")
    s.wait_turn_done()

    body = ctx.mock.requests[-1]
    content = body["messages"][-1]["content"]
    assert content[0]["text"] == "what is [Image #1]", content
    assert len(openai_images(body)) == 1, content


def test_picking_a_text_file_still_writes_its_path(ctx):
    """Only an image is taken as an attachment: everything else is a path the
    tools can read."""
    ctx.write_file("notes.txt", "hello\n")
    s = ctx.spawn()
    s.type("read @no").sync()
    s.key("tab").sync()
    assert s.composer_text() == "read @notes.txt", s.composer_lines()


def test_images_off_leaves_a_picked_image_as_a_path(ctx):
    """With nothing to attach to, an image is a path like any other."""
    shot(ctx)
    s = ctx.spawn(ARQAN_IMAGES="off")
    s.type("look at @sho").sync()
    s.key("tab").sync()
    assert s.composer_text() == "look at @shot.png", s.composer_lines()


def test_picking_an_image_during_a_turn_attaches_to_the_next_message(ctx):
    """The picker answers mid-turn as the key does, and the marker waits in
    the composer for the message after the running one."""
    shot(ctx)
    ctx.scenario("text=ok,hold=1")
    s = ctx.spawn()
    s.submit("start")
    s.wait_activity("thinking")
    s.type("and @sho").sync()
    s.key("tab")
    s.wait_text("attached [Image #1]")
    wait_composer(s, "and [Image #1]")
    ctx.mock.release()
    s.wait_turn_done()


def test_a_long_image_path_attaches_mid_turn(ctx):
    """A path outruns any command name, so the mid-turn handler must read it
    from the line rather than a buffer sized for commands."""
    deep = "assets/screenshots/2024/very-long-directory-name/shot.png"
    (ctx.work / deep).parent.mkdir(parents=True, exist_ok=True)
    (ctx.work / deep).write_bytes(png(4, 3))
    ctx.scenario("text=ok,hold=1")
    s = ctx.spawn()
    s.submit("start")
    s.wait_activity("thinking")
    s.type(f"/attach {deep}").sync()
    s.key("enter")
    s.wait_text("attached [Image #1] shot.png - png 4x3")
    wait_composer(s, "[Image #1]")
    ctx.mock.release()
    s.wait_turn_done()
