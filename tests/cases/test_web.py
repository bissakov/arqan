"""Built-in public web search and bounded page fetching."""

import json


def web_env(ctx):
    return {
        "ARQAN_TEST_WEB_ALLOW_PRIVATE": "1",
        "ARQAN_TEST_WEB_SEARCH_URL": f"{ctx.mock.origin}/web/search?q=",
    }


def run_web(ctx, name, args, reply="web done"):
    ctx.scenario(f"tool={name}:{json.dumps(args)},final_text={reply.replace(' ', '+')}")
    s = ctx.spawn(rows=40, **web_env(ctx))
    s.submit(f"use {name}")
    s.wait_text(reply)
    s.wait_turn_done()
    return s, ctx.mock.tool_results()[-1]


def run_web_fallback(ctx, args, reply="web done"):
    """Search with both the lite and HTML endpoints configured."""
    ctx.scenario(
        f"tool=internet_search:{json.dumps(args)},final_text={reply.replace(' ', '+')}"
    )
    env = dict(web_env(ctx))
    env["ARQAN_TEST_WEB_SEARCH_FALLBACK_URL"] = f"{ctx.mock.origin}/web/search-html?q="
    env["ARQAN_TEST_WEB_SEARCH_INTERVAL_MS"] = "0"
    s = ctx.spawn(rows=40, **env)
    s.submit("use internet_search")
    s.wait_text(reply)
    s.wait_turn_done()
    return s, ctx.mock.tool_results()[-1]


def run_web_backend(ctx, backend, args, reply="web done", **extra):
    """Search through one named engine, pointed at the fixture server."""
    ctx.scenario(
        f"tool=internet_search:{json.dumps(args)},final_text={reply.replace(' ', '+')}"
    )
    env = {
        "ARQAN_TEST_WEB_ALLOW_PRIVATE": "1",
        "ARQAN_TEST_WEB_SEARCH_INTERVAL_MS": "0",
        "ARQAN_SEARCH_BACKEND": backend,
        **extra,
    }
    s = ctx.spawn(rows=40, **env)
    s.submit("use internet_search")
    s.wait_text(reply)
    s.wait_turn_done()
    return s, ctx.mock.tool_results()[-1]



def test_web_tool_schemas_are_offered_in_build_mode(ctx):
    """Both public schemas carry their required argument and limits."""
    ctx.scenario("text=ok")
    s = ctx.spawn()
    s.submit("hello")
    s.wait_turn_done()
    tools = {t["function"]["name"]: t["function"] for t in ctx.mock.requests[-1]["tools"]}
    assert "web_search" not in tools and "web_fetch" not in tools
    assert tools["internet_search"]["parameters"]["required"] == ["query"]
    assert tools["page_fetch"]["parameters"]["required"] == ["url"]
    assert "untrusted" in tools["page_fetch"]["description"]


def test_web_tool_schemas_are_offered_to_anthropic_in_plan_mode(ctx):
    """The same two read-only tools use Anthropic's flat schema shape."""
    ctx.scenario("text=ok")
    s = ctx.spawn(ARQAN_API="anthropic")
    s.key("shift-tab")
    s.wait_for(lambda t: s.status_field(2) == "plan", "plan mode")
    s.submit("inspect the web")
    s.wait_turn_done()
    tools = {t["name"]: t for t in ctx.mock.requests[-1]["tools"]}
    assert tools["internet_search"]["input_schema"]["required"] == ["query"]
    assert tools["page_fetch"]["input_schema"]["required"] == ["url"]


def test_web_search_round_trips_normalized_results(ctx):
    """Titles, snippets, entities, operators, redirects, and duplicates normalize."""
    query = 'site:example.com "quoted phrase" -excluded'
    s, result = run_web(ctx, "internet_search", {"query": query})
    assert result.startswith("External search results (untrusted): 2"), result
    assert "First & best" in result and f"snippet for {query}." in result, result
    assert "https://example.com/first?a=1&b=two" in result, result
    assert "http://example.org/two" in result and "Second result Ω" in result, result
    assert "Duplicate" not in result and "Malformed" not in result, result
    assert "◆  internet_search " + query in s.text(), s.text()


