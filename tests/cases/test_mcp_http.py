"""MCP over the streamable HTTP transport."""

import json
import subprocess
import sys
import time
from pathlib import Path

SERVER = str(Path(__file__).resolve().parents[1] / "harness" / "mcpserver.py")


def start_http_server(
    ctx,
    mode="ok",
    sse=False,
    session=False,
    log=None,
    era="legacy",
    expire=False,
    delay_ms=0,
    status=0,
    extra=(),
    env=None,
):
    """Run the fake server over HTTP and answer with its base url."""
    port_file = ctx.tmp / ("port-%d" % len(ctx.helpers))
    argv = [
        sys.executable,
        SERVER,
        "--http",
        "--mode",
        mode,
        "--era",
        era,
        "--port-file",
        str(port_file),
    ]
    if sse:
        argv.append("--sse")
    if session:
        argv.append("--session")
    if expire:
        argv.append("--expire-once")
    if delay_ms:
        argv += ["--delay-ms", str(delay_ms)]
    if status:
        argv += ["--status", str(status)]
    if log:
        argv += ["--log", str(log)]
    argv += list(extra)
    ctx.helpers.append(subprocess.Popen(argv, env=env))

    deadline = time.time() + 10
    while time.time() < deadline:
        if port_file.exists():
            port = port_file.read_text().strip()
            if port:
                return "http://127.0.0.1:%s/mcp" % port
        time.sleep(0.02)
    raise AssertionError("the fake HTTP server did not start")


def write_url_mcp(ctx, url, name="demo", where="user", **extra):
    entry = {"url": url}
    entry.update(extra)
    root = ctx.xdg / "arqan" if where == "user" else ctx.work / ".arqan"
    root.mkdir(parents=True, exist_ok=True)
    (root / "mcp.json").write_text(json.dumps({"servers": {name: entry}}))


def tool_names(request):
    return [t["function"]["name"] for t in request.get("tools", [])]


def test_http_server_registers_and_calls(ctx):
    """A url server answers initialize, tools/list and tools/call as JSON."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, log=log)
    write_url_mcp(ctx, url)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert "demo_echo" in tool_names(ctx.mock.requests[-1])
    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()
    seen = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    accepts = {r.get("accept") for r in seen if r.get("accept")}
    assert accepts == {"application/json, text/event-stream"}, accepts


def test_http_server_over_sse(ctx):
    """A reply streamed as SSE is read to the event that carries the id."""
    url = start_http_server(ctx, sse=True)
    write_url_mcp(ctx, url)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()


def test_http_session_id_is_carried(ctx):
    """The session id from initialize goes back on every later request."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, session=True, log=log)
    write_url_mcp(ctx, url)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    seen = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    posts = [r for r in seen if "raw" in r]
    methods = [json.loads(r["raw"]).get("method") for r in posts]
    assert methods[:2] == ["server/discover", "initialize"], methods
    assert all(r["session"] is None for r in posts[:2]), posts[:2]
    assert all(r["session"] for r in posts[2:]), posts
    versions = {r["protocol"] for r in posts[2:]}
    assert versions == {"2025-06-18"}, versions


def test_http_headers_come_from_the_environment(ctx):
    """A $NAME header is expanded from the environment, not stored in config."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, log=log)
    write_url_mcp(ctx, url, headers={"Authorization": "Bearer $DEMO_TOKEN"})
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", DEMO_TOKEN="s3cret")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    seen = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    assert seen[0]["auth"] == "Bearer s3cret", seen[0]


def test_http_bearer_auth_block(ctx):
    """auth: bearer with a $NAME token becomes an Authorization header."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, log=log)
    write_url_mcp(ctx, url, auth={"type": "bearer", "token": "$DEMO_TOKEN"})
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", DEMO_TOKEN="t0ken")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    seen = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    assert seen[0]["auth"] == "Bearer t0ken", seen[0]


def test_http_bearer_token_must_not_be_literal_in_a_project_file(ctx):
    """A project file with a literal token fails the server, and says why."""
    url = start_http_server(ctx)
    write_url_mcp(ctx, url, where="project", auth={"type": "bearer", "token": "abc"})
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    s.send("/mcp approve demo\r")
    s.wait_text("approved")
    s.submit("again")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("literal token")


