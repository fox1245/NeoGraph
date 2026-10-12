#!/usr/bin/env python3
"""A small MCP server for the NeoGraph examples, written with the standard library only.

Nothing to install: no fastmcp, no `mcp` package. NeoGraph's MCP client needs nothing from
the other side except a process that speaks the wire protocol, and this file is that process
in about 200 lines. It serves a few deterministic tools, so every example output is stable.

  python3 examples/mcp_demo_server.py                 stdio: a client starts it as a child
  python3 examples/mcp_demo_server.py --http 0        Streamable HTTP on a free port
  python3 examples/mcp_demo_server.py --http 8765     Streamable HTTP on port 8765
  python3 examples/mcp_demo_server.py --tools calculate,get_weather      serve only these

With --http the first line on stdout is `listening http://127.0.0.1:<port>/mcp`.

The tools:
  calculate(expression)   + - * / ** % and parentheses; division by zero is an `isError` result
  get_weather(city)       a fixed reading per city
  get_current_time(timezone)   a fixed demo clock, so the output never changes
  kb_lookup(topic), save_note(content), list_notes()   a tiny knowledge base and a notepad

A call with a missing required argument is a JSON-RPC error (-32602), an unknown tool is also
-32602, and a tool that fails is a normal result with `isError: true`. The three are different
things on the wire, and the examples show how NeoGraph hands each to the model.

Other scripts reuse this file: `from mcp_demo_server import DemoServer, Tool`.
"""
import argparse
import ast
import json
import operator
import sys
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SUPPORTED = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")


class Tool:
    def __init__(self, name, description, properties, required, handler):
        self.name = name
        self.handler = handler
        self.required = list(required)
        self.definition = {
            "name": name,
            "description": description,
            "inputSchema": {"type": "object", "properties": properties, "required": self.required},
        }


class ToolFailure(Exception):
    """A tool that ran and failed: reported as a result with isError true."""


class DemoServer:
    """The protocol part: initialize, tools/list, tools/call, ping. Transport-independent."""

    def __init__(self, name, tools):
        self.name = name
        self.tools = {tool.name: tool for tool in tools}

    def handle(self, message):
        """Return the response for a request, or None for a notification or a response."""
        if not isinstance(message, dict):
            return error_response(None, -32600, "Invalid Request")
        method, request_id = message.get("method"), message.get("id")
        if method is None or request_id is None:
            return None  # notifications/initialized, cancellations, responses: nothing to answer
        params = message.get("params") or {}
        if method == "initialize":
            asked = params.get("protocolVersion")
            return result_response(request_id, {
                "protocolVersion": asked if asked in SUPPORTED else SUPPORTED[0],
                "serverInfo": {"name": self.name, "version": "1.0.0"},
                "capabilities": {"tools": {"listChanged": False}},
            })
        if method == "ping":
            return result_response(request_id, {})
        if method == "tools/list":
            return result_response(request_id, {"tools": [t.definition for t in self.tools.values()]})
        if method == "tools/call":
            return self.call(request_id, params)
        return error_response(request_id, -32601, "method not found: %s" % method)

    def call(self, request_id, params):
        tool = self.tools.get(params.get("name"))
        if tool is None:
            return error_response(request_id, -32602, "unknown tool: %s" % params.get("name"))
        arguments = params.get("arguments") or {}
        missing = [key for key in tool.required if key not in arguments]
        if missing:
            return error_response(request_id, -32602, "Invalid params: %s is required" % missing[0])
        try:
            text, failed = str(tool.handler(arguments)), False
        except ToolFailure as failure:
            text, failed = "Error: %s" % failure, True
        return result_response(request_id, {"content": [{"type": "text", "text": text}], "isError": failed})

    def run_stdio(self):
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                reply = self.handle(json.loads(line))
            except json.JSONDecodeError:
                reply = error_response(None, -32700, "Parse error")
            if reply is not None:
                sys.stdout.write(json.dumps(reply) + "\n")
                sys.stdout.flush()

    def run_http(self, port):
        server = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def reply(self, status, body=None, session=None):
                payload = json.dumps(body).encode() if body is not None else b""
                self.send_response(status)
                if body is not None:
                    self.send_header("Content-Type", "application/json")
                if session:
                    self.send_header("Mcp-Session-Id", session)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_POST(self):
                length = int(self.headers.get("Content-Length") or 0)
                try:
                    message = json.loads(self.rfile.read(length) or b"null")
                except json.JSONDecodeError:
                    return self.reply(400, error_response(None, -32700, "Parse error"))
                answer = server.handle(message)
                if answer is None:
                    return self.reply(202)
                opening = isinstance(message, dict) and message.get("method") == "initialize"
                self.reply(200, answer, session=uuid.uuid4().hex if opening else None)

            def do_DELETE(self):
                self.reply(200)

            def do_GET(self):  # no server-initiated event stream
                self.reply(405)

        http = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        http.daemon_threads = True
        print("listening http://127.0.0.1:%d/mcp" % http.server_address[1], flush=True)
        try:
            http.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            http.server_close()


