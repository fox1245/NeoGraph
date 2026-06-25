#!/usr/bin/env python3
"""Concurrent stdio JSON-RPC server for MCP overlap tests.

Like mcp_stdio_echo.py but each `tools/call` is handled on its own
thread that sleeps `arguments.delay_ms` before replying. This models an
I/O-bound MCP server that CAN process several requests at once — so a
client that multiplexes the single pipe sees wall ≈ max(delay), while a
client that serialises the round trip sees wall ≈ sum(delay).

Pure stdlib so it runs anywhere Python 3 is installed.
"""
import json
import sys
import threading
import time

_write_lock = threading.Lock()


def reply(obj):
    line = json.dumps(obj) + "\n"
    with _write_lock:                 # frame each response atomically
        sys.stdout.write(line)
        sys.stdout.flush()


def handle_call(rid, params):
    delay_ms = 0
    args = params.get("arguments", {}) or {}
    try:
        delay_ms = int(args.get("delay_ms", 0))
    except (TypeError, ValueError):
        delay_ms = 0
    if delay_ms > 0:
        time.sleep(delay_ms / 1000.0)
    reply({
        "jsonrpc": "2.0",
        "id": rid,
        "result": {
            "content": [{
                "type": "text",
                "text": json.dumps({
                    "tool": params.get("name", ""),
                    "args": args,
                }),
            }],
        },
    })


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
                "serverInfo": {"name": "stdio-slow", "version": "0.1.0"},
                "capabilities": {},
            },
        })
        return

    if method == "tools/call":
        # Off-thread so sibling calls overlap their sleeps.
        threading.Thread(target=handle_call, args=(rid, params),
                         daemon=True).start()
        return

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
