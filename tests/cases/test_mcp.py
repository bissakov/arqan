"""MCP servers: configuration, connecting, and calling a tool."""

import json
import sys
from pathlib import Path

SERVER = str(Path(__file__).resolve().parents[1] / "harness" / "mcpserver.py")


def write_mcp(ctx, name="demo", mode="ok", **extra):
    """Put one fake server in the user config directory's mcp.json."""
    entry = {"command": sys.executable, "args": [SERVER, "--mode", mode]}
    entry.update(extra)
    p = ctx.xdg / "arqan" / "mcp.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"servers": {name: entry}}, indent=2))
    return p


def write_project_mcp(ctx, name="demo", mode="ok", **extra):
    """Put one fake server in the project's .arqan/mcp.json."""
    entry = {"command": sys.executable, "args": [SERVER, "--mode", mode]}
    entry.update(extra)
    p = ctx.work / ".arqan" / "mcp.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"servers": {name: entry}}, indent=2))
    return p


def tool_names(request):
    return [t["function"]["name"] for t in request.get("tools", [])]


def test_mcp_is_off_by_default(ctx):
    """With mcp unset, no server starts and no tool is offered."""
    write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn()
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert not [n for n in names if n.startswith("demo_")], names
    assert "MCP" not in ctx.mock.requests[-1]["messages"][0]["content"]


def test_mcp_tools_register_on_the_first_turn(ctx):
    """A configured server starts on the first turn and offers its tools."""
    write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_SYSTEM_PROMPT=None)
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "demo_echo" in names, names
    assert "demo_boom" in names, names
    system = ctx.mock.requests[-1]["messages"][0]["content"]
    assert "<server>_<tool>" in system, system


def test_mcp_tool_call_round_trip(ctx):
    """The model calls an MCP tool and its text comes back as the result."""
    write_mcp(ctx)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()
    assert "demo_echo" in s.text(), s.text()


def test_mcp_error_result_is_reported(ctx):
    """isError comes back as a failed tool call, not as content."""
    write_mcp(ctx)
    ctx.scenario("tool=demo_boom:{},final_text=done")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("break it")
    s.wait_text("done")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert results and "the server refused" in results[0], results


def test_mcp_large_result_is_paged(ctx):
    """A result over the tool limit is cut and the rest goes to a spill file."""
    write_mcp(ctx)
    ctx.scenario("tool=demo_big:{},final_text=done")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("give me a lot")
    s.wait_text("done")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[0]
    assert len(result) < 40000, len(result)
    assert "showing the first" in result, result[-400:]
    assert "full output" in result, result[-400:]


def test_mcp_image_block_is_named_not_passed(ctx):
    """An image block is described in the result; no image reaches the model."""
    write_mcp(ctx)
    ctx.scenario("tool=demo_picture:{},final_text=done")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("show me")
    s.wait_text("done")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[0]
    assert "here it is" in result, result
    assert "image" in result and "not passed on" in result, result
    assert "aGk=" not in json.dumps(ctx.mock.requests[-1])


def test_mcp_bad_tool_definitions_are_dropped(ctx):
    """A bad name, a non-object schema and an oversized schema are refused."""
    write_mcp(ctx, mode="bad")
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "demo_echo" in names, names
    assert [n for n in names if n.startswith("demo_")] == ["demo_echo"], names


def test_mcp_tool_limit_holds(ctx):
    """A server offering more tools than the cap fills it and stops there."""
    write_mcp(ctx, mode="flood")
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = [n for n in tool_names(ctx.mock.requests[-1]) if n.startswith("demo_")]
    assert len(names) == 32, len(names)


def test_mcp_dead_server_is_reported_once(ctx):
    """A server that exits at once fails, offers nothing and is not retried."""
    write_mcp(ctx, mode="crash")
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    s.submit("again")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert not [n for n in names if n.startswith("demo_")], names
    assert len(ctx.mock.requests) == 2, len(ctx.mock.requests)
    s.send("/mcp\r")
    s.wait_text("failed")
    assert "demo" in s.text(), s.text()


def test_mcp_command_lists_servers(ctx):
    """/mcp names the server, its state and how many tools it brought."""
    write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("MCP servers")
    text = s.text()
    assert "demo" in text, text
    assert "ready" in text, text
    assert "5 tools" in text, text


def test_mcp_command_says_when_mcp_is_off(ctx):
    """/mcp is not offered when mcp is off, and says so if typed."""
    write_mcp(ctx)
    s = ctx.spawn()
    s.send("/mcp\r")
    s.wait_text("MCP is off")


