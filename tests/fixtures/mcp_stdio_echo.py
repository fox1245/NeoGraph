#!/usr/bin/env python3
"""Minimal stdio JSON-RPC echo server for MCPClient stdio tests.

Reads one JSON object per line on stdin, replies one per line on
stdout. Notifications (no id) are silently consumed. Calls echo back
the method name in result.echo. The 'tools/list' method returns a
canned tool list so MCPClient::get_tools() round-trips. The 'tools/call'
method echoes back the tool name + arguments.

No external dependencies — pure stdlib so the test fixture runs in any
Ubuntu/macOS Python 3.x without fastmcp or a venv.
"""
import json
import sys


def reply(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def handle(req):
    if "id" not in req:
        return  # notification

    rid = req["id"]
    method = req.get("method", "")
    params = req.get("params", {}) or {}

    if method == "initialize":
        reply({
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "protocolVersion": "2025-03-26",
                "serverInfo": {"name": "stdio-echo", "version": "0.1.0"},
                "capabilities": {},
            },
        })
        return

    if method == "tools/list":
        reply({
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "tools": [{
                    "name": "echo",
                    "description": "Echo input back",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "msg": {"type": "string"},
                        },
                    },
                }],
            },
        })
        return

    if method == "tools/call":
        reply({
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "content": [{
                    "type": "text",
                    "text": json.dumps({
                        "tool": params.get("name", ""),
                        "args": params.get("arguments", {}),
                    }),
                }],
            },
        })
        return

    # Unknown method
    reply({
        "jsonrpc": "2.0",
        "id": rid,
        "error": {"code": -32601, "message": f"method not found: {method}"},
    })


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            continue
        handle(req)


if __name__ == "__main__":
    main()