def test_http_project_file_may_not_hold_a_literal_header(ctx):
    """A project file with a literal header value loses that header."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, log=log)
    write_url_mcp(ctx, url, where="project", headers={"Authorization": "Bearer abc"})
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp approve demo\r")
    s.wait_text("approved")
    s.submit("again")
    s.wait_turn_done()

    seen = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    assert seen, "the server was never reached"
    assert all(r.get("auth") is None for r in seen), seen


def test_http_plain_url_off_the_machine_is_refused(ctx):
    """http:// is only allowed for a server on this machine."""
    write_url_mcp(ctx, "http://example.com/mcp")
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    assert not [n for n in tool_names(ctx.mock.requests[-1]) if "demo" in n]
    s.send("/mcp\r")
    s.wait_text("failed")
    assert "https" in s.text(), s.text()


def test_http_server_that_is_not_there_fails_once(ctx):
    """A url nothing answers fails the server and leaves the turn alone."""
    write_url_mcp(ctx, "http://127.0.0.1:1/mcp")
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("failed")


def test_http_session_is_closed_on_exit(ctx):
    """Leaving sends DELETE so the server can drop the session."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, session=True, log=log)
    write_url_mcp(ctx, url)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()
    s.send("/quit\r")
    s.wait_exit()

    seen = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    assert any(r.get("method") == "DELETE" for r in seen), seen


def test_http_expired_session_is_started_again(ctx):
    """A 404 for a live session re-initializes once and retries the call."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, session=True, expire=True, log=log)
    write_url_mcp(ctx, url)
    ctx.scenario('tool=demo_echo:{"text":"ping"},final_text=done')
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("say ping")
    s.wait_text("done")
    s.wait_turn_done()

    assert ctx.mock.tool_results() == ["echo: ping"], ctx.mock.tool_results()
    posts = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    methods = [json.loads(p["raw"]).get("method") for p in posts if "raw" in p]
    assert methods.count("initialize") == 2, methods
    assert methods.count("tools/call") == 2, methods


def test_http_bearer_auth_refuses_a_literal_token(ctx):
    """A user config also keeps bearer tokens out of mcp.json."""
    url = start_http_server(ctx)
    write_url_mcp(ctx, url, auth={"type": "bearer", "token": "abc"})
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("literal token")


def test_http_entry_may_not_also_name_a_command(ctx):
    """An entry selects one transport rather than silently preferring url."""
    url = start_http_server(ctx)
    write_url_mcp(ctx, url, command=sys.executable)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("both command and url")


def test_http_header_rejects_a_newline_from_the_environment(ctx):
    """An expanded header cannot add a second HTTP header."""
    url = start_http_server(ctx)
    write_url_mcp(ctx, url, headers={"X-Demo": "$DEMO_HEADER"})
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", DEMO_HEADER="yes\r\nX-Injected: yes")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("control character")


def test_http_redirect_is_reported_not_followed(ctx):
    """Custom MCP headers never follow a redirect to another URL."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, status=302, log=log)
    write_url_mcp(ctx, url)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("redirected")
    seen = [json.loads(line) for line in log.read_text().splitlines()]
    assert len(seen) == 2, seen


def test_http_unauthorized_server_shows_its_challenge(ctx):
    """A 401 keeps the WWW-Authenticate value in the server error."""
    url = start_http_server(ctx, status=401)
    write_url_mcp(ctx, url)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("server answered 401")
    assert 'realm="fake"' in s.text(), s.text()


def test_http_timeout_stops_a_slow_server(ctx):
    """The MCP timeout also bounds an HTTP exchange."""
    url = start_http_server(ctx, delay_ms=500)
    write_url_mcp(ctx, url)
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", ARQAN_MCP_TIMEOUT_MS="100")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("failed")


def test_http_project_header_with_a_lone_dollar_is_still_literal(ctx):
    """A '$' that names no variable does not make a secret acceptable."""
    log = ctx.tmp / "http.log"
    url = start_http_server(ctx, log=log)
    write_url_mcp(ctx, url, where="project",
                  headers={"Authorization": "Bearer abc$"})
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp approve demo\r")
    s.wait_text("approved")
    s.submit("again")
    s.wait_turn_done()

    seen = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
    assert seen, "the server was never reached"
    assert all(r.get("auth") is None for r in seen), seen


def test_http_approval_prompt_names_the_headers_it_would_send(ctx):
    """The user approving a project server sees which headers it sets."""
    url = start_http_server(ctx)
    write_url_mcp(ctx, url, where="project",
                  headers={"X-Team": "$TEAM_ID"})
    ctx.scenario("text=zebra")
    s = ctx.spawn(ARQAN_MCP="on", TEAM_ID="t1")
    s.submit("hello")
    s.wait_text("zebra")
    s.wait_turn_done()

    s.send("/mcp\r")
    s.wait_text("X-Team")