def test_web_search_limit_and_empty_page(ctx):
    """A requested limit is honored and an explicit empty layout is legitimate."""
    _, one = run_web(ctx, "internet_search", {"query": "ordinary", "limit": 1})
    assert one.startswith("External search results (untrusted): 1"), one
    assert "Second result" not in one

    ctx.mock.reset()
    _, empty = run_web(ctx, "internet_search", {"query": "empty"})
    assert empty == "External search results (untrusted): 0", empty


def test_web_search_refuses_challenges_and_changed_layouts(ctx):
    """Blocking and markup changes are errors, never silent empty searches."""
    _, challenged = run_web(ctx, "internet_search", {"query": "challenge"})
    assert challenged.startswith("ERROR: ") and "challenge page" in challenged

    ctx.mock.reset()
    _, changed = run_web(ctx, "internet_search", {"query": "changed"})
    assert changed.startswith("ERROR: ") and "layout was not recognized" in changed


def test_web_search_refuses_service_statuses(ctx):
    """202, 403, and 429 are refusals and are not retried or parsed."""
    for status in (202, 403, 429):
        ctx.mock.reset()
        _, result = run_web(ctx, "internet_search", {"query": f"status{status}"})
        assert result.startswith("ERROR: ") and f"HTTP {status}" in result, result


def test_web_search_falls_back_to_the_html_endpoint(ctx):
    """A refusal from the lite endpoint retries once on the HTML layout."""
    _, result = run_web_fallback(ctx, {"query": "liteblocked"})
    assert result.startswith("External search results (untrusted): 2"), result
    assert "First & best" in result and "snippet for liteblocked." in result, result
    assert "https://example.com/first?a=1&b=two" in result, result
    assert "http://example.org/two" in result and "Second result Ω" in result, result
    assert "example.com</a>" not in result, result
    assert len(ctx.mock.web_request_times) == 2, ctx.mock.web_request_times


def test_web_search_pauses_only_when_every_endpoint_refuses(ctx):
    """Both endpoints blocking is a refusal that reports the first status."""
    _, result = run_web_fallback(ctx, {"query": "status403"})
    assert result.startswith("ERROR: ") and "HTTP 403" in result, result
    assert "paused" in result and "do not retry" in result, result
    assert len(ctx.mock.web_request_times) == 2, ctx.mock.web_request_times


def test_web_search_names_every_endpoint_it_tried(ctx):
    """A refusal must not read as the only attempt: the fallback is reported too."""
    _, result = run_web_fallback(ctx, {"query": "bothfail"})
    assert result.startswith("ERROR: "), result
    assert "lite" in result and "HTTP 202" in result, result
    assert "html" in result and "layout was not recognized" in result, result
    assert "paused" in result and "do not retry" in result, result
    assert len(ctx.mock.web_request_times) == 2, ctx.mock.web_request_times


def test_web_search_reads_the_brave_layout(ctx):
    """Brave's hashed class names are ignored; its stable tokens are not."""
    _, result = run_web_backend(
        ctx, "brave", {"query": "ordinary"},
        ARQAN_SEARCH_ENDPOINT=f"{ctx.mock.origin}/web/brave",
    )
    assert result.startswith("External search results (untrusted): 2"), result
    assert "First & best" in result and "snippet for ordinary." in result, result
    assert "https://example.com/first?a=1&b=two" in result, result
    # The title element wins: the anchor also holds the site name and URL.
    assert "example.com ›" not in result, result
    # A block with no title is not a result.
    assert "ads.example" not in result, result


