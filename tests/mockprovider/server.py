"""A dummy provider, OpenAI-compatible and Anthropic-compatible.

Speaks just enough of both APIs for `arqan`: `POST /v1/chat/completions` with
`stream: true`, SSE deltas, tool calls, `stream_options.include_usage` and
`[DONE]`, and `POST /v1/messages` with the content-block events the Anthropic
API streams. One scenario drives either, so a case says what comes back and
not which wire format it arrives in. The body is lorem ipsum whose length,
chunking and pacing come from a scenario, so a test can ask for "40 words in
5-word chunks, 20 ms apart, after a tool call to read()" and get exactly that
every run.

Two ways to pick a scenario:

  * server default: `MockProvider(scenario="words=40,chunk=5")`, used by the
    test harness so the status line keeps a clean model name;
  * the model name itself: `ARQAN_MODEL=lorem:words=40,delay=0.02`, which makes
    the server useful standalone with no test code in the loop.

Standalone:

    python3 -m tests.mockprovider.server --port 8080 --scenario words=80

    ARQAN_BASE_URL=http://127.0.0.1:8080/v1 ARQAN_API_KEY=x ARQAN_MODEL=mock ./bin/arqan
"""

from __future__ import annotations

import argparse
import gzip
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlsplit

try:  # importable both as a package module and as a script
    from . import lorem
except ImportError:  # pragma: no cover
    import lorem  # type: ignore


# ---- scenarios ------------------------------------------------------------