def test_mcp_tool_can_be_disabled(ctx):
    """disable_tools takes the prefixed name and the tool never appears."""
    write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_DISABLE_TOOLS="demo_echo")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "demo_echo" not in names, names
    assert "demo_boom" in names, names


def test_mcp_env_and_cwd_reach_the_child(ctx):
    """env entries and cwd are passed to the server process."""
    log = ctx.tmp / "calls.log"
    write_mcp(ctx)
    entry = {
        "command": sys.executable,
        "args": [SERVER, "--mode", "ok", "--log", str(log)],
        "env": {"ARQAN_FAKE_TOKEN": "abc"},
        "cwd": str(ctx.work),
    }
    (ctx.xdg / "arqan" / "mcp.json").write_text(
        json.dumps({"servers": {"demo": entry}})
    )
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    lines = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    methods = [m.get("method") for m in lines]
    assert methods[0] == "server/discover", methods
    assert methods[1] == "initialize", methods
    assert "notifications/initialized" in methods, methods
    assert methods[-1] == "tools/call", methods


def test_mcp_call_is_refused_when_the_server_dies(ctx):
    """A server that exits during a call fails the call and stays down."""
    write_mcp(ctx, mode="die-on-call")
    ctx.scenario("tool=demo_echo:{},final_text=done")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert results and "exited" in results[0], results


def test_mcp_call_denied_at_the_prompt(ctx):
    """Under ask, a no stops the call and the model is told."""
    write_mcp(ctx)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_PERMISSIONS="ask")
    s.submit("say ping")
    s.wait_text("allow MCP tool?")
    s.send("\x1b[B\x1b[B\r")
    s.wait_text("done")
    s.wait_turn_done()

    results = ctx.mock.tool_results()
    assert results and "did not approve" in results[0], results


def test_mcp_call_approved_at_the_prompt(ctx):
    """Under ask, a yes runs the call and its text comes back."""
    write_mcp(ctx)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_PERMISSIONS="ask")
    s.submit("say ping")
    s.wait_text("allow MCP tool?")
    s.send("\r")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()


def test_mcp_tools_are_absent_in_plan_mode(ctx):
    """Plan mode offers no MCP tool, since a server may do anything."""
    write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_MODE="plan")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert not [n for n in names if n.startswith("demo_")], names


def test_mcp_server_added_mid_session_is_picked_up(ctx):
    """A server written to mcp.json after the session started still runs."""
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    assert not [n for n in tool_names(ctx.mock.requests[-1]) if "demo" in n]

    write_mcp(ctx)
    s.submit("again")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "demo_echo" in names, names


def test_mcp_one_shot_run_uses_a_server(ctx):
    """A non-interactive run starts the server and calls its tool."""
    write_mcp(ctx)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    out = ctx.run_cli("-p", "say ping", ARQAN_MCP="on")
    assert "done" in out.stdout, out.stdout
    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()


# ---- project servers ------------------------------------------------------


def test_project_server_waits_for_approval(ctx):
    """A server from a project file offers nothing until it is approved."""
    write_project_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert not [n for n in names if n.startswith("demo_")], names
    s.send("/mcp\r")
    s.wait_text("waiting for approval")
    assert "mcpserver.py" in s.text(), s.text()


def test_project_server_runs_once_approved(ctx):
    """Approval is recorded in the state file and the server starts."""
    write_project_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.send("/mcp approve demo\r")
    s.wait_text("approved")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert "demo_echo" in tool_names(ctx.mock.requests[-1])
    state = ctx.settings(ctx.state_file())
    section = [k for k in state if k.startswith("mcp.")]
    assert section, state
    assert "demo" in state[section[0]], state


def test_approval_is_remembered_next_session(ctx):
    """A second session starts the same project server without asking."""
    write_project_mcp(ctx)
    ctx.scenario("text=zebra")
    first = ctx.spawn(ARQAN_MCP="on")
    first.send("/mcp approve demo\r")
    first.wait_text("approved")
    first.close()

    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    assert "demo_echo" in tool_names(ctx.mock.requests[-1])


def test_edited_project_command_asks_again(ctx):
    """Approval is bound to the command, so an edit needs a new approval."""
    write_project_mcp(ctx)
    ctx.scenario("text=zebra")
    first = ctx.spawn(ARQAN_MCP="on")
    first.send("/mcp approve demo\r")
    first.wait_text("approved")
    first.close()

    write_project_mcp(ctx, mode="bad")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert not [n for n in tool_names(ctx.mock.requests[-1]) if "demo" in n]
    s.send("/mcp\r")
    s.wait_text("waiting for approval")