def test_web_search_reads_keyed_json_engines(ctx):
    """Each keyed engine's own field names, key placement, and empty answer."""
    _, searx = run_web_backend(
        ctx, "searxng", {"query": "ordinary"},
        ARQAN_SEARCH_ENDPOINT=f"{ctx.mock.origin}/web/searxng",
    )
    assert searx.startswith("External search results (untrusted): 2"), searx
    assert "First & best" in searx and "second snippet" in searx, searx
    assert "ftp://example.net" not in searx and "Duplicate" not in searx, searx
    assert ctx.mock.web_calls[-1]["query"]["format"] == ["json"]

    ctx.mock.reset()
    _, brave = run_web_backend(
        ctx, "brave_api", {"query": "ordinary"},
        ARQAN_SEARCH_ENDPOINT=f"{ctx.mock.origin}/web/braveapi",
        ARQAN_SEARCH_API_KEY="secret-token",
    )
    assert brave.startswith("External search results (untrusted): 2"), brave
    assert "A nested snippet for ordinary." in brave, brave
    call = ctx.mock.web_calls[-1]
    assert call["token"] == "secret-token", call
    assert "secret-token" not in call["query"].get("q", [""])[0], call

    ctx.mock.reset()
    _, google = run_web_backend(
        ctx, "google", {"query": "ordinary"},
        ARQAN_SEARCH_ENDPOINT=f"{ctx.mock.origin}/web/google",
        ARQAN_SEARCH_API_KEY="secret-key",
        ARQAN_SEARCH_ENGINE_ID="cx-1234",
    )
    assert google.startswith("External search results (untrusted): 2"), google
    call = ctx.mock.web_calls[-1]
    assert call["query"]["key"] == ["secret-key"], call
    assert call["query"]["cx"] == ["cx-1234"], call

    ctx.mock.reset()
    _, empty = run_web_backend(
        ctx, "searxng", {"query": "empty"},
        ARQAN_SEARCH_ENDPOINT=f"{ctx.mock.origin}/web/searxng",
    )
    assert empty == "External search results (untrusted): 0", empty


def test_web_search_keeps_the_key_out_of_a_failure(ctx):
    """A keyed engine carries its key in the URL, so no failure may quote it."""
    _, result = run_web_backend(
        ctx, "google", {"query": "ordinary"},
        ARQAN_SEARCH_ENDPOINT="http://127.0.0.1:1",
        ARQAN_SEARCH_API_KEY="secret-key",
        ARQAN_SEARCH_ENGINE_ID="cx-1234",
    )
    assert result.startswith("ERROR: ") and "google" in result, result
    assert "secret-key" not in result and "cx-1234" not in result, result


def test_web_search_reports_a_changed_json_engine(ctx):
    """A keyed engine that stops returning its array is an error, not zero hits."""
    _, result = run_web_backend(
        ctx, "searxng", {"query": "changed"},
        ARQAN_SEARCH_ENDPOINT=f"{ctx.mock.origin}/web/searxng",
    )
    assert result.startswith("ERROR: ") and "searxng" in result, result
    assert "no results array" in result or "carried no results" in result, result


def test_web_search_falls_back_when_a_keyed_engine_is_unconfigured(ctx):
    """A backend missing its key searches the keyless engines instead."""
    _, result = run_web_backend(
        ctx, "brave_api", {"query": "ordinary"},
        **{"ARQAN_TEST_WEB_SEARCH_URL": f"{ctx.mock.origin}/web/search?q="},
    )
    assert result.startswith("External search results (untrusted): 2"), result
    assert ctx.mock.web_calls[-1]["path"] == "/web/search", ctx.mock.web_calls


def test_web_search_treats_blank_settings_as_unset(ctx):
    """Empty and whitespace-only settings are unset, not configured values.

    An unset setting reaches web_search_init as a Str with a NULL pointer, so
    duplicating one used to copy from NULL and trap under the sanitizers.
    """
    _, result = run_web_backend(
        ctx, "brave_api", {"query": "ordinary"},
        ARQAN_SEARCH_API_KEY="",
        ARQAN_SEARCH_ENDPOINT="   ",
        ARQAN_SEARCH_ENGINE_ID="",
        **{"ARQAN_TEST_WEB_SEARCH_URL": f"{ctx.mock.origin}/web/search?q="},
    )
    assert result.startswith("External search results (untrusted): 2"), result
    call = ctx.mock.web_calls[-1]
    assert call["path"] == "/web/search", ctx.mock.web_calls
    assert call.get("token") in (None, ""), call