class Scenario:
    """Everything the mock needs to decide what to stream back.

    Fields map one-to-one onto the `key=value,key=value` DSL, so
    `Scenario.parse("words=12,chunk=2")` and the constructor stay in step.
    """

    def __init__(self, **kw):
        self.text: str | None = kw.get("text")
        self.words: int = int(kw.get("words", 24))
        self.paragraphs: int = int(kw.get("paragraphs", 1))
        self.seed: int = int(kw.get("seed", 7))
        self.chunk: int = int(kw.get("chunk", 4))
        self.delay: float = float(kw.get("delay", 0.0))
        self.first_delay: float = float(kw.get("first_delay", 0.0))
        self.status: int = int(kw.get("status", 200))
        self.error: str = kw.get("error", "mock provider error")
        # Transient failures: the first `fail_times` completion requests fail,
        # with `status` (default 503) or, with fail_mode=close, by dropping the
        # connection before answering. `abort_after` drops it mid-stream, after
        # that many content deltas have been sent.
        self.fail_times: int = int(kw.get("fail_times", 0))
        self.fail_mode: str = kw.get("fail_mode", "status")
        self.fail_status: int = int(kw.get("fail_status", 503))
        self.abort_after: int = int(kw.get("abort_after", 0))
        # tools: "read:{...}" entries, repeatable via `|`
        self.tools: list[tuple[str, str]] = kw.get("tools", [])
        self.tool_rounds: int = int(kw.get("tool_rounds", 1))
        # Not every OpenAI-compatible server names its calls: with tool_ids=0
        # the call arrives without an "id", which a client has to tolerate.
        self.tool_ids: bool = _truthy(kw.get("tool_ids", "1"))
        # reasoning: streamed before the content, in the field a provider of
        # that family uses ("reasoning_content" or "reasoning").
        self.reasoning: str | None = kw.get("reasoning")
        self.redacted: str | None = kw.get("redacted")
        self.reasoning_field: str = kw.get("reasoning_field", "reasoning_content")
        self.final_text: str | None = kw.get("final_text")
        self.prompt_tokens = kw.get("prompt_tokens")
        self.completion_tokens = kw.get("completion_tokens")
        self.cache_creation_tokens: int = int(kw.get("cache_creation", 0))
        self.cache_read_tokens: int = int(kw.get("cache_read", 0))
        self.usage: bool = _truthy(kw.get("usage", True))
        # Some providers report usage on an early event rather than the last;
        # when set, arqan hears it before any reasoning or content, which is
        # what an interrupt after that cannot take back.
        self.usage_first: bool = _truthy(kw.get("usage_first", "0"))
        # A broken but common SSE server sends [DONE] and keeps an unframed
        # HTTP/1.1 response open. Clients should still honor the sentinel.
        self.keep_open: bool = _truthy(kw.get("keep_open", "0"))
        self.prefix: str = kw.get("prefix", "")
        # GET /v1/models: an explicit list, or `model_count` generated ids.
        self.models: list[str] = kw.get("models", [])
        self.model_count: int = int(kw.get("model_count", 0))
        self.models_empty: bool = _truthy(kw.get("models_empty", "0"))
        self.models_status: int = int(kw.get("models_status", 200))
        # GET /v1/models may publish a context window, under whichever name
        # the endpoint being imitated uses. "top_provider" nests it the way
        # OpenRouter does.
        self.model_window: int = int(kw.get("model_window", 0))
        self.model_window_key: str = kw.get("model_window_key",
                                            "context_length")

    def model_ids(self) -> list[str]:
        if self.models_empty:
            return []
        if self.models:
            return self.models
        if self.model_count:
            return [f"model-{i:03d}" for i in range(self.model_count)]
        return ["mock"]

    def model_entry(self, name: str) -> dict:
        entry = {"id": name, "object": "model", "owned_by": "mock"}
        if self.model_window:
            if self.model_window_key == "top_provider":
                entry["top_provider"] = {"context_length": self.model_window}
            else:
                entry[self.model_window_key] = self.model_window
        return entry

    def _usage(self, messages, completion_chars):
        prompt = self.prompt_tokens
        if prompt is None:
            prompt = max(1, len(json.dumps(messages)) // 4)
        completion = self.completion_tokens
        if completion is None:
            completion = max(1, completion_chars // 4)
        return {
            "prompt_tokens": prompt,
            "completion_tokens": completion,
            "total_tokens": prompt + completion,
        }

    # -- DSL ---------------------------------------------------------------
    @staticmethod
    def parse(spec: str | None) -> "Scenario":
        kw: dict = {}
        if not spec:
            return Scenario()
        for part in _split_fields(spec):
            part = part.strip()
            if not part:
                continue
            if "=" not in part:
                kw[part] = "1"
                continue
            key, value = part.split("=", 1)
            key = key.strip()
            value = value.strip()
            if key == "tool":
                name, _, args = value.partition(":")
                kw.setdefault("tools", []).append((name, args or "{}"))
            elif key == "usage":
                if "/" in value:
                    p, c = value.split("/", 1)
                    kw["prompt_tokens"] = int(p)
                    kw["completion_tokens"] = int(c)
                    kw["usage"] = "1"
                else:
                    kw["usage"] = value
            elif key == "models":
                kw["models"] = [m for m in _unescape(value).split("|") if m]
            elif key in ("text", "final_text", "prefix", "error", "reasoning",
                         "redacted"):
                kw[key] = _unescape(value)
            else:
                kw[key] = value
        return Scenario(**kw)

    # -- body --------------------------------------------------------------
    def body_text(self) -> str:
        if self.text is not None:
            return self.prefix + self.text
        return self.prefix + lorem.text(self.words, self.paragraphs, self.seed)

    def follow_up_text(self) -> str:
        if self.final_text is not None:
            return self.final_text
        return lorem.text(max(6, self.words // 2), 1, self.seed + 1)


def _truthy(v) -> bool:
    if isinstance(v, bool):
        return v
    return str(v).lower() not in ("0", "false", "no", "off")


def _unescape(v: str) -> str:
    """'+' is a space and '\\n' a line break, so prose fits in one DSL field."""
    return v.replace("\\n", "\n").replace("+", " ")


def _split_fields(spec: str) -> list[str]:
    """Split on commas that are not inside a tool's JSON argument blob."""
    out: list[str] = []
    depth = 0
    quoted = False
    escaped = False
    current: list[str] = []
    for ch in spec:
        if escaped:
            current.append(ch)
            escaped = False
            continue
        if quoted:
            current.append(ch)
            if ch == "\\":
                escaped = True
            elif ch == '"':
                quoted = False
            continue
        if ch == '"':
            quoted = True
        elif ch in "{[":
            depth += 1
        elif ch in "}]":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append("".join(current))
            current = []
            continue
        current.append(ch)
    out.append("".join(current))
    return out


def chunks(text: str, words_per_chunk: int) -> list[str]:
    """Split on word boundaries, keeping every separator byte."""
    if words_per_chunk <= 0:
        return [text]
    out: list[str] = []
    current: list[str] = []
    count = 0
    token = ""
    for ch in text:
        token += ch
        if ch in " \n":
            current.append(token)
            token = ""
            count += 1
            if count >= words_per_chunk:
                out.append("".join(current))
                current, count = [], 0
    if token:
        current.append(token)
    if current:
        out.append("".join(current))
    return out


# ---- HTTP -----------------------------------------------------------------


class _AnthropicHandlerMixin:
    """`POST /v1/messages`: the same scenario in Anthropic's shapes.

    A reply is a sequence of content blocks rather than a flat delta stream,
    the prompt cost is reported on message_start and the completion cost on
    message_delta, and the arguments of a tool call arrive as partial JSON.
    """

    def _anth_sse(self, kind: str, obj) -> bool:
        """An event carries its type twice: in the event line and in the data,
        which is the one arqan reads."""
        try:
            payload = json.dumps(dict(obj, type=kind)).encode()
            self.wfile.write(b"event: " + kind.encode() + b"\n")
            self.wfile.write(b"data: " + payload + b"\n\n")
            self.wfile.flush()
            return True
        except (BrokenPipeError, ConnectionResetError, OSError):
            return False

    def _anthropic(self):
        srv = self.server
        body = self._read_body()
        self._record(body)

        model = str(body.get("model", ""))
        scenario = self._scenario_for(body)
        if self._refused(scenario):
            return

        messages = body.get("messages") or []
        tool_replies = _anth_tool_replies(messages)
        emit_tools = bool(scenario.tools) and _anth_turn_replies(
            messages
        ) < scenario.tool_rounds * len(scenario.tools)

        if not body.get("stream", True):
            self._anth_message(scenario, messages, tool_replies, emit_tools, model)
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        prompt, completion = self._anth_usage(scenario, messages, 0)
        start = {
            "message": {
                "id": "msg_mock",
                "type": "message",
                "role": "assistant",
                "model": model or "mock",
                "content": [],
                "usage": {
                    "input_tokens": prompt,
                    "output_tokens": 0,
                    "cache_creation_input_tokens": scenario.cache_creation_tokens,
                    "cache_read_input_tokens": scenario.cache_read_tokens,
                },
            }
        }
        if not self._anth_sse("message_start", start):
            return

        if scenario.first_delay:
            time.sleep(scenario.first_delay)

        index = 0
        completion_chars = 0
        if scenario.redacted and tool_replies == 0:
            if not self._anth_block(
                index, {"type": "redacted_thinking", "data": scenario.redacted},
                [], scenario,
            ):
                return
            index += 1
        if scenario.reasoning and tool_replies == 0:
            if not self._anth_block(
                index, {"type": "thinking", "thinking": ""},
                [{"type": "thinking_delta", "thinking": piece}
                 for piece in chunks(scenario.reasoning, scenario.chunk)]
                + [{"type": "signature_delta", "signature": "sig_mock"}],
                scenario,
            ):
                return
            index += 1

        if emit_tools:
            for order, (name, args) in enumerate(scenario.tools):
                head = {"type": "tool_use", "id": f"call_{order}",
                        "name": name, "input": {}}
                deltas = [{"type": "input_json_delta", "partial_json": piece}
                          for piece in _split(args, 12)]
                if not self._anth_block(index, head, deltas, scenario):
                    return
                index += 1
                completion_chars += len(args)
            stop = "tool_use"
        else:
            text = scenario.body_text() if tool_replies == 0 else scenario.follow_up_text()
            completion_chars = len(text)
            deltas = [{"type": "text_delta", "text": piece}
                      for piece in chunks(text, scenario.chunk)]
            if not self._anth_block(
                index, {"type": "text", "text": ""}, deltas, scenario, abort=True
            ):
                return
            stop = "end_turn"

        _, completion = self._anth_usage(scenario, messages, completion_chars)
        if not self._anth_sse(
            "message_delta",
            {"delta": {"stop_reason": stop}, "usage": {"output_tokens": completion}},
        ):
            return
        self._anth_sse("message_stop", {})

    def _anth_block(self, index, head, deltas, scenario, abort=False) -> bool:
        """One content block, start to stop. False once the client has gone."""
        if not self._anth_sse(
            "content_block_start", {"index": index, "content_block": head}
        ):
            return False
        sent = 0
        for delta in deltas:
            if not self._anth_sse(
                "content_block_delta", {"index": index, "delta": delta}
            ):
                return False
            sent += 1
            if abort and scenario.abort_after and sent >= scenario.abort_after:
                self._reset()
                return False
            if scenario.delay:
                time.sleep(scenario.delay)
        return self._anth_sse("content_block_stop", {"index": index})

    def _anth_usage(self, scenario, messages, completion_chars):
        usage = scenario._usage(messages, completion_chars)
        return usage["prompt_tokens"], usage["completion_tokens"]

    def _anth_message(self, scenario, messages, tool_replies, emit_tools, model):
        """`stream: false`: the same reply as one message document."""
        content = []
        completion_chars = 0
        if scenario.redacted and tool_replies == 0:
            content.append({"type": "redacted_thinking", "data": scenario.redacted})
        if scenario.reasoning and tool_replies == 0:
            content.append({"type": "thinking", "thinking": scenario.reasoning,
                            "signature": "sig_mock"})
        if emit_tools:
            for order, (name, args) in enumerate(scenario.tools):
                try:
                    parsed = json.loads(args)
                except json.JSONDecodeError:
                    parsed = {}
                content.append({"type": "tool_use", "id": f"call_{order}",
                                "name": name, "input": parsed})
                completion_chars += len(args)
            stop = "tool_use"
        else:
            text = scenario.body_text() if tool_replies == 0 else scenario.follow_up_text()
            content.append({"type": "text", "text": text})
            completion_chars = len(text)
            stop = "end_turn"

        prompt, completion = self._anth_usage(scenario, messages, completion_chars)
        payload = {
            "id": "msg_mock",
            "type": "message",
            "role": "assistant",
            "model": model or "mock",
            "content": content,
            "stop_reason": stop,
            "usage": {
                "input_tokens": prompt,
                "output_tokens": completion,
                "cache_creation_input_tokens": scenario.cache_creation_tokens,
                "cache_read_input_tokens": scenario.cache_read_tokens,
            },
        }
        if scenario.first_delay:
            time.sleep(scenario.first_delay)
        self._json(200, payload)


class _Handler(_AnthropicHandlerMixin, BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mock-openai/1.0"

    # quiet by default; the runner captures stderr anyway
    def log_message(self, fmt, *args):
        if getattr(self.server, "verbose", False):
            print("[mock] " + fmt % args)

    # -- helpers -----------------------------------------------------------
    def _json(self, code: int, payload):
        raw = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True

    def _body(self, code: int, content_type: str, body: bytes, **headers):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for name, value in headers.items():
            self.send_header(name.replace("_", "-"), value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        self.close_connection = True

    def _web_search(self):
        query = parse_qs(urlsplit(self.path).query).get("q", [""])[0]
        if query in ("status202", "status403", "status429"):
            status = int(query.removeprefix("status"))
            self._body(status, "text/html", b"<html><body>refused</body></html>")
            return
        if query == "liteblocked":
            self._body(202, "text/html", b"<html><body>refused</body></html>")
            return
        if query == "bothfail":
            self._body(202, "text/html", b"<html><body>refused</body></html>")
            return
        if query == "challenge":
            self._body(200, "text/html", b"<html><body><form class='challenge-form'>captcha</form></body></html>")
            return
        if query == "changed":
            self._body(200, "text/html", b"<html><body>different markup</body></html>")
            return
        if query == "empty":
            self._body(200, "text/html", b"<html><body><div class='no-results'>No results.</div></body></html>")
            return
        if query == "verbose":
            # ten results whose snippets together exceed one result page
            rows = []
            for i in range(10):
                url = quote(f"https://example.com/verbose/{i}", safe="")
                rows.append(
                    f'<a class="result-link" href="//duckduckgo.com/l/?uddg={url}">'
                    f"Verbose result {i}</a>"
                    f'<div class="result-snippet">{"padding " * 120}tail {i}</div>'
                )
            self._body(200, "text/html",
                       ("<html><body>" + "".join(rows) + "</body></html>").encode())
            return
        one = quote("https://example.com/first?a=1&b=two", safe="")
        duplicate = quote("https://example.com/first?a=1&b=two", safe="")
        two = quote("http://example.org/two", safe="")
        html = f"""<!doctype html><html><body>
          <a class="result-link" href="//duckduckgo.com/l/?uddg={one}">First &amp; best</a>
          <div class="result-snippet">A <b>nested</b> snippet for {query}.</div>
          <a class="result-link" href="//duckduckgo.com/l/?uddg={duplicate}">Duplicate</a>
          <div class="result-snippet">duplicate snippet</div>
          <a class="result-link" href="%zz">Malformed</a>
          <a class="other" href="https://ignored.example/">Ignored</a>
          <a class="result-link extra" href="//duckduckgo.com/l/?uddg={two}">Second result Ω</a>
        </body></html>""".encode()
        self._body(200, "text/html; charset=utf-8", html)

    def _web_search_html(self):
        """The html.duckduckgo.com layout used when the lite endpoint refuses."""
        query = parse_qs(urlsplit(self.path).query).get("q", [""])[0]
        if query in ("status202", "status403", "status429"):
            status = int(query.removeprefix("status"))
            self._body(status, "text/html", b"<html><body>refused</body></html>")
            return
        if query == "bothfail":
            self._body(200, "text/html", b"<html><body>different markup</body></html>")
            return
        one = quote("https://example.com/first?a=1&b=two", safe="")
        two = quote("http://example.org/two", safe="")
        html = f"""<!doctype html><html><body>
          <div class="result results_links">
            <a class="result__a" href="//duckduckgo.com/l/?uddg={one}&amp;rut=x">First &amp; best</a>
            <a class="result__url" href="//duckduckgo.com/l/?uddg={one}">example.com</a>
            <a class="result__snippet" href="#">A <b>nested</b> snippet for {query}.</a>
          </div>
          <div class="result results_links">
            <a class="result__a" href="//duckduckgo.com/l/?uddg={two}">Second result Ω</a>
            <a class="result__snippet" href="#">second snippet</a>
          </div>
        </body></html>""".encode()
        self._body(200, "text/html; charset=utf-8", html)

    def _web_search_brave(self):
        query = parse_qs(urlsplit(self.path).query).get("q", [""])[0]
        html = f"""<!doctype html><html><body>
          <div class="snippet svelte-jmfu5f" data-pos="0">
            <a href="https://example.com/first?a=1&amp;b=two" class="svelte-x l1">
              <cite class="snippet-url">example.com &rsaquo; first</cite>
              <div class="title search-snippet-title line-clamp-1">First &amp; best</div>
            </a>
            <div class="generic-snippet svelte-y"><div class="content">A
              <b>nested</b> snippet for {query}.</div></div>
          </div>
          <div class="snippet svelte-jmfu5f" data-pos="1">
            <a href="http://example.org/two" class="svelte-x l1">
              <div class="title search-snippet-title">Second result &#937;</div>
            </a>
            <div class="generic-snippet"><div class="content">second snippet</div></div>
          </div>
          <div class="snippet svelte-jmfu5f" data-pos="2">
            <a href="https://ads.example/promo" class="svelte-x l1"></a>
          </div>
        </body></html>""".encode()
        self._body(200, "text/html; charset=utf-8", html)

    def _web_search_api(self, array, wrapper, fields):
        """One keyed engine's JSON answer, in that engine's field names."""
        query = parse_qs(urlsplit(self.path).query).get("q", [""])[0]
        title, url, snippet = fields
        if query == "empty":
            body = {array: []}
        elif query == "changed":
            body = {"error": "quota"}
        else:
            body = {array: [
                {title: "First & best", url: "https://example.com/first?a=1&b=two",
                 snippet: f"A nested snippet for {query}."},
                {title: "Second result \u03a9", url: "http://example.org/two",
                 snippet: "second snippet"},
                {title: "Duplicate", url: "https://example.com/first?a=1&b=two",
                 snippet: "duplicate snippet"},
                {title: "Not public", url: "ftp://example.net/file",
                 snippet: "wrong scheme"},
            ]}
        if wrapper:
            body = {wrapper: body}
        self._body(200, "application/json", json.dumps(body).encode())

    def _web_page(self):
        filler = " ".join(["visible article content"] * 12)
        html = f"""<!doctype html><html><head>
          <title>Fixture &amp; title</title><base href="/web/base/">
        </head><body><nav>hidden navigation</nav><main>
          <h1>Heading</h1><p>{filler}</p>
          <ul><li>First item</li><li>Second item</li></ul>
          <table><tr><th>Name</th><th>Value</th></tr><tr><td>A</td><td>B</td></tr></table>
          <pre>if (x &lt; 2) {{\n    go();\n}}</pre>
          <p>Read <a href="child?q=one&amp;x=two">the child</a>.</p>
          <footer>hidden footer</footer>
        </main><article>{'fallback ' * 40}</article></body></html>""".encode()
        self._body(200, "text/html; charset=UTF-8", html)

    def _reset(self):
        """Break the connection with an RST, so a client mid-stream sees a
        transport failure rather than a body the close ended."""
        try:
            self.connection.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
            )
            self.connection.close()
        except OSError:
            pass
        self.close_connection = True

    def _read_body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            return json.loads(raw or b"{}")
        except UnicodeDecodeError:
            # A JSON body must be UTF-8 (RFC 8259), and a real provider
            # answers 400 rather than guessing. Record it so a case can
            # assert the agent never sends one.
            self.server.bad_utf8.append(raw)   # type: ignore[attr-defined]
            return {"_raw": raw.decode("utf-8", "replace")}
        except json.JSONDecodeError:
            return {"_raw": raw.decode("utf-8", "replace")}

    def _sse(self, obj) -> bool:
        """One SSE event; False once the client has gone away."""
        try:
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()
            return True
        except (BrokenPipeError, ConnectionResetError, OSError):
            return False

    # -- routes ------------------------------------------------------------
    def do_GET(self):
        srv = self.server
        path = urlsplit(self.path).path
        if path.startswith("/web/"):
            srv.web_user_agents.append(self.headers.get("User-Agent"))
            srv.web_request_times.append(time.monotonic())
            srv.web_calls.append({
                "path": path,
                "query": parse_qs(urlsplit(self.path).query),
                "token": self.headers.get("X-Subscription-Token"),
            })
        if path == "/web/search":
            self._web_search()
        elif path == "/web/search-html":
            self._web_search_html()
        elif path == "/web/brave/search":
            self._web_search_brave()
        elif path == "/web/searxng/search":
            self._web_search_api("results", None, ("title", "url", "content"))
        elif path == "/web/braveapi/res/v1/web/search":
            self._web_search_api("results", "web", ("title", "url", "description"))
        elif path == "/web/google/customsearch/v1":
            self._web_search_api("items", None, ("title", "link", "snippet"))
        elif path == "/web/page":
            self._web_page()
        elif path == "/web/malformed":
            article = " ".join(["chosen article text"] * 15)
            self._body(200, "text/html", (
                "<title>Broken &amp; useful</title><body>"
                "<main>too short</main><article><h2>Article</h2><p>"
                + article + "<b>bold"
            ).encode())
        elif path == "/web/redirect":
            self.send_response(302)
            self.send_header("Location", "/web/page")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
        elif path == "/web/text":
            self._body(200, "text/plain", b"one\r\ntwo\nthree\n")
        elif path == "/web/json":
            self._body(200, "application/problem+json", b'{"ok": true}\n')
        elif path == "/web/xml":
            self._body(200, "application/xml", b"<root>value</root>\n")
        elif path == "/web/binary":
            self._body(200, "application/pdf", b"%PDF-1.7\x00")
        elif path == "/web/oversized":
            self._body(200, "text/plain", b"x" * ((2 << 20) + 1))
        elif path == "/web/gzip-oversized":
            body = gzip.compress(b"x" * ((2 << 20) + 1))
            self._body(200, "text/plain", body, Content_Encoding="gzip")
        elif path == "/web/slow":
            time.sleep(2.0)
            self._body(200, "text/plain", b"finally\n")
        elif path == "/web/lines":
            self._body(200, "text/plain", b"".join(
                f"line {i}\n".encode() for i in range(1, 2501)
            ))
        elif path == "/web/base/child":
            self._body(200, "text/plain", b"child\n")
        elif self.path.startswith("/__requests"):
            self._json(200, srv.requests)
        elif self.path.startswith("/health"):
            self._json(200, {"ok": True, "requests": len(srv.requests)})
        elif self.path.startswith("/v1/models"):
            scenario = srv.scenario
            if scenario.models_status != 200:
                self._json(
                    scenario.models_status,
                    {"error": {"message": scenario.error, "type": "mock_error"}},
                )
                return
            self._json(
                200,
                {
                    "object": "list",
                    "data": [scenario.model_entry(m)
                             for m in scenario.model_ids()],
                },
            )
        else:
            self._json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        srv = self.server
        if self.path.startswith("/__reset"):
            srv.requests.clear()
            srv.auth.clear()
            srv.keys.clear()
            srv.versions.clear()
            self._json(200, {"ok": True})
            return
        if self.path.startswith("/__scenario"):
            body = self._read_body()
            srv.scenario = Scenario.parse(body.get("scenario", ""))
            self._json(200, {"ok": True})
            return
        if self.path.endswith("/chat/completions"):
            self._completions()
        elif self.path.endswith("/messages"):
            self._anthropic()
        else:
            self._json(404, {"error": {"message": "not found"}})

    def _record(self, body):
        """The request and the headers a key could have ridden in."""
        srv = self.server
        srv.requests.append(body)
        srv.auth.append(self.headers.get("Authorization"))
        srv.keys.append(self.headers.get("x-api-key"))
        srv.versions.append(self.headers.get("anthropic-version"))

    def _scenario_for(self, body):
        """The server's, unless the model name carries one of its own."""
        model = str(body.get("model", ""))
        scenario = self.server.scenario
        if ":" in model:
            head, _, spec = model.partition(":")
            if head in ("lorem", "mock", "scenario"):
                scenario = Scenario.parse(spec)
        return scenario

    def _refused(self, scenario) -> bool:
        """True once the request has been answered with a failure."""
        if scenario.status != 200:
            self._json(
                scenario.status,
                {"error": {"message": scenario.error, "type": "mock_error"}},
            )
            return True
        if len(self.server.requests) <= scenario.fail_times:
            if scenario.fail_mode == "close":
                # No response at all: curl reports an empty reply, which is the
                # transport failure a retry is for.
                self.close_connection = True
                return True
            self._json(
                scenario.fail_status,
                {"error": {"message": scenario.error, "type": "mock_error"}},
            )
            return True
        return False

    # -- the interesting one -----------------------------------------------
    def _completions(self):
        srv = self.server
        body = self._read_body()
        self._record(body)

        model = str(body.get("model", ""))
        scenario = self._scenario_for(body)
        if self._refused(scenario):
            return

        messages = body.get("messages") or []
        tool_replies = sum(1 for m in messages if m.get("role") == "tool")
        emit_tools = bool(scenario.tools) and _oai_turn_replies(
            messages
        ) < scenario.tool_rounds * len(scenario.tools)

        if not body.get("stream", True):
            self._completion(scenario, messages, tool_replies, emit_tools, model)
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive" if scenario.keep_open
                         else "close")
        self.end_headers()
        self.close_connection = not scenario.keep_open

        created = 1700000000
        base = {
            "id": "chatcmpl-mock",
            "object": "chat.completion.chunk",
            "created": created,
            "model": model or "mock",
        }

        def frame(delta, finish=None):
            return dict(
                base,
                choices=[{"index": 0, "delta": delta, "finish_reason": finish}],
            )

        if not self._sse(frame({"role": "assistant", "content": ""})):
            return

        if scenario.usage and scenario.usage_first and tool_replies == 0:
            usage = scenario._usage(messages, 0)
            if not self._sse(dict(base, choices=[], usage=usage)):
                return

        if scenario.first_delay:
            time.sleep(scenario.first_delay)

        completion_chars = 0
        if scenario.reasoning and tool_replies == 0:
            for piece in chunks(scenario.reasoning, scenario.chunk):
                if not self._sse(frame({scenario.reasoning_field: piece})):
                    return
                if scenario.delay:
                    time.sleep(scenario.delay)

        if emit_tools:
            for index, (name, args) in enumerate(scenario.tools):
                call = {
                    "index": index,
                    "type": "function",
                    "function": {"name": name, "arguments": ""},
                }
                if scenario.tool_ids:
                    call["id"] = f"call_{index}"
                head = {"tool_calls": [call]}
                if not self._sse(frame(head)):
                    return
                # arguments arrive in fragments, like a real provider
                for piece in _split(args, 12):
                    if not self._sse(
                        frame(
                            {
                                "tool_calls": [
                                    {
                                        "index": index,
                                        "function": {"arguments": piece},
                                    }
                                ]
                            }
                        )
                    ):
                        return
                    if scenario.delay:
                        time.sleep(scenario.delay)
                completion_chars += len(args)
            finish = "tool_calls"
        else:
            text = scenario.body_text() if tool_replies == 0 else scenario.follow_up_text()
            completion_chars = len(text)
            sent = 0
            for piece in chunks(text, scenario.chunk):
                if not self._sse(frame({"content": piece})):
                    return
                sent += 1
                if scenario.abort_after and sent >= scenario.abort_after:
                    self._reset()
                    return
                if scenario.delay:
                    time.sleep(scenario.delay)
            finish = "stop"

        if not self._sse(frame({}, finish)):
            return

        if scenario.usage:
            usage = scenario._usage(messages, completion_chars)
            if not self._sse(dict(base, choices=[], usage=usage)):
                return

        try:
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


    def _completion(self, scenario, messages, tool_replies, emit_tools, model):
        """`stream: false`: the same reply as one chat.completion document."""
        message = {"role": "assistant", "content": None}
        completion_chars = 0
        if scenario.reasoning and tool_replies == 0:
            message[scenario.reasoning_field] = scenario.reasoning
        if emit_tools:
            message["tool_calls"] = [
                {
                    "index": index,
                    "type": "function",
                    "function": {"name": name, "arguments": args},
                    **({"id": f"call_{index}"} if scenario.tool_ids else {}),
                }
                for index, (name, args) in enumerate(scenario.tools)
            ]
            completion_chars = sum(len(a) for _, a in scenario.tools)
            finish = "tool_calls"
        else:
            text = scenario.body_text() if tool_replies == 0 else scenario.follow_up_text()
            message["content"] = text
            completion_chars = len(text)
            finish = "stop"

        payload = {
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "created": 1700000000,
            "model": model or "mock",
            "choices": [{"index": 0, "message": message, "finish_reason": finish}],
        }
        if scenario.usage:
            prompt = scenario.prompt_tokens
            if prompt is None:
                prompt = max(1, len(json.dumps(messages)) // 4)
            completion = scenario.completion_tokens
            if completion is None:
                completion = max(1, completion_chars // 4)
            payload["usage"] = {
                "prompt_tokens": prompt,
                "completion_tokens": completion,
                "total_tokens": prompt + completion,
            }
        if scenario.first_delay:
            time.sleep(scenario.first_delay)
        self._json(200, payload)


def _anth_tool_replies(messages) -> int:
    """tool_result blocks so far, which is what a round of calls comes back as."""
    n = 0
    for m in messages:
        content = m.get("content")
        if isinstance(content, list):
            n += sum(1 for b in content if b.get("type") == "tool_result")
    return n


def _anth_turn_replies(messages) -> int:
    """The same count, but only for the turn being answered.

    The round budget is spent per turn: a conversation-wide count would let
    an earlier turn exhaust it and leave a later one silently answering with
    plain text where the scenario asked for a tool call. Which text a reply
    carries still keys off the whole conversation, so a follow-up stays a
    follow-up after a resume.
    """
    n = 0
    for m in messages[_anth_turn_start(messages):]:
        content = m.get("content")
        if isinstance(content, list):
            n += sum(1 for b in content if b.get("type") == "tool_result")
    return n


def _anth_turn_start(messages) -> int:
    """Index of the user message that opened the turn being answered."""
    for i in range(len(messages) - 1, -1, -1):
        m = messages[i]
        if m.get("role") != "user":
            continue
        content = m.get("content")
        if isinstance(content, list) and any(
            b.get("type") == "tool_result" for b in content
        ):
            continue          # a round of results, not the user asking
        return i
    return 0


def _oai_turn_replies(messages) -> int:
    """Tool results in the turn being answered; see `_anth_turn_replies`."""
    start = 0
    for i in range(len(messages) - 1, -1, -1):
        if messages[i].get("role") == "user":
            start = i
            break
    return sum(1 for m in messages[start:] if m.get("role") == "tool")


def _split(s: str, n: int) -> list[str]:
    return [s[i : i + n] for i in range(0, len(s), n)] or [""]


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class MockProvider:
    """Owns the background server; `base_url` plugs straight into ARQAN_BASE_URL."""

    def __init__(self, scenario: str | Scenario | None = None, host="127.0.0.1", port=0):
        self.httpd = _Server((host, port), _Handler)
        self.httpd.requests = []           # type: ignore[attr-defined]
        self.httpd.bad_utf8 = []           # type: ignore[attr-defined]
        self.httpd.auth = []               # type: ignore[attr-defined]
        self.httpd.keys = []               # type: ignore[attr-defined]
        self.httpd.versions = []           # type: ignore[attr-defined]
        self.httpd.web_user_agents = []     # type: ignore[attr-defined]
        self.httpd.web_request_times = []   # type: ignore[attr-defined]
        self.httpd.web_calls = []           # type: ignore[attr-defined]
        self.httpd.verbose = False         # type: ignore[attr-defined]
        self.scenario = scenario
        # socketserver's shutdown() only returns on the next poll tick, so the
        # default 0.5s would dominate the runtime of a short case.
        self.thread = threading.Thread(
            target=self.httpd.serve_forever, kwargs={"poll_interval": 0.01},
            daemon=True,
        )

    # -- configuration -----------------------------------------------------
    @property
    def scenario(self) -> Scenario:
        return self.httpd.scenario  # type: ignore[attr-defined]

    @scenario.setter
    def scenario(self, value):
        if isinstance(value, Scenario):
            self.httpd.scenario = value      # type: ignore[attr-defined]
        else:
            self.httpd.scenario = Scenario.parse(value)  # type: ignore[attr-defined]

    @property
    def port(self) -> int:
        return self.httpd.server_address[1]

    @property
    def host(self) -> str:
        return self.httpd.server_address[0]

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}/v1"

    @property
    def origin(self) -> str:
        return f"http://{self.host}:{self.port}"

    @property
    def requests(self) -> list:
        return self.httpd.requests  # type: ignore[attr-defined]

    @property
    def bad_utf8(self) -> list:
        """Request bodies that were not valid UTF-8, so not valid JSON."""
        return self.httpd.bad_utf8  # type: ignore[attr-defined]

    @property
    def auth(self) -> list:
        """The Authorization header of each completion request, None when absent."""
        return self.httpd.auth  # type: ignore[attr-defined]

    @property
    def keys(self) -> list:
        """The x-api-key header of each request, which is where the Anthropic
        API carries the key."""
        return self.httpd.keys  # type: ignore[attr-defined]

    @property
    def versions(self) -> list:
        """The anthropic-version header of each request, None when absent."""
        return self.httpd.versions  # type: ignore[attr-defined]

    @property
    def web_user_agents(self) -> list:
        """The User-Agent header of each fixture web request, if present."""
        return self.httpd.web_user_agents  # type: ignore[attr-defined]

    @property
    def web_request_times(self) -> list:
        """Monotonic arrival times for fixture web requests."""
        return self.httpd.web_request_times  # type: ignore[attr-defined]

    @property
    def web_calls(self) -> list:
        """One {"path", "query", "token"} per fixture web request."""
        return self.httpd.web_calls  # type: ignore[attr-defined]

    def reset(self):
        self.requests.clear()
        self.bad_utf8.clear()
        self.auth.clear()
        self.keys.clear()
        self.versions.clear()
        self.web_user_agents.clear()
        self.web_request_times.clear()
        self.web_calls.clear()

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> "MockProvider":
        self.thread.start()
        return self

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    # -- assertions helpers ------------------------------------------------
    def last_messages(self) -> list:
        return self.requests[-1].get("messages", []) if self.requests else []

    def tool_results(self) -> list[str]:
        """What was fed back to each call, in either API's shape."""
        out = []
        for req in self.requests:
            for m in req.get("messages", []):
                if m.get("role") == "tool":
                    out.append(m.get("content", ""))
                    continue
                content = m.get("content")
                if not isinstance(content, list):
                    continue
                for block in content:
                    if block.get("type") == "tool_result":
                        out.append(block.get("content", ""))
        return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="dummy OpenAI-compatible provider")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument(
        "--scenario",
        default="words=40,chunk=3,delay=0.03",
        help="key=value,... (words, paragraphs, seed, chunk, delay, "
        "first_delay, status, tool=name:args, tool_rounds, text, usage=P/C)",
    )
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    mock = MockProvider(args.scenario, host=args.host, port=args.port)
    mock.httpd.verbose = args.verbose  # type: ignore[attr-defined]
    print(f"mock provider on {mock.base_url}  scenario: {args.scenario}")
    print(
        f"  ARQAN_BASE_URL={mock.base_url} ARQAN_API_KEY=test ARQAN_MODEL=mock ./bin/arqan"
    )
    mock.start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        mock.stop()


if __name__ == "__main__":
    main()