def test_edited_project_environment_asks_again(ctx):
    """Approval covers the full definition, including environment mappings."""
    write_project_mcp(ctx, env={"DEMO_ONE": "$DEMO_ONE"})
    ctx.scenario("text=zebra")
    first = ctx.spawn(ARQAN_MCP="on", DEMO_ONE="one", DEMO_TWO="two")
    first.send("/mcp approve demo\r")
    first.wait_text("approved")
    first.close()

    write_project_mcp(ctx, env={"DEMO_TWO": "$DEMO_TWO"})
    s = ctx.spawn(ARQAN_MCP="on", DEMO_ONE="one", DEMO_TWO="two")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert not [n for n in tool_names(ctx.mock.requests[-1]) if "demo" in n]
    s.send("/mcp\r")
    s.wait_text("waiting for approval")


def test_reject_forgets_the_approval(ctx):
    """Reject takes the server down and drops the recorded approval."""
    write_project_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.send("/mcp approve demo\r")
    s.wait_text("approved")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    assert "demo_echo" in tool_names(ctx.mock.requests[-1])

    s.send("/mcp reject demo\r")
    s.wait_text("rejected")
    s.submit("again")
    s.wait_turn_done()
    assert not [n for n in tool_names(ctx.mock.requests[-1]) if "demo" in n]

    state = ctx.settings(ctx.state_file())
    for section, rows in state.items():
        if section.startswith("mcp."):
            assert "demo" not in rows, rows


def test_project_literal_env_is_refused(ctx):
    """A project file may not plant an environment value."""
    log = ctx.tmp / "calls.log"
    write_project_mcp(ctx, env={"ARQAN_FAKE_TOKEN": "secret"})
    s = ctx.spawn(ARQAN_MCP="on")
    s.send("/mcp approve demo\r")
    s.wait_text("approved")
    ctx.scenario("text=zebra")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert "demo_echo" in tool_names(ctx.mock.requests[-1])
    assert not log.exists()


def test_user_config_wins_over_a_project_file(ctx):
    """The same name in both files takes the user's definition."""
    write_mcp(ctx)
    write_project_mcp(ctx, mode="crash")
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert "demo_echo" in tool_names(ctx.mock.requests[-1])


def test_mcp_restart_reloads_the_tool_list(ctx):
    """Restart stops the child, forgets its tools and asks again."""
    write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    assert "demo_echo" in tool_names(ctx.mock.requests[-1])

    s.send("/mcp restart demo\r")
    s.wait_text("restarted with 5 tools")
    s.submit("again")
    s.wait_turn_done()

    names = [n for n in tool_names(ctx.mock.requests[-1]) if n.startswith("demo_")]
    assert len(names) == 5, names


def test_mcp_disable_takes_the_tools_away(ctx):
    """Disable ends the server for the session and unregisters its tools."""
    write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    assert "demo_echo" in tool_names(ctx.mock.requests[-1])

    s.send("/mcp disable demo\r")
    s.wait_text("off for this session")
    s.submit("again")
    s.wait_turn_done()
    assert not [n for n in tool_names(ctx.mock.requests[-1]) if "demo" in n]


def test_mcp_unknown_verb_is_reported(ctx):
    """A typo says what /mcp takes."""
    write_mcp(ctx)
    s = ctx.spawn(ARQAN_MCP="on")
    s.send("/mcp frobnicate demo\r")
    s.wait_text("approve, reject, restart or disable")


# ---- process hygiene and server messages ----------------------------------


def test_a_second_server_does_not_inherit_the_first_ones_pipes(ctx):
    """Each server holds only its own stdin, stdout and stderr."""
    p = ctx.xdg / "arqan" / "mcp.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    reports = [ctx.tmp / "fds-one.json", ctx.tmp / "fds-two.json"]
    servers = {
        name: {
            "command": sys.executable,
            "args": [SERVER, "--report-fds", str(report)],
        }
        for name, report in zip(["one", "two"], reports)
    }
    p.write_text(json.dumps({"servers": servers}))
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    for report in reports:
        seen = json.loads(report.read_text())
        extra = {fd: t for fd, t in seen.items() if int(fd) > 2 and "pipe:" in t}
        assert not extra, (report.name, seen)


