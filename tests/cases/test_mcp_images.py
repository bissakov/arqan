"""Images in MCP tool results, on the wire and after replay."""

import base64
import json
import os

from cases.test_image import png
from cases.test_mcp import write_mcp
from cases.test_mcp_http import start_http_server, write_url_mcp
from cases.test_budget import windowed


def openai_images(body):
    return [part["image_url"]["url"] for msg in body["messages"]
            if isinstance(msg.get("content"), list)
            for part in msg["content"] if part.get("type") == "image_url"]


def setup_image(ctx, size=0, count=1):
    data = png(4, 3)
    data += b"\x00" * max(0, size - len(data))
    path = ctx.work / "result.png"
    path.write_bytes(data)
    write_mcp(ctx, env={"MCP_TEST_IMAGE": str(path),
                       "MCP_TEST_IMAGE_COUNT": str(count)})
    ctx.scenario("tool=demo_picture:{},final_text=done")
    return data


def run_image(ctx, **env):
    s = ctx.spawn(ARQAN_MCP="on", **env)
    s.submit("show me")
    s.wait_text("done")
    s.wait_turn_done()
    return s, ctx.mock.requests[-1]


def test_mcp_image_openai_follows_tool_result(ctx):
    data = setup_image(ctx)
    _, body = run_image(ctx)
    messages = body["messages"]
    assert messages[-2]["role"] == "tool", messages
    assert isinstance(messages[-2]["content"], str), messages
    assert "Image #1" in messages[-2]["content"], messages
    assert messages[-1]["role"] == "user", messages
    assert messages[-2]["tool_call_id"] in messages[-1]["content"][1]["text"]
    urls = openai_images(body)
    assert len(urls) == 1, urls
    assert base64.b64decode(urls[0].split(",", 1)[1]) == data
    assert len(ctx.mock.requests) == 2


def test_mcp_image_anthropic_is_inside_tool_result(ctx):
    data = setup_image(ctx)
    _, body = run_image(ctx, ARQAN_API="anthropic")
    blocks = body["messages"][-1]["content"]
    assert len(blocks) == 1, blocks
    result = blocks[0]
    assert result["type"] == "tool_result", result
    assert result["content"][0]["type"] == "text", result
    image = result["content"][1]
    assert image["type"] == "image", image
    assert image["source"]["media_type"] == "image/png", image
    assert base64.b64decode(image["source"]["data"]) == data


def test_mcp_image_off_keeps_plain_tool_result(ctx):
    setup_image(ctx)
    _, body = run_image(ctx, ARQAN_IMAGES="off")
    assert openai_images(body) == []
    assert body["messages"][-1]["role"] == "tool"
    assert "images are off" in body["messages"][-1]["content"]


def test_mcp_image_large_reply_uses_overflow(ctx):
    data = setup_image(ctx, size=400000)
    _, body = run_image(ctx)
    urls = openai_images(body)
    assert len(urls) == 1
    assert base64.b64decode(urls[0].split(",", 1)[1]) == data


def test_mcp_image_fifth_is_refused(ctx):
    setup_image(ctx, count=5)
    _, body = run_image(ctx)
    assert len(openai_images(body)) == 4
    assert "at most 4 images" in body["messages"][-2]["content"]


def test_mcp_image_over_limit_is_refused(ctx):
    setup_image(ctx, size=(5 << 20) + 1)
    _, body = run_image(ctx)
    assert openai_images(body) == []
    assert "5.0 MB limit" in body["messages"][-1]["content"]


def test_mcp_image_replays_from_session(ctx):
    data = setup_image(ctx)
    s, _ = run_image(ctx)
    s.submit("/exit")
    s.wait_exit()
    ctx.scenario("text=resumed,final_text=resumed")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("here it is")
    s.submit("again")
    s.wait_text("resumed")
    s.wait_turn_done()
    urls = openai_images(ctx.mock.requests[-1])
    assert len(urls) == 1
    assert base64.b64decode(urls[0].split(",", 1)[1]) == data


