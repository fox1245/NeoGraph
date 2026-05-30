#!/usr/bin/env python3
"""Minimal MCP stdio server — pure Python stdlib, NO fastmcp / no mcp SDK.

Implements just enough of the Model Context Protocol over newline-delimited
JSON-RPC (stdin/stdout) to drive NeoGraph's built-in MCP *client*:
  - initialize
  - tools/list
  - tools/call  (get_current_time / calculate / get_weather)

The point of this cookbook: NeoGraph's MCP client needs *nothing* on the
peer side except a process that speaks the wire protocol. A "real" MCP
server can be fastmcp, the official `mcp` SDK, or — as shown here — a
~60-line stdlib script with zero pip installs. Swap this file for your
own server and the NeoGraph side is unchanged.
"""
import json
import sys
from datetime import datetime, timezone

TOOLS = [
    {
        "name": "get_current_time",
        "description": "Get the current UTC date and time (ISO format).",
        "inputSchema": {
            "type": "object",
            "properties": {"timezone": {"type": "string"}},
        },
    },
    {
        "name": "calculate",
        "description": "Evaluate a simple math expression (+ - * / ** % and parens).",
        "inputSchema": {
            "type": "object",
            "properties": {"expression": {"type": "string"}},
            "required": ["expression"],
        },
    },
    {
        "name": "get_weather",
        "description": "Return deterministic demo weather for a city.",
        "inputSchema": {
            "type": "object",
            "properties": {"city": {"type": "string"}},
            "required": ["city"],
        },
    },
]


def call(name, args):
    if name == "get_current_time":
        tz = args.get("timezone", "UTC")
        return f"{datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')} ({tz})"
    if name == "calculate":
        expr = args.get("expression", "")
        allowed = set("0123456789+-*/.()% ")
        if not all(c in allowed for c in expr):
            return "Error: invalid characters"
        try:
            return str(eval(expr))  # safe: only digits/operators allowed above
        except Exception as e:  # noqa: BLE001
            return f"Error: {e}"
    if name == "get_weather":
        # Deterministic so the demo output is reproducible.
        return f"{args.get('city', '?')}: 22C, clear (demo)"
    return f"(unknown tool {name})"


def reply(rid, result):
    sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": rid, "result": result}) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        method = msg.get("method")
        rid = msg.get("id")
        if method == "initialize":
            reply(rid, {
                "protocolVersion": "2025-11-25",
                "serverInfo": {"name": "min-stdlib-mcp", "version": "0.1.0"},
                "capabilities": {"tools": {}},
            })
        elif method == "notifications/initialized":
            pass  # notification, no reply
        elif method == "tools/list":
            reply(rid, {"tools": TOOLS})
        elif method == "tools/call":
            p = msg.get("params", {})
            text = call(p.get("name", ""), p.get("arguments", {}) or {})
            reply(rid, {"content": [{"type": "text", "text": text}], "isError": False})
        elif rid is not None:
            sys.stdout.write(json.dumps({
                "jsonrpc": "2.0", "id": rid,
                "error": {"code": -32601, "message": f"method not found: {method}"},
            }) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
