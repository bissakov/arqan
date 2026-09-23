"""Image reads use the same tool-result wire format as MCP images."""

import base64
import json

from .test_image import png
from .test_mcp_images import openai_images


def formats():
    return [
        ("png", png(4, 3)),
        ("jpeg", bytes.fromhex("ffd8ffc00011080003000403012200021101031100ffd9")),
        ("gif", b"GIF89a\x04\x00\x03\x00" + bytes(20)),
        ("webp", b"RIFF" + bytes(4) + b"WEBPVP8X" + bytes(8)
         + b"\x03\x00\x00\x02\x00\x00"),
    ]


def read_formats(ctx, **env):
    calls = []
    for kind, data in formats():
        path = f"{kind}.txt"
        (ctx.work / path).write_bytes(data)
        calls.append("tool=read:" + json.dumps({"path": path, "offset": 2,
                                               "limit": 1}))
    ctx.scenario(",".join(calls) + ",final_text=inspected")
    s = ctx.spawn(**env)
    s.submit("inspect the images")
    s.wait_text("inspected")
    s.wait_turn_done()
    return s, ctx.mock.requests[-1]


def test_read_images_openai_exact_content(ctx):
    _, body = read_formats(ctx)
    urls = openai_images(body)
    assert len(urls) == 4, body
    for url, (kind, data) in zip(urls, formats()):
        assert url.startswith(f"data:image/{kind};base64,"), url[:80]
        assert base64.b64decode(url.split(",", 1)[1]) == data
    results = ctx.mock.tool_results()
    assert all("Image #" in result for result in results), results


def test_read_images_anthropic_exact_content(ctx):
    _, body = read_formats(ctx, ARQAN_API="anthropic")
    results = body["messages"][-1]["content"]
    assert len(results) == 4, results
    for result, (kind, data) in zip(results, formats()):
        assert result["type"] == "tool_result", result
        assert result["content"][0]["type"] == "text", result
        image = result["content"][1]
        assert image["type"] == "image", image
        assert image["source"]["media_type"] == f"image/{kind}"
        assert base64.b64decode(image["source"]["data"]) == data


def test_read_images_off_adds_no_media(ctx):
    _, body = read_formats(ctx, ARQAN_IMAGES="off")
    assert openai_images(body) == []
    assert all("images are off" in result for result in ctx.mock.tool_results())


def test_read_images_plan_mode(ctx):
    _, body = read_formats(ctx, ARQAN_MODE="plan")
    assert len(openai_images(body)) == 4


def test_read_images_resume(ctx):
    s, _ = read_formats(ctx)
    s.submit("/exit")
    s.wait_exit()
    ctx.scenario("text=resumed,final_text=resumed")
    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("inspected")
    s.submit("again")
    s.wait_text("resumed")
    s.wait_turn_done()
    urls = openai_images(ctx.mock.requests[-1])
    assert len(urls) == 4
    for url, (_, data) in zip(urls, formats()):
        assert base64.b64decode(url.split(",", 1)[1]) == data


def read_one(ctx, path, **env):
    ctx.scenario("tool=read:" + json.dumps({"path": path})
                 + ",final_text=inspected")
    s = ctx.spawn(**env)
    s.submit("inspect the file")
    s.wait_text("inspected")
    s.wait_turn_done()
    return s, ctx.mock.tool_results()[0]


def test_read_image_size_limit(ctx):
    (ctx.work / "large.png").write_bytes(png(4, 3) + bytes(5 << 20))
    _, result = read_one(ctx, "large.png")
    assert result.startswith("ERROR:"), result
    assert "limit is 5.0 MB" in result, result
    assert openai_images(ctx.mock.requests[-1]) == []


def test_read_image_dimension_limit(ctx):
    data = bytearray(png(4, 3))
    data[16:20] = (8001).to_bytes(4, "big")
    (ctx.work / "wide.png").write_bytes(data)
    _, result = read_one(ctx, "wide.png")
    assert "limit is 8000 on a side" in result, result
    assert openai_images(ctx.mock.requests[-1]) == []


