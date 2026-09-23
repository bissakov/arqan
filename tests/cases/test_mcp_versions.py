"""Which protocol era a server speaks, and how the client finds out."""

import json
import sys
from pathlib import Path

from .test_mcp_http import start_http_server, tool_names, write_url_mcp

SERVER = str(Path(__file__).resolve().parents[1] / "harness" / "mcpserver.py")


def write_era_mcp(ctx, era, name="demo", log=None):
    args = [SERVER, "--era", era]
    if log:
        args += ["--log", str(log)]
    entry = {"command": sys.executable, "args": args}
    p = ctx.xdg / "arqan" / "mcp.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"servers": {name: entry}}))
    return p


def test_modern_stdio_server_needs_no_handshake(ctx):
    """server/discover answers, so no initialize is sent."""
    log = ctx.tmp / "mcp.log"
    write_era_mcp(ctx, "modern", log=log)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()
    lines = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    methods = [m.get("method") for m in lines]
    assert methods[0] == "server/discover", methods
    assert "initialize" not in methods, methods
    meta = (lines[-1].get("params") or {}).get("_meta") or {}
    assert meta.get("io.modelcontextprotocol/protocolVersion") == "2026-07-28", meta


def test_a_modern_server_that_shares_no_version_is_not_downgraded(ctx):
    """UnsupportedProtocolVersionError stops the fallback to initialize."""
    log = ctx.tmp / "mcp.log"
    write_era_mcp(ctx, "unsupported", log=log)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    lines = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    methods = [m.get("method") for m in lines]
    assert methods == ["server/discover"], methods
    s.send("/mcp\r")
    s.wait_text("failed")
    assert "2027-01-01" in s.text(), s.text()


def test_modern_http_server_mirrors_the_body_in_headers(ctx):
    """Mcp-Method and Mcp-Name go beside the protocol version header."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, era="modern", log=log)
    write_url_mcp(ctx, url)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()
    posts = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    posts = [p for p in posts if "raw" in p]
    assert all(p["protocol"] == "2026-07-28" for p in posts), posts
    calls = [p for p in posts if p["mcp_method"] == "tools/call"]
    assert calls and calls[0]["mcp_name"] == "echo", posts


def test_modern_http_server_gets_no_session_header(ctx):
    """Sessions are gone in the modern era, so nothing is sent or stored."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, era="modern", session=True, log=log)
    write_url_mcp(ctx, url)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    posts = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    assert all(p.get("session") is None for p in posts if "raw" in p), posts
    assert not [n for n in tool_names(ctx.mock.requests[-1]) if n == "demo_boom"]


def test_modern_http_tool_parameters_become_headers(ctx):
    """x-mcp-header mirrors primitive values, including nested ones."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, mode="headers", era="modern", log=log)
    write_url_mcp(ctx, url)
    ctx.scenario(
        'tool=demo_headed:{"region":" padded ","count":7,'
        '"nested":{"trace":true}},final_text=done'
    )
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("call it")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["headers received"], ctx.mock.tool_results()
    posts = [json.loads(line) for line in log.read_text().splitlines()]
    calls = [p for p in posts if p.get("mcp_method") == "tools/call"]
    assert len(calls) == 1, posts
    assert calls[0]["param_region"] == "=?base64?IHBhZGRlZCA=?=", calls[0]
    assert calls[0]["param_count"] == "7", calls[0]
    assert calls[0]["param_trace"] == "true", calls[0]


def test_modern_http_drops_a_tool_with_an_invalid_header_schema(ctx):
    """An x-mcp-header on a non-primitive property rejects that tool."""
    url = start_http_server(ctx, mode="invalid-headers", era="modern")
    write_url_mcp(ctx, url)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert "demo_invalid_header" not in tool_names(ctx.mock.requests[-1])


def test_a_server_that_lists_only_an_older_version_gets_initialize(ctx):
    """Any handshake-era version in supported means fall back."""
    log = ctx.tmp / "mcp.log"
    write_era_mcp(ctx, "old-only", log=log)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()
    lines = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    methods = [m.get("method") for m in lines]
    assert methods[:2] == ["server/discover", "initialize"], methods
    hello = lines[1]["params"]
    assert hello["protocolVersion"] == "2025-11-25", hello


def test_an_unknown_initialize_version_fails_the_server(ctx):
    """A version outside the known handshake era is reported, not used."""
    entry = {
        "command": sys.executable,
        "args": [SERVER, "--legacy-version", "1999-01-01"],
    }
    p = ctx.xdg / "arqan" / "mcp.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"servers": {"demo": entry}}))
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert not [n for n in tool_names(ctx.mock.requests[-1])
                if n.startswith("demo_")]
    s.send("/mcp\r")
    s.wait_text("failed")
    assert "1999-01-01" in s.text(), s.text()


def test_http_probe_refused_with_method_not_found_falls_back(ctx):
    """A 400 carrying -32601 is a server that predates server/discover."""
    url = start_http_server(ctx, extra=["--reject-status", "400"])
    write_url_mcp(ctx, url)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()
