#!/usr/bin/env python3
"""A fake MCP server over stdio, for the end-to-end suite.

One JSON-RPC message per line on stdin and stdout, the way the stdio
transport asks for. The mode picks how it behaves; every mode that answers at
all speaks the same handshake.
"""

import argparse
import json
import os
import sys
import time

GOOD_TOOLS = [
    {
        "name": "echo",
        "description": "Echo the text back.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "big",
        "description": "Return more text than a result may carry.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "boom",
        "description": "Answer with an error result.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "picture",
        "description": "Answer with an image block.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "slow",
        "description": "Wait, then answer.",
        "inputSchema": {
            "type": "object",
            "properties": {"ms": {"type": "integer"}},
        },
    },
]

BAD_TOOLS = [
    {
        "name": "not a name",
        "description": "The name is not allowed.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "no_schema",
        "description": "The schema is not an object.",
        "inputSchema": "string",
    },
    {
        "name": "huge_schema",
        "description": "The schema is over the limit.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "f%04d" % i: {"type": "string", "description": "x" * 64}
                for i in range(64)
            },
        },
    },
]

HEADER_TOOLS = [
    {
        "name": "headed",
        "description": "Mirror selected arguments into HTTP headers.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "region": {"type": "string", "x-mcp-header": "Region"},
                "count": {"type": "integer", "x-mcp-header": "Count"},
                "nested": {
                    "type": "object",
                    "properties": {
                        "trace": {"type": "boolean", "x-mcp-header": "Trace"}
                    },
                },
            },
        },
    }
]

LINKED_TOOL = {
    "name": "linked",
    "description": "Answer with a resource link and an embedded resource.",
    "inputSchema": {"type": "object", "properties": {}},
}

INVALID_HEADER_TOOLS = [
    {
        "name": "invalid_header",
        "description": "Has an invalid header annotation.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "items": {"type": "array", "x-mcp-header": "Items"}
            },
        },
    }
]


def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def reply(req, result):
    send({"jsonrpc": "2.0", "id": req["id"], "result": result})


DISCOVER = {
    "resultType": "complete",
    "supportedVersions": ["2026-07-28"],
    "capabilities": {"tools": {}},
    "_meta": {
        "io.modelcontextprotocol/serverInfo": {"name": "fake", "version": "1.0"}
    },
}

META_VERSION = "io.modelcontextprotocol/protocolVersion"

RESOURCES = [
    {"uri": "demo://notes", "name": "notes", "mimeType": "text/plain"},
    {"uri": "demo://logo", "name": "logo", "mimeType": "image/png"},
]

PROMPTS = [
    {
        "name": "review",
        "description": "Review a file.",
        "arguments": [{"name": "path", "required": True}],
    }
]


def caps(opts):
    if opts.extras:
        return {"tools": {}, "resources": {}, "prompts": {}}
    return {"tools": {}}


def extra(opts, method, params):
    """Answer a resources or prompts call, or None when it is not one."""
    if not opts.extras:
        return None
    if method == "resources/list":
        return {"resources": RESOURCES}
    if method == "resources/read":
        uri = params.get("uri")
        if uri == "demo://notes":
            return {
                "contents": [
                    {"uri": uri, "mimeType": "text/plain", "text": "buy milk"}
                ]
            }
        if uri == "demo://logo":
            return {
                "contents": [{"uri": uri, "mimeType": "image/png", "blob": "AAAA"}]
            }
        raise KeyError("no such resource")
    if method == "prompts/list":
        return {"prompts": PROMPTS}
    if method == "prompts/get":
        if params.get("name") != "review":
            raise KeyError("no such prompt")
        path = (params.get("arguments") or {}).get("path", "nothing")
        return {
            "description": "Review a file.",
            "messages": [
                {
                    "role": "user",
                    "content": {"type": "text", "text": "Review %s please" % path},
                }
            ],
        }
    return None


