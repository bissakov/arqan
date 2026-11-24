"""MCP resources as a tool, and MCP prompts through /mcp."""

import json
import re
import sys
from pathlib import Path

SERVER = str(Path(__file__).resolve().parents[1] / "harness" / "mcpserver.py")
LIST = "ListMcpResourcesTool"
READ = "ReadMcpResourceTool"


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


def test_a_server_with_resources_gets_the_resource_tools(ctx):
    """The capability adds a list and a read tool, and the list shows uris."""
    write_extras_mcp(ctx)
    ctx.scenario(f"tool={LIST}:{{}},final_text=listed")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("what is there")
    s.wait_text("listed")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert LIST in names and READ in names, names
    out = ctx.mock.tool_results()[0]
    assert "demo://notes" in out, out
    assert "text/plain" in out, out


def test_no_mcp_tool_name_starts_with_mcp_and_one_underscore(ctx):
    """Anthropic bills a subscription request with such a name as extra usage."""
    write_extras_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "mcp__demo__echo" in names, names
    assert not [n for n in names if re.match(r"mcp_[A-Za-z0-9]", n)], names


def test_a_server_that_connects_prints_nothing_in_the_transcript(ctx):
    """/mcp shows the status and tool count; a connect adds no info line."""
    write_extras_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert "mcp__demo__echo" in tool_names(ctx.mock.requests[-1])
    assert "[info:" not in s.text(), s.text()


def test_listing_resources_refuses_a_uri(ctx):
    """A uri belongs to the read tool, and the error names it."""
    write_extras_mcp(ctx)
    ctx.scenario(
        f'tool={LIST}:{{"server":"demo","uri":"demo://notes"}},final_text=done'
    )
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("list")
    s.wait_text("done")
    s.wait_turn_done()

    out = ctx.mock.tool_results()[0]
    assert READ in out and "buy milk" not in out, out


def test_reading_a_resource_needs_a_uri(ctx):
    """The read tool does not fall back to listing."""
    write_extras_mcp(ctx)
    ctx.scenario(f'tool={READ}:{{"server":"demo"}},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("read")
    s.wait_text("done")
    s.wait_turn_done()

    out = ctx.mock.tool_results()[0]
    assert "server and uri are both required" in out, out


def test_reading_a_resource_returns_its_text(ctx):
    """A uri reads that one resource."""
    write_extras_mcp(ctx)
    ctx.scenario(
        f'tool={READ}:{{"server":"demo","uri":"demo://notes"}},final_text=read'
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
        f'tool={READ}:{{"server":"demo","uri":"demo://logo"}},final_text=read'
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

    names = tool_names(ctx.mock.requests[-1])
    assert LIST not in names and READ not in names, names


def test_resource_tools_can_be_disabled_before_the_server_connects(ctx):
    """disable_tools accepts the dynamic resource tool names at startup."""
    write_extras_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_DISABLE_TOOLS=f"{LIST},{READ}")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert LIST not in names and READ not in names, names


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