def test_web_search_quarantines_only_the_refusing_engine(ctx):
    """A blocked endpoint is skipped next time; the one that answered is not."""
    args = json.dumps({"query": "liteblocked"})
    ctx.scenario(f"tool=internet_search:{args},tool_rounds=2,final_text=done")
    env = dict(web_env(ctx))
    env["ARQAN_TEST_WEB_SEARCH_FALLBACK_URL"] = f"{ctx.mock.origin}/web/search-html?q="
    env["ARQAN_TEST_WEB_SEARCH_INTERVAL_MS"] = "0"
    s = ctx.spawn(rows=40, **env)
    s.submit("search twice")
    s.wait_text("done")
    s.wait_turn_done()
    paths = [call["path"] for call in ctx.mock.web_calls]
    assert paths == ["/web/search", "/web/search-html", "/web/search-html"], paths
    for result in ctx.mock.tool_results()[-2:]:
        assert result.startswith("External search results (untrusted): 2"), result


def test_web_search_spaces_repeated_requests(ctx):
    """Consecutive successful searches cannot hit the service in a burst."""
    args = json.dumps({"query": "ordinary"})
    ctx.scenario(
        f"tool=internet_search:{args},tool_rounds=2,final_text=done"
    )
    s = ctx.spawn(
        rows=40,
        ARQAN_TEST_WEB_SEARCH_INTERVAL_MS="400",
        **web_env(ctx),
    )
    s.submit("search twice")
    s.wait_text("done")
    s.wait_turn_done()
    times = ctx.mock.web_request_times
    assert len(times) == 2, times
    # Arrival times, not client start times: the first request pays connection
    # setup the second does not, so the observed gap runs under the interval.
    assert times[1] - times[0] >= 0.2, times


def test_web_search_quarantines_after_a_refusal(ctx):
    """A challenge response prevents later calls from reinforcing the block."""
    args = json.dumps({"query": "status202"})
    ctx.scenario(
        f"tool=internet_search:{args},tool_rounds=2,final_text=done"
    )
    s = ctx.spawn(rows=40, **web_env(ctx))
    s.submit("search twice")
    s.wait_text("done")
    s.wait_turn_done()
    assert len(ctx.mock.web_request_times) == 1, ctx.mock.web_request_times
    results = ctx.mock.tool_results()
    assert "HTTP 202" in results[-2] and "do not retry" in results[-2], results
    assert "paused" in results[-1] and "do not retry" in results[-1], results


def test_web_fetch_extracts_html_and_resolves_links(ctx):
    """The qualifying main wins, boilerplate leaves, and relative links resolve."""
    url = f"{ctx.mock.origin}/web/page"
    s, result = run_web(ctx, "page_fetch", {"url": url})
    assert result.startswith("External page (untrusted): Fixture & title"), result
    assert f"Final URL: {url}" in result, result
    assert "# Heading" in result and "- First item" in result, result
    assert "Name | Value" in result and "```" in result and "go();" in result, result
    assert f"{ctx.mock.origin}/web/base/child?q=one&x=two" in result, result
    assert "hidden navigation" not in result and "hidden footer" not in result
    assert "fallback" not in result
    # Without a User-Agent a service answers with a challenge or a stripped page.
    assert "Firefox/" in (ctx.mock.web_user_agents[-1] or ""), ctx.mock.web_user_agents
    assert f"◆  page_fetch {url}" in s.text(), s.text()


def test_web_fetch_reports_redirect_final_url(ctx):
    """A public redirect chain is followed and its effective URL is shown."""
    start = f"{ctx.mock.origin}/web/redirect"
    _, result = run_web(ctx, "page_fetch", {"url": start})
    assert f"Final URL: {ctx.mock.origin}/web/page" in result, result