def modern_gate(opts, req):
    """Answer a modern-era request, or None to let the normal path run."""
    method = req.get("method")
    if opts.era == "old-only":
        if method != "server/discover":
            return None
        return {
            "error": {
                "code": -32022,
                "message": "Unsupported protocol version",
                "data": {"supported": ["2025-03-26"], "requested": "2026-07-28"},
            }
        }
    if method == "server/discover":
        if opts.era == "unsupported":
            return {
                "error": {
                    "code": -32022,
                    "message": "Unsupported protocol version",
                    "data": {"supported": ["2027-01-01"], "requested": "2026-07-28"},
                }
            }
        return {"result": dict(DISCOVER, capabilities=caps(opts))}
    if method == "initialize":
        return {"error": {"code": -32601, "message": "no such method"}}
    meta = (req.get("params") or {}).get("_meta") or {}
    if meta.get(META_VERSION) != "2026-07-28":
        return {
            "error": {
                "code": -32022,
                "message": "Unsupported protocol version",
                "data": {"supported": ["2026-07-28"], "requested": meta.get(META_VERSION)},
            }
        }
    return None


def tools_for(mode):
    if mode == "bad":
        return BAD_TOOLS + [GOOD_TOOLS[0]]
    if mode == "headers":
        return HEADER_TOOLS
    if mode == "links":
        return GOOD_TOOLS + [LINKED_TOOL]
    if mode == "invalid-headers":
        return INVALID_HEADER_TOOLS
    if mode == "flood":
        return [
            {
                "name": "t%02d" % i,
                "description": "One of many.",
                "inputSchema": {"type": "object", "properties": {}},
            }
            for i in range(40)
        ]
    return GOOD_TOOLS


def call(mode, name, args):
    if name == "echo":
        text = args.get("text", "")
        return {"content": [{"type": "text", "text": "echo: " + str(text)}]}
    if name == "big":
        line = "a line of output that pads the result out" + " " * 8
        return {"content": [{"type": "text", "text": (line + "\n") * 400}]}
    if name == "boom":
        return {
            "content": [{"type": "text", "text": "the server refused"}],
            "isError": True,
        }
    if name == "picture":
        return {
            "content": [
                {"type": "text", "text": "here it is"},
                {"type": "image", "data": "aGk=", "mimeType": "image/png"},
            ]
        }
    if name == "slow":
        time.sleep(float(args.get("ms", 200)) / 1000.0)
        return {"content": [{"type": "text", "text": "awake"}]}
    if name == "headed":
        return {"content": [{"type": "text", "text": "headers received"}]}
    if name == "linked":
        return {
            "content": [
                {"type": "resource_link", "uri": "demo://notes", "name": "notes"},
                {
                    "type": "resource",
                    "resource": {
                        "uri": "demo://inline",
                        "mimeType": "text/plain",
                        "text": "inline words",
                    },
                },
            ]
        }
    return None


def handle(opts, req, state):
    """Answer one JSON-RPC message, or None for a notification."""
    method = req.get("method")
    params = req.get("params") or {}
    got = extra(opts, method, params)
    if got is not None:
        return got
    if method == "initialize":
        return {
            "protocolVersion": opts.legacy_version,
            "capabilities": caps(opts),
            "serverInfo": {"name": "fake", "version": "1.0"},
        }
    if method == "tools/list":
        return {"tools": tools_for(opts.mode)}
    if method == "tools/call":
        params = req.get("params") or {}
        result = call(opts.mode, params.get("name"), params.get("arguments") or {})
        if result is None:
            raise KeyError("no such tool")
        return result
    raise KeyError("no such method")


