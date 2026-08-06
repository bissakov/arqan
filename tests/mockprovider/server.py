"""A dummy OpenAI-compatible chat-completions provider.

Speaks just enough of the API for `yoke`: `POST /v1/chat/completions` with
`stream: true`, SSE deltas, tool calls, `stream_options.include_usage` and
`[DONE]`. The body is lorem ipsum whose length, chunking and pacing come from
a scenario, so a test can ask for "40 words in 5-word chunks, 20 ms apart,
after a tool call to read()" and get exactly that every run.

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
        self.prefix: str = kw.get("prefix", "")
        # GET /v1/models: an explicit list, or `model_count` generated ids.
        self.models: list[str] = kw.get("models", [])
        self.model_count: int = int(kw.get("model_count", 0))
        self.models_status: int = int(kw.get("models_status", 200))

    def model_ids(self) -> list[str]:
        if self.models:
            return self.models
        if self.model_count:
            return [f"model-{i:03d}" for i in range(self.model_count)]
        return ["mock"]

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


class _Handler(BaseHTTPRequestHandler):
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
            self._json(200, {"ok": True})
            return
        if self.path.startswith("/__scenario"):
            body = self._read_body()
            srv.scenario = Scenario.parse(body.get("scenario", ""))
            self._json(200, {"ok": True})
            return
        if not self.path.endswith("/chat/completions"):
            self._json(404, {"error": {"message": "not found"}})
            return
        self._completions()

    # -- the interesting one -----------------------------------------------
    def _completions(self):
        srv = self.server
        body = self._read_body()
        srv.requests.append(body)

        model = str(body.get("model", ""))
        scenario = srv.scenario
        if ":" in model:
            head, _, spec = model.partition(":")
            if head in ("lorem", "mock", "scenario"):
                scenario = Scenario.parse(spec)

        if scenario.status != 200:
            self._json(
                scenario.status,
                {"error": {"message": scenario.error, "type": "mock_error"}},
            )
            return

        messages = body.get("messages") or []
        tool_replies = sum(1 for m in messages if m.get("role") == "tool")
        emit_tools = bool(scenario.tools) and tool_replies < scenario.tool_rounds * len(
            scenario.tools
        )

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

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
            for piece in chunks(text, scenario.chunk):
                if not self._sse(frame({"content": piece})):
                    return
                if scenario.delay:
                    time.sleep(scenario.delay)
            finish = "stop"

        if not self._sse(frame({}, finish)):
            return

        if scenario.usage:
            prompt = scenario.prompt_tokens
            if prompt is None:
                prompt = max(1, len(json.dumps(messages)) // 4)
            completion = scenario.completion_tokens
            if completion is None:
                completion = max(1, completion_chars // 4)
            usage = {
                "prompt_tokens": prompt,
                "completion_tokens": completion,
                "total_tokens": prompt + completion,
            }
            if not self._sse(dict(base, choices=[], usage=usage)):
                return

        try:
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


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

    def reset(self):
        self.requests.clear()

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
        out = []
        for req in self.requests:
            for m in req.get("messages", []):
                if m.get("role") == "tool":
                    out.append(m.get("content", ""))
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