def result_response(request_id, result):
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def error_response(request_id, code, message):
    return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}


# ---- the demo tools --------------------------------------------------------------------------

_OPERATORS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
              ast.Div: operator.truediv, ast.Pow: operator.pow, ast.Mod: operator.mod,
              ast.USub: operator.neg, ast.UAdd: operator.pos}


def _evaluate(node):
    if isinstance(node, ast.Expression):
        return _evaluate(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
        return node.value
    if isinstance(node, ast.BinOp) and type(node.op) in _OPERATORS:
        return _OPERATORS[type(node.op)](_evaluate(node.left), _evaluate(node.right))
    if isinstance(node, ast.UnaryOp) and type(node.op) in _OPERATORS:
        return _OPERATORS[type(node.op)](_evaluate(node.operand))
    raise ToolFailure("unsupported expression")


def calculate(arguments):
    try:
        value = _evaluate(ast.parse(arguments["expression"].strip(), mode="eval"))
    except ZeroDivisionError:
        raise ToolFailure("division by zero")
    except (SyntaxError, ValueError, OverflowError):
        raise ToolFailure("cannot evaluate %r" % arguments["expression"])
    return int(value) if isinstance(value, float) and value == int(value) else value


WEATHER = {"tokyo": "22C, clear", "seoul": "18C, cloudy", "paris": "15C, rain"}
KB = {
    "neograph": "NeoGraph is a C++ graph engine for agents.",
    "mcp": "MCP is a JSON-RPC protocol for discovering and calling tools.",
    "a2a": "A2A is a protocol for agents that call other agents.",
}
NOTES = []


def weather(arguments):
    city = arguments["city"]
    return "%s: %s" % (city, WEATHER.get(city.lower(), "20C, clear"))


def kb_lookup(arguments):
    return KB.get(arguments["topic"].strip().lower(), "(no entry for %s)" % arguments["topic"])


def save_note(arguments):
    NOTES.append(arguments["content"])
    return "note#%d saved" % len(NOTES)


def list_notes(_):
    return "\n".join("#%d %s" % (i, note) for i, note in enumerate(NOTES, 1)) or "(no notes)"


def build_tools():
    text = {"type": "string"}
    return [
        Tool("get_current_time", "Get the current date and time (a fixed demo clock).",
             {"timezone": text}, [], lambda a: "2026-01-01 12:00:00 (%s, demo clock)" % a.get("timezone", "UTC")),
        Tool("calculate", "Evaluate an arithmetic expression with + - * / ** % and parentheses.",
             {"expression": text}, ["expression"], calculate),
        Tool("get_weather", "Get the current weather for a city (fixed demo data).",
             {"city": text}, ["city"], weather),
        Tool("kb_lookup", "Look up a topic in the local knowledge base.", {"topic": text}, ["topic"], kb_lookup),
        Tool("save_note", "Save a note and return its id.", {"content": text}, ["content"], save_note),
        Tool("list_notes", "List every saved note.", {}, [], list_notes),
    ]


def main():
    parser = argparse.ArgumentParser(description="Standard-library MCP server for the NeoGraph examples.")
    parser.add_argument("--http", type=int, metavar="PORT", help="serve Streamable HTTP (0 = free port)")
    parser.add_argument("--tools", help="comma-separated tool names to serve (default: all)")
    args = parser.parse_args()
    tools = build_tools()
    if args.tools:
        wanted = set(args.tools.split(","))
        tools = [tool for tool in tools if tool.name in wanted]
    server = DemoServer("neograph-demo-tools", tools)
    if args.http is None:
        server.run_stdio()
    else:
        server.run_http(args.http)


if __name__ == "__main__":
    main()