def serve_http(opts):
    """Serve the streamable HTTP transport on a loopback port."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    state = {
        "session": "s-" + str(os.getpid()),
        "deleted": False,
        "expired": False,
    }

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def note(self, text):
            if opts.log:
                with open(opts.log, "a") as f:
                    f.write(text + "\n")

        def do_DELETE(self):
            state["deleted"] = True
            self.note(json.dumps({"method": "DELETE"}))
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length).decode()
            self.note(
                json.dumps(
                    {
                        "raw": raw,
                        "session": self.headers.get("Mcp-Session-Id"),
                        "protocol": self.headers.get("MCP-Protocol-Version"),
                        "auth": self.headers.get("Authorization"),
                        "accept": self.headers.get("Accept"),
                        "mcp_method": self.headers.get("Mcp-Method"),
                        "mcp_name": self.headers.get("Mcp-Name"),
                        "param_region": self.headers.get("Mcp-Param-Region"),
                        "param_count": self.headers.get("Mcp-Param-Count"),
                        "param_trace": self.headers.get("Mcp-Param-Trace"),
                    }
                )
            )
            if opts.delay_ms:
                time.sleep(opts.delay_ms / 1000.0)
            if opts.status:
                self.send_response(opts.status)
                if 300 <= opts.status < 400:
                    port = self.server.server_address[1]
                    self.send_header("Location", "http://127.0.0.1:%d/moved" % port)
                if opts.status == 401:
                    self.send_header("WWW-Authenticate", 'Bearer realm="fake"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            req = json.loads(raw)
            if opts.reject_status and req.get("method") == "server/discover":
                self.reply(
                    {
                        "jsonrpc": "2.0",
                        "id": req.get("id"),
                        "error": {"code": -32601, "message": "no such method"},
                    },
                    opts.reject_status,
                    req,
                )
                return
            if (
                opts.expire_once
                and not state["expired"]
                and req.get("method") == "tools/call"
            ):
                state["expired"] = True
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if opts.era != "legacy":
                answer = modern_gate(opts, req) or self.header_gate(req)
                if answer is not None:
                    if "id" not in req:
                        self.send_response(202)
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                    msg = {"jsonrpc": "2.0", "id": req["id"]}
                    msg.update(answer)
                    code = 200 if "result" in msg else 400
                    self.reply(msg, code, req)
                    return
            if opts.session and req.get("method") != "initialize":
                if self.headers.get("Mcp-Session-Id") != state["session"]:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
            if "id" not in req:
                self.send_response(202)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            try:
                result = handle(opts, req, state)
                msg = {"jsonrpc": "2.0", "id": req["id"], "result": result}
            except KeyError as e:
                msg = {
                    "jsonrpc": "2.0",
                    "id": req["id"],
                    "error": {"code": -32601, "message": str(e)},
                }
            if opts.sse:
                body = (
                    ": waiting\n\n"
                    + "event: message\ndata: "
                    + json.dumps(msg)
                    + "\n\n"
                ).encode()
                ctype = "text/event-stream"
            else:
                body = json.dumps(msg).encode()
                ctype = "application/json"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            if opts.session and req.get("method") == "initialize":
                self.send_header("Mcp-Session-Id", state["session"])
            self.end_headers()
            self.wfile.write(body)

        def header_gate(self, req):
            """Check the headers that mirror the body in the modern era."""
            method = req.get("method")
            if self.headers.get("Mcp-Method") != method:
                return {
                    "error": {"code": -32020, "message": "Mcp-Method does not match"}
                }
            if method == "tools/call":
                want = (req.get("params") or {}).get("name")
                if self.headers.get("Mcp-Name") != want:
                    return {
                        "error": {"code": -32020, "message": "Mcp-Name does not match"}
                    }
            return None

        def reply(self, msg, code, req):
            body = json.dumps(msg).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = httpd.server_address[1]
    if opts.port_file:
        tmp = opts.port_file + ".tmp"
        with open(tmp, "w") as f:
            f.write(str(port))
        os.replace(tmp, opts.port_file)
    else:
        sys.stderr.write("port %d\n" % port)
    httpd.serve_forever()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--mode",
        default="ok",
        choices=[
            "ok",
            "bad",
            "headers",
            "invalid-headers",
            "flood",
            "crash",
            "silent",
            "die-on-call",
            "links",
            "ping-first",
        ],
    )
    ap.add_argument("--log", default=None)
    ap.add_argument(
        "--extras", action="store_true", help="offer resources and prompts too"
    )
    ap.add_argument(
        "--era",
        default="legacy",
        choices=["legacy", "modern", "unsupported", "old-only"],
    )
    ap.add_argument(
        "--legacy-version",
        default="2025-06-18",
        help="the version initialize answers with",
    )
    ap.add_argument(
        "--reject-status",
        type=int,
        default=0,
        help="answer server/discover with this HTTP status and -32601",
    )
    ap.add_argument(
        "--report-fds",
        default=None,
        help="write what each inherited descriptor points at, then serve",
    )
    ap.add_argument("--http", action="store_true", help="serve over HTTP")
    ap.add_argument("--sse", action="store_true", help="answer with SSE")
    ap.add_argument("--session", action="store_true", help="use a session id")
    ap.add_argument("--delay-ms", type=int, default=0)
    ap.add_argument("--status", type=int, default=0)
    ap.add_argument(
        "--expire-once",
        action="store_true",
        help="answer the first tools/call with 404, as an expired session does",
    )
    ap.add_argument("--port-file", default=None)
    opts = ap.parse_args()

    if opts.http:
        serve_http(opts)
        return

    if opts.report_fds:
        fds = sorted(int(fd) for fd in os.listdir("/proc/self/fd"))
        seen = {}
        for fd in fds:
            try:
                seen[fd] = os.readlink("/proc/self/fd/%d" % fd)
            except OSError:
                pass
        with open(opts.report_fds, "w") as f:
            json.dump(seen, f)

    if opts.mode == "crash":
        sys.exit(3)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if opts.log:
            with open(opts.log, "a") as f:
                f.write(line + "\n")
        req = json.loads(line)
        method = req.get("method")
        if opts.era != "legacy":
            answer = modern_gate(opts, req)
            if answer is not None:
                if "id" in req:
                    msg = {"jsonrpc": "2.0", "id": req["id"]}
                    msg.update(answer)
                    send(msg)
                continue
        if method == "initialize":
            if opts.mode == "silent":
                time.sleep(30)
                return
            reply(
                req,
                {
                    "protocolVersion": opts.legacy_version,
                    "capabilities": caps(opts),
                    "serverInfo": {"name": "fake", "version": "1.0"},
                },
            )
        elif method == "tools/list":
            reply(req, {"tools": tools_for(opts.mode)})
        elif opts.extras and method.startswith(("resources/", "prompts/")):
            try:
                reply(req, extra(opts, method, req.get("params") or {}))
            except KeyError as e:
                send(
                    {
                        "jsonrpc": "2.0",
                        "id": req["id"],
                        "error": {"code": -32602, "message": str(e)},
                    }
                )
        elif method == "tools/call":
            params = req.get("params") or {}
            if opts.mode == "die-on-call":
                sys.exit(4)
            if opts.mode == "ping-first":
                send({"jsonrpc": "2.0", "id": req["id"], "method": "ping"})
                answer = sys.stdin.readline()
                if opts.log:
                    with open(opts.log, "a") as f:
                        f.write(answer.strip() + "\n")
                if json.loads(answer).get("result") != {}:
                    sys.exit(5)
            result = call(opts.mode, params.get("name"), params.get("arguments") or {})
            if result is None:
                send(
                    {
                        "jsonrpc": "2.0",
                        "id": req["id"],
                        "error": {"code": -32602, "message": "no such tool"},
                    }
                )
            else:
                reply(req, result)
        elif "id" in req:
            send(
                {
                    "jsonrpc": "2.0",
                    "id": req["id"],
                    "error": {"code": -32601, "message": "no such method"},
                }
            )


if __name__ == "__main__":
    main()
