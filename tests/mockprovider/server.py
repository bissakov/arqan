"""A dummy provider, OpenAI-compatible and Anthropic-compatible.

Speaks just enough of both APIs for `yoke`: `POST /v1/chat/completions` with
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
  * the model name itself: `YOKE_MODEL=lorem:words=40,delay=0.02`, which makes
    the server useful standalone with no test code in the loop.

Standalone:

    python3 -m tests.mockprovider.server --port 8080 --scenario words=80

    YOKE_BASE_URL=http://127.0.0.1:8080/v1 YOKE_API_KEY=x YOKE_MODEL=mock ./bin/yoke
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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
        # reasoning: streamed before the content, in the field a provider of
        # that family uses ("reasoning_content" or "reasoning").
        self.reasoning: str | None = kw.get("reasoning")
        self.reasoning_field: str = kw.get("reasoning_field", "reasoning_content")
        self.final_text: str | None = kw.get("final_text")
        self.prompt_tokens = kw.get("prompt_tokens")
        self.completion_tokens = kw.get("completion_tokens")
        self.usage: bool = _truthy(kw.get("usage", True))
        # Some providers report usage on an early event rather than the last;
        # when set, yoke hears it before any reasoning or content, which is
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

    def model_ids(self) -> list[str]:
        if self.models_empty:
            return []
        if self.models:
            return self.models
        if self.model_count:
            return [f"model-{i:03d}" for i in range(self.model_count)]
        return ["mock"]

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
            elif key in ("text", "final_text", "prefix", "error", "reasoning"):
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
        which is the one yoke reads."""
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
        emit_tools = bool(scenario.tools) and tool_replies < scenario.tool_rounds * len(
            scenario.tools
        )

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
                "usage": {"input_tokens": prompt, "output_tokens": 0},
            }
        }
        if not self._anth_sse("message_start", start):
            return

        if scenario.first_delay:
            time.sleep(scenario.first_delay)

        index = 0
        completion_chars = 0
        if scenario.reasoning and tool_replies == 0:
            if not self._anth_block(
                index, {"type": "thinking", "thinking": ""},
                [{"type": "thinking_delta", "thinking": piece}
                 for piece in chunks(scenario.reasoning, scenario.chunk)],
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
        if scenario.reasoning and tool_replies == 0:
            content.append({"type": "thinking", "thinking": scenario.reasoning})
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
            "usage": {"input_tokens": prompt, "output_tokens": completion},
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
        if self.path.startswith("/__requests"):
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
                    "data": [
                        {"id": m, "object": "model", "owned_by": "mock"}
                        for m in scenario.model_ids()
                    ],
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
        emit_tools = bool(scenario.tools) and tool_replies < scenario.tool_rounds * len(
            scenario.tools
        )

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
                head = {
                    "tool_calls": [
                        {
                            "index": index,
                            "id": f"call_{index}",
                            "type": "function",
                            "function": {"name": name, "arguments": ""},
                        }
                    ]
                }
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
                    "id": f"call_{index}",
                    "type": "function",
                    "function": {"name": name, "arguments": args},
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


def _split(s: str, n: int) -> list[str]:
    return [s[i : i + n] for i in range(0, len(s), n)] or [""]


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class MockProvider:
    """Owns the background server; `base_url` plugs straight into YOKE_BASE_URL."""

    def __init__(self, scenario: str | Scenario | None = None, host="127.0.0.1", port=0):
        self.httpd = _Server((host, port), _Handler)
        self.httpd.requests = []           # type: ignore[attr-defined]
        self.httpd.auth = []               # type: ignore[attr-defined]
        self.httpd.keys = []               # type: ignore[attr-defined]
        self.httpd.versions = []           # type: ignore[attr-defined]
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
    def requests(self) -> list:
        return self.httpd.requests  # type: ignore[attr-defined]

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

    def reset(self):
        self.requests.clear()
        self.auth.clear()
        self.keys.clear()
        self.versions.clear()

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
        f"  YOKE_BASE_URL={mock.base_url} YOKE_API_KEY=test YOKE_MODEL=mock ./bin/yoke"
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