def test_a_request_from_the_server_is_answered_not_taken_as_the_reply(ctx):
    """A ping that reuses the call's id gets a reply; the real answer wins."""
    log = ctx.tmp / "mcp.log"
    write_mcp(ctx, mode="ping-first", args=[SERVER, "--mode", "ping-first",
                                            "--log", str(log)])
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()
    lines = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    answers = [m for m in lines if "method" not in m]
    assert answers and answers[0].get("result") == {}, lines


def test_an_interrupted_call_tells_the_server(ctx):
    """Ctrl-C during a call sends notifications/cancelled for that request."""
    log = ctx.tmp / "mcp.log"
    write_mcp(ctx, args=[SERVER, "--log", str(log)])
    ctx.scenario('tool=demo_slow:{"ms":1500},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("wait")
    s.wait_text("demo_slow")
    s.key("ctrl-c")
    s.wait_turn_done()

    def cancelled(_term):
        if not log.exists():
            return None
        lines = [json.loads(l) for l in log.read_text().splitlines() if l]
        calls = [m for m in lines if m.get("method") == "tools/call"]
        stops = [m for m in lines if m.get("method") == "notifications/cancelled"]
        return calls and stops and \
            stops[0]["params"]["requestId"] == calls[0]["id"]

    s.wait_for(cancelled, "the cancel notification", timeout=10)


def test_resource_blocks_in_a_result_are_passed_on_as_text(ctx):
    """An embedded text resource is kept and a link is named, not dropped."""
    write_mcp(ctx, mode="links", args=[SERVER, "--mode", "links", "--extras"])
    ctx.scenario("tool=demo_linked:{},final_text=done")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("link")
    s.wait_text("done")
    s.wait_turn_done()

    result = ctx.mock.tool_results()[0]
    assert "demo://notes" in result, result
    assert "mcp_read can read it" in result, result
    assert "inline words" in result, result
    assert "not passed on" not in result, result


# ---- editing mcp.json while the session runs ------------------------------


def test_an_edited_entry_is_restarted_with_its_new_tools(ctx):
    """Changing a server's definition takes down its old tools."""
    p = write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    assert "demo_echo" in tool_names(ctx.mock.requests[-1])

    entry = {"command": sys.executable, "args": [SERVER, "--mode", "headers"]}
    p.write_text(json.dumps({"servers": {"demo": entry}}, indent=4))
    s.submit("again")
    s.wait_turn_done()

    names = [n for n in tool_names(ctx.mock.requests[-1])
             if n.startswith("demo_")]
    assert names == ["demo_headed"], names


def test_a_removed_entry_takes_its_tools_away(ctx):
    """A server deleted from mcp.json stops and offers nothing more."""
    p = write_mcp(ctx)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    p.write_text(json.dumps({"servers": {}}))
    s.submit("again")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert not [n for n in names if n.startswith("demo_")], names
    s.send("/mcp\r")
    s.wait_text("no longer in mcp.json")


def test_an_untouched_entry_keeps_running_across_an_edit(ctx):
    """Adding a server leaves a running one alone."""
    log = ctx.tmp / "mcp.log"
    p = write_mcp(ctx, args=[SERVER, "--log", str(log)])
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    body = json.loads(p.read_text())
    body["servers"]["other"] = {"command": sys.executable, "args": [SERVER]}
    p.write_text(json.dumps(body, indent=4))
    s.submit("again")
    s.wait_turn_done()

    names = tool_names(ctx.mock.requests[-1])
    assert "demo_echo" in names and "other_echo" in names, names
    lines = [json.loads(l) for l in log.read_text().splitlines() if l]
    hellos = [m for m in lines if m.get("method") == "initialize"]
    assert len(hellos) == 1, lines


def test_telemetry_keeps_server_names_and_messages_out(ctx):
    """A call is recorded as an MCP call; names and server text are not."""
    from .test_telemetry import body, events

    write_mcp(ctx, name="secretname", mode="bad")
    ctx.scenario('tool=secretname_echo:{"secretkey":"x"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.settings_toggle("Telemetry")
    s.submit("go")
    s.wait_text("done")
    s.wait_turn_done()

    text = body(ctx)
    assert "secretname" not in text, text
    assert "secretkey" not in text, text
    assert "not a name" not in text, text
    tool = [e for e in events(ctx) if e["ev"] == "tool"][-1]
    assert tool["name"] == "mcp", tool
    connect = [e for e in events(ctx) if e["ev"] == "mcp_connect"]
    assert connect and connect[0]["ok"] is True, connect
    assert connect[0]["transport"] == "stdio", connect