def test_web_fetch_parses_malformed_html_and_uses_article_fallback(ctx):
    """Permissive parsing skips a short main and selects a qualifying article."""
    _, result = run_web(ctx, "page_fetch", {"url": f"{ctx.mock.origin}/web/malformed"})
    assert "Broken & useful" in result and "## Article" in result, result
    assert "chosen article text" in result and "too short" not in result, result


def test_web_fetch_supports_text_json_and_xml(ctx):
    """Textual non-HTML media types retain normalized readable content."""
    for route, marker in (("text", "one\ntwo\nthree"),
                          ("json", '{"ok": true}'),
                          ("xml", "<root>value</root>")):
        ctx.mock.reset()
        _, result = run_web(ctx, "page_fetch", {"url": f"{ctx.mock.origin}/web/{route}"})
        assert marker in result, result


def test_web_fetch_refuses_binary_and_oversized_bodies(ctx):
    """Unsupported media and decompression expansion fail before extraction."""
    _, binary = run_web(ctx, "page_fetch", {"url": f"{ctx.mock.origin}/web/binary"})
    assert binary.startswith("ERROR: ") and "application/pdf" in binary, binary

    ctx.mock.reset()
    _, large = run_web(ctx, "page_fetch", {"url": f"{ctx.mock.origin}/web/oversized"})
    assert large.startswith("ERROR: ") and "exceeds 2097152 bytes" in large, large

    ctx.mock.reset()
    _, expanded = run_web(
        ctx, "page_fetch", {"url": f"{ctx.mock.origin}/web/gzip-oversized"}
    )
    assert expanded.startswith("ERROR: ") and "exceeds 2097152 bytes" in expanded


def test_web_fetch_pages_at_line_boundaries(ctx):
    """The body page stays under 8 KiB and gives an exact refetch call."""
    url = f"{ctx.mock.origin}/web/lines"
    _, first = run_web(ctx, "page_fetch", {"url": url, "limit": 2000})
    assert len(first.encode()) <= 8192, len(first.encode())
    assert "line 1\n" in first
    assert f'[continue with page_fetch {{"url":"{url}","offset":' in first, first[-300:]

    ctx.mock.reset()
    _, later = run_web(ctx, "page_fetch", {"url": url, "offset": 2400, "limit": 100})
    assert "line 2400\n" in later and "line 2499\n" in later, later


def test_web_fetch_validates_urls_before_networking(ctx):
    """Schemes, credentials, and private destinations are refused explicitly."""
    cases = [
        ("file:///etc/passwd", "HTTP(S)"),
        ("http://user:pass@example.com/", "without credentials"),
        *[(f"http://{host}/secret", "non-public") for host in (
            "localhost", "0.0.0.0", "10.0.0.1", "100.64.0.1", "127.0.0.1",
            "169.254.1.1", "172.16.0.1", "192.0.2.1", "192.168.0.1",
            "198.18.0.1", "198.51.100.1", "203.0.113.1", "224.0.0.1",
            "240.0.0.1", "[::]", "[::1]", "[fc00::1]", "[fe80::1]",
            "[ff02::1]", "[2001:2::1]", "[2001:db8::1]",
            "[::ffff:127.0.0.1]",
        )],
    ]
    spec = ",".join(
        f"tool=page_fetch:{json.dumps({'url': url})}" for url, _ in cases
    )
    ctx.scenario(spec + ",final_text=done")
    s = ctx.spawn()
    s.submit("validate destinations")
    s.wait_text("done")
    s.wait_turn_done()
    results = ctx.mock.tool_results()
    assert len(results) == len(cases), results
    for result, (_, marker) in zip(results, cases):
        assert result.startswith("ERROR: ") and marker in result, result


