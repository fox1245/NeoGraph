#!/usr/bin/env python3
"""Stdio MCP server whose tools fail in the ways MCP allows.

`is_error` returns a tool result with isError set, `rpc_error` answers
tools/call with a JSON-RPC server error (-32000), and `ok` succeeds.
`invalid_params`, `unknown_method` and `internal_error` answer with the
reserved JSON-RPC codes -32602, -32601 and -32603. `marked_invalid` returns an
isError result that the server marks as an invalid request in
`_meta["io.neograph/failure"]`, as NeoGraph's MCPServer does for arguments that
do not match a tool's input schema. Standard library only.
"""
import json
import sys

TOOLS = [
    {"name": name, "description": name, "inputSchema": {"type": "object"}}
    for name in ("ok", "is_error", "rpc_error", "invalid_params", "unknown_method",
                 "internal_error", "marked_invalid")
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
        elif name == "marked_invalid":
            reply({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": "missing required property value"}],
                "isError": True,
                "_meta": {"io.neograph/failure": {
                    "code": "NG.InvalidRequest", "retryable": False}}}})
        elif name in ("invalid_params", "unknown_method", "internal_error"):
            code, message = {
                "invalid_params": (-32602, "Invalid params: value is required"),
                "unknown_method": (-32601, "Method not found: tools/call"),
                "internal_error": (-32603, "Internal error: the handler crashed"),
            }[name]
            reply({"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}})
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