def test_mcp_image_overflow_reused_between_servers(ctx):
    data = setup_image(ctx, size=400000)
    path = ctx.xdg / "arqan" / "mcp.json"
    conf = json.loads(path.read_text())
    conf["servers"]["other"] = dict(conf["servers"]["demo"])
    path.write_text(json.dumps(conf))
    s, _ = run_image(ctx)
    ctx.scenario("tool=other_picture:{},final_text=second")
    s.submit("another")
    s.wait_text("second")
    s.wait_turn_done()
    urls = openai_images(ctx.mock.requests[-1])
    assert len(urls) == 2, [(m.get("role"), str(m.get("content"))[:150])
                            for m in ctx.mock.requests[-1]["messages"]]
    assert all(base64.b64decode(url.split(",", 1)[1]) == data for url in urls)


def test_mcp_image_http_sse(ctx):
    data = setup_image(ctx, size=400000)
    url = start_http_server(ctx, sse=True, env={
        **os.environ, "MCP_TEST_IMAGE": str(ctx.work / "result.png")})
    write_url_mcp(ctx, url)
    _, body = run_image(ctx)
    urls = openai_images(body)
    assert len(urls) == 1
    assert base64.b64decode(urls[0].split(",", 1)[1]) == data


def test_mcp_images_follow_all_parallel_tool_results(ctx):
    setup_image(ctx)
    ctx.scenario("tool=demo_picture:{},tool=demo_echo:{\"text\":\"hello\"},"
                 "final_text=done")
    _, body = run_image(ctx)
    messages = body["messages"]
    assert [m["role"] for m in messages[-3:]] == ["tool", "tool", "user"]
    assert "echo: hello" in messages[-2]["content"]
    assert len(openai_images(body)) == 1


def test_mcp_image_resumed_with_images_off(ctx):
    setup_image(ctx)
    s, _ = run_image(ctx)
    s.submit("/exit")
    s.wait_exit()
    ctx.scenario("text=resumed,final_text=resumed")
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_IMAGES="off")
    s.submit("/resume")
    s.wait_status("pick a session")
    s.key("enter")
    s.wait_text("here it is")
    s.submit("again")
    s.wait_text("resumed")
    s.wait_turn_done()
    assert openai_images(ctx.mock.requests[-1]) == []


def test_mcp_images_from_old_rounds_are_not_replayed(ctx):
    setup_image(ctx)
    (ctx.work / "result.png").write_bytes(png(1000, 1000))
    ctx.scenario("tool=demo_picture:{},tool_rounds=9,final_text=done")
    s = windowed(ctx, window=500, ARQAN_MCP="on")
    s.submit("show me")
    s.wait_text("done")
    s.wait_turn_done()
    body = ctx.mock.requests[-1]
    assert len([m for m in body["messages"] if m["role"] == "tool"]) == 9
    assert len(openai_images(body)) == 5, len(openai_images(body))


def test_mcp_image_side_limit_is_named(ctx):
    setup_image(ctx)
    path = ctx.work / "result.png"
    data = bytearray(path.read_bytes())
    data[16:20] = (20000).to_bytes(4, "big")
    path.write_bytes(data)
    _, body = run_image(ctx)
    assert openai_images(body) == []
    result = body["messages"][-1]["content"]
    assert "the limit is 8000 on a side" in result, result


def test_mcp_failed_large_reply_releases_shared_buffer(ctx):
    setup_image(ctx, size=6 << 20)
    data = png(4, 3) + b"\x00" * 400000
    small = ctx.work / "small.png"
    small.write_bytes(data)
    path = ctx.xdg / "arqan" / "mcp.json"
    conf = json.loads(path.read_text())
    conf["servers"]["other"] = {
        **conf["servers"]["demo"], "env": {"MCP_TEST_IMAGE": str(small)}}
    path.write_text(json.dumps(conf))
    s, body = run_image(ctx)
    assert "cannot be parsed" in body["messages"][-1]["content"]
    ctx.scenario("tool=other_picture:{},final_text=second")
    s.submit("another")
    s.wait_text("second")
    s.wait_turn_done()
    urls = openai_images(ctx.mock.requests[-1])
    assert len(urls) == 1, ctx.mock.tool_results()
    assert base64.b64decode(urls[0].split(",", 1)[1]) == data
