"""MCP resources as a tool, and MCP prompts through /mcp."""

import json
import sys
from pathlib import Path

SERVER = str(Path(__file__).resolve().parents[1] / "harness" / "mcpserver.py")


def write_extras_mcp(ctx, name="demo", era="legacy"):
    entry = {
        "command": sys.executable,
        "args": [SERVER, "--extras", "--era", era],
    }
    p = ctx.xdg / "arqan" / "mcp.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"servers": {name: entry}}))
    return p


def tool_names(request):
    return [t["function"]["name"] for t in request.get("tools", [])]


def test_a_server_with_resources_gets_a_resources_tool(ctx):
    """The capability adds one tool, and no uri lists what is on offer."""
    write_extras_mcp(ctx)
    ctx.scenario("tool=mcp_read:{},final_text=listed")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("what is there")
    s.wait_text("listed")
    s.wait_turn_done()

    assert "mcp_read" in tool_names(ctx.mock.requests[-1])
    out = ctx.mock.tool_results()[0]
    assert "demo://notes" in out, out
    assert "text/plain" in out, out


def test_reading_a_resource_returns_its_text(ctx):
    """A uri reads that one resource."""
    write_extras_mcp(ctx)
    ctx.scenario(
        'tool=mcp_read:{"server":"demo","uri":"demo://notes"},final_text=read'
    )
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("read the notes")
    s.wait_text("read")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["buy milk"], ctx.mock.tool_results()


def test_a_binary_resource_is_named_not_passed_on(ctx):
    """A blob is named in the result instead of being sent to the model."""
    write_extras_mcp(ctx)
    ctx.scenario(
        'tool=mcp_read:{"server":"demo","uri":"demo://logo"},final_text=read'
    )
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("read the logo")
    s.wait_text("read")
    s.wait_turn_done()

    out = ctx.mock.tool_results()[0]
    assert "demo://logo" in out and "not passed on" in out, out


def test_a_server_without_resources_gets_no_such_tool(ctx):
    """The tool only shows up when the server says it has resources."""
    p = ctx.xdg / "arqan" / "mcp.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(
        json.dumps(
            {"servers": {"demo": {"command": sys.executable, "args": [SERVER]}}}
        )
    )
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert "mcp_read" not in tool_names(ctx.mock.requests[-1])


def test_mcp_read_can_be_disabled_before_the_server_connects(ctx):
    """disable_tools accepts the dynamic resource tool name at startup."""
    write_extras_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_DISABLE_TOOLS="mcp_read")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert "mcp_read" not in tool_names(ctx.mock.requests[-1])


def test_mcp_prompts_lists_what_a_server_offers(ctx):
    """/mcp prompts names the server, the prompt and its arguments."""
    write_extras_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp prompts\r")
    s.wait_text("MCP prompts")
    assert "demo review" in s.text(), s.text()
    assert "<path>" in s.text(), s.text()


def test_mcp_prompts_can_connect_before_the_first_turn(ctx):
    """The command starts pending servers when no provider turn has run."""
    write_extras_mcp(ctx)
    s = ctx.spawn(ARQAN_MCP="on")
    s.send("/mcp prompts\r")
    s.wait_text("demo review")
    assert "demo review" in s.text(), s.text()


def test_mcp_prompt_loads_the_text_into_the_composer(ctx):
    """/mcp prompt fills the composer, and the arguments reach the server."""
    write_extras_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp prompt demo review path=src/main.c\r")
    s.wait_text("Review src/main.c please")


def test_mcp_prompt_on_a_server_without_prompts_says_so(ctx):
    """A server with no prompts capability is reported, not called."""
    p = ctx.xdg / "arqan" / "mcp.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(
        json.dumps(
            {"servers": {"demo": {"command": sys.executable, "args": [SERVER]}}}
        )
    )
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp prompts\r")
    s.wait_text("no running MCP server offers a prompt")