def test_read_image_missing(ctx):
    _, result = read_one(ctx, "missing.png")
    assert "open missing.png failed" in result, result
    assert openai_images(ctx.mock.requests[-1]) == []


def test_read_image_directory(ctx):
    (ctx.work / "directory.png").mkdir()
    _, result = read_one(ctx, "directory.png")
    assert "is a directory" in result, result
    assert openai_images(ctx.mock.requests[-1]) == []


def test_read_image_nul_path(ctx):
    (ctx.work / "real.png").write_bytes(png(4, 3))
    _, result = read_one(ctx, "real.png\x00.txt")
    assert "contains a nul byte" in result, result
    assert openai_images(ctx.mock.requests[-1]) == []


def test_read_image_bad_path_length(ctx):
    _, result = read_one(ctx, "x" * 4096)
    assert "path too long" in result, result
    assert openai_images(ctx.mock.requests[-1]) == []


def test_read_image_disabled_tool(ctx):
    (ctx.work / "shot.png").write_bytes(png(4, 3))
    ctx.scenario('tool=read:{"path":"shot.png"},final_text=inspected')
    s = ctx.spawn()
    s.open_settings().settings_select("read")
    s.key("space").sync()
    s.wait_text("[ ] read")
    s.key("esc").sync()
    s.wait_status("ready")
    s.submit("inspect the image")
    s.wait_text("inspected")
    s.wait_turn_done()
    assert "read is disabled" in ctx.mock.tool_results()[0]
    assert openai_images(ctx.mock.requests[-1]) == []


def test_read_image_session_capacity(ctx):
    (ctx.work / "shot.png").write_bytes(png(4, 3))
    ctx.scenario('tool=read:{"path":"shot.png"},tool_rounds=65,'
                 'final_text=inspected')
    s = ctx.spawn()
    s.submit("inspect the image repeatedly")
    s.wait_text("inspected")
    s.wait_turn_done()
    assert "no room for another image" in ctx.mock.tool_results()[-1]


def test_read_image_subagent_stays_text_only(ctx):
    from .test_subagent import collect, spawn, sub_requests

    (ctx.work / "shot.png").write_bytes(png(4, 3))
    ctx.scenario(collect(ctx, prompt="read shot.png"))
    s = spawn(ctx, sub='tool=read:{"path":"shot.png"},final_text=no+image')
    s.submit("delegate it")
    s.wait_text("done")
    s.wait_turn_done()
    requests = sub_requests(ctx)
    assert len(requests) == 2, requests
    result = requests[-1]["messages"][-1]
    assert result["role"] == "tool", result
    assert "does not support images" in result["content"], result
    assert all(openai_images(body) == [] for body in ctx.mock.requests)
    ctx.scenario('tool=read:{"path":"shot.png"},final_text=inspected')
    s.submit("read it yourself")
    s.wait_text("inspected")
    s.wait_turn_done()
    assert len(openai_images(ctx.mock.requests[-1])) == 1


def test_read_image_resume_off(ctx):
    s, _ = read_formats(ctx)
    s.submit("/exit")
    s.wait_exit()
    ctx.scenario("text=resumed,final_text=resumed")
    s = ctx.spawn(ARQAN_IMAGES="off")
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("inspected")
    s.submit("again")
    s.wait_text("resumed")
    s.wait_turn_done()
    assert openai_images(ctx.mock.requests[-1]) == []


def test_read_image_missing_sidecar(ctx):
    from .test_image import sessions_dir

    s, _ = read_formats(ctx)
    s.submit("/exit")
    s.wait_exit()
    sidecars = list(sessions_dir(ctx).rglob("*.png"))
    assert len(sidecars) == 1, sidecars
    sidecars[0].unlink()
    ctx.scenario("text=resumed,final_text=resumed")
    s = ctx.spawn()
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("inspected")
    s.submit("again")
    s.wait_text("resumed")
    s.wait_turn_done()
    urls = openai_images(ctx.mock.requests[-1])
    assert len(urls) == 3, urls
    for url, (kind, data) in zip(urls, formats()[1:]):
        assert url.startswith(f"data:image/{kind};base64,"), url[:80]
        assert base64.b64decode(url.split(",", 1)[1]) == data
