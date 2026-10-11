#!/usr/bin/env python3
"""Stdio MCP server whose tools fail in the two ways MCP allows.

`is_error` returns a tool result with isError set, `rpc_error` answers
tools/call with a JSON-RPC error, and `ok` succeeds. Standard library only.
"""
import json
import sys

TOOLS = [
    {"name": name, "description": name, "inputSchema": {"type": "object"}}
    for name in ("ok", "is_error", "rpc_error")
]


def reply(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def handle(request):
    if "id" not in request:
        return
    rid = request["id"]
    method = request.get("method", "")
    params = request.get("params") or {}
    if method == "initialize":
        reply({"jsonrpc": "2.0", "id": rid, "result": {
            "protocolVersion": "2025-11-25",
            "serverInfo": {"name": "failures", "version": "1"},
            "capabilities": {"tools": {}},
        }})
    elif method == "tools/list":
        reply({"jsonrpc": "2.0", "id": rid, "result": {"tools": TOOLS}})
    elif method == "tools/call":
        name = params.get("name")
        if name == "ok":
            reply({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": "fine"}]}})
        elif name == "is_error":
            reply({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": "remote tool refused"}],
                "isError": True}})
        else:
            reply({"jsonrpc": "2.0", "id": rid, "error": {
                "code": -32000, "message": "server exploded"}})
    else:
        reply({"jsonrpc": "2.0", "id": rid, "error": {
            "code": -32601, "message": "method not found: " + method}})


for line in sys.stdin:
    line = line.strip()
    if line:
        handle(json.loads(line))