def test_web_argument_limits_are_refused_not_clamped(ctx):
    """Empty, NUL-containing, oversized, and out-of-range arguments never run."""
    calls = [
        ("internet_search", {"query": ""}, "query is empty"),
        ("internet_search", {"query": "a\x00b"}, "nul byte"),
        ("internet_search", {"query": "x" * 1025}, "query too long"),
        ("internet_search", {"query": "ok", "limit": 11}, "1..10"),
        ("page_fetch", {"url": "http://example.com/" + "x" * 4096}, "URL too long"),
        ("page_fetch", {"url": "http://example.com/", "limit": 2001}, "1..2000"),
    ]
    spec = ",".join(f"tool={name}:{json.dumps(args)}" for name, args, _ in calls)
    ctx.scenario(spec + ",final_text=done")
    s = ctx.spawn()
    s.submit("try invalid web calls")
    s.wait_text("done")
    s.wait_turn_done()
    results = ctx.mock.tool_results()
    assert len(results) == len(calls), results
    for result, (_, _, marker) in zip(results, calls):
        assert result.startswith("ERROR: ") and marker in result, result


def test_web_tools_can_be_disabled_together(ctx):
    """The existing disable list withholds and refuses both network tools."""
    ctx.scenario('tool=page_fetch:{"url":"https://example.com/"},final_text=done')
    s = ctx.spawn(args=["--disable-tools", "internet_search,page_fetch"])
    s.submit("fetch")
    s.wait_turn_done()
    names = [t["function"]["name"] for t in ctx.mock.requests[0]["tools"]]
    assert "internet_search" not in names and "page_fetch" not in names
    assert "page_fetch is disabled" in ctx.mock.tool_results()[-1]


def test_web_transfer_keeps_ctrl_c_responsive(ctx):
    """A slow page wait pumps the TUI and Ctrl-C interrupts the transfer."""
    url = f"{ctx.mock.origin}/web/slow"
    ctx.scenario(f"tool=page_fetch:{json.dumps({'url': url})},final_text=too+late")
    s = ctx.spawn(**web_env(ctx))
    s.submit("fetch slowly")
    s.wait_text("◆  page_fetch")
    s.key("ctrl-c")
    s.wait_text("[interrupted]", timeout=1.0)
    s.wait_turn_done()
    assert "too late" not in s.text()


def test_web_telemetry_keeps_queries_urls_and_content_out(ctx):
    """Only the operation label and hashed host reach diagnostics."""
    secret = "private-query-needle"
    ctx.write_config("telemetry = true\n")
    ctx.scenario(
        f"tool=internet_search:{json.dumps({'query': secret})},final_text=private-final"
    )
    s = ctx.spawn(**web_env(ctx))
    s.submit("private prompt needle")
    s.wait_turn_done()

    files = list((ctx.home / ".local" / "state" / "arqan" / "telemetry").rglob("*.jsonl"))
    assert files
    logged = "".join(p.read_text() for p in files)
    for value in (
        secret, "private prompt needle", "private-final", "First & best",
        "/web/search", ctx.mock.origin, "example.com/first",
    ):
        assert value not in logged, logged
    events = [json.loads(line) for p in files for line in p.read_text().splitlines()]
    web_http = [e for e in events if e.get("ev") == "http" and e.get("path") == "internet_search"]
    assert web_http and len(web_http[-1]["host"]) == 16, web_http


def test_web_results_survive_session_resume(ctx):
    """Generic conversation persistence replays web results without a cache."""
    first = f"{ctx.mock.origin}/web/text"
    s, result = run_web(ctx, "page_fetch", {"url": first})
    assert "one\ntwo\nthree" in result
    s.submit("/exit")
    s.wait_exit()

    again = ctx.spawn(rows=40, **web_env(ctx))
    again.submit("/resume")
    again.wait_status("pick a session")
    again.key("enter")
    again.wait_text("External page (untrusted):")
    assert f"Final URL: {first}" in again.text(), again.text()


def test_web_call_and_result_render_on_a_narrow_screen(ctx):
    """The generic folded result keeps the URL header and expansion target."""
    url = f"{ctx.mock.origin}/web/page"
    ctx.scenario(f"tool=page_fetch:{json.dumps({'url': url})},final_text=done")
    s = ctx.spawn(cols=42, rows=28, **web_env(ctx))
    s.submit("fetch")
    s.wait_text("done")
    s.wait_turn_done()
    text = s.text()
    assert "Final URL:" in text, text
    assert "more lines" in text, text
    assert "{\"url\"" not in text, text
