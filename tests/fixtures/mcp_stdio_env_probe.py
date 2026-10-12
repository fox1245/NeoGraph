#!/usr/bin/env python3
"""A stdio MCP server whose one tool reports environment variables.

The ACP server tests start it as a client-supplied MCP server to check which
of the agent's environment variables it inherits. `env` takes {"names": [...]}
and returns {name: value or null}. Standard library only.
"""
import json
import os
import sys


def reply(rid, result=None, error=None):
    message = {"jsonrpc": "2.0", "id": rid}
    if error is not None:
        message["error"] = error
    else:
        message["result"] = result
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


TOOL = {
    "name": "env",
    "description": "Report the named environment variables",
    "inputSchema": {
        "type": "object",
        "properties": {"names": {"type": "array", "items": {"type": "string"}}},
    },
}

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    request = json.loads(line)
    if "id" not in request:
        continue
    rid, method = request["id"], request.get("method", "")
    params = request.get("params") or {}
    if method == "initialize":
        reply(rid, {"protocolVersion": "2025-03-26",
                    "serverInfo": {"name": "env-probe", "version": "0.1.0"},
                    "capabilities": {"tools": {}}})
    elif method == "tools/list":
        reply(rid, {"tools": [TOOL]})
    elif method == "tools/call":
        names = (params.get("arguments") or {}).get("names") or []
        found = {name: os.environ.get(name) for name in names}
        reply(rid, {"content": [{"type": "text", "text": json.dumps(found)}]})
    else:
        reply(rid, error={"code": -32601, "message": "method not found"})
