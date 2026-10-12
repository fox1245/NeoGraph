"""Stdio MCP server for client conformance tests (stdlib only).

    mcp_stdio_conformance.py RECORD_FILE

Every line it reads is handled in order; events the tests check are appended
to RECORD_FILE:
  - "eof" when its stdin closes, "sigterm" when it gets SIGTERM;
  - "cancelled <id>" when notifications/cancelled names a pending slow call.
Tools:
  - ping_back: sends the client a ping whose id equals the call's own id and
    an undeclared roots/list request, waits for both responses and returns
    them as the call's text;
  - slow: never answers; waits for its cancellation.
"""
import json
import os
import queue
import signal
import sys
import threading
import time

record_path = sys.argv[1]


def record(text):
    with open(record_path, "a", encoding="utf-8") as handle:
        handle.write(text + "\n")


def on_term(_signum, _frame):
    record("sigterm")
    os._exit(0)


if os.name != "nt":
    signal.signal(signal.SIGTERM, on_term)

lines = queue.Queue()


def read():
    for line in sys.stdin:
        lines.put(line)
    lines.put(None)


threading.Thread(target=read, daemon=True).start()


def send(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def answer(request_id, result):
    send({"jsonrpc": "2.0", "id": request_id, "result": result})


slow_calls = set()
deadline = time.time() + 30
while time.time() < deadline:
    try:
        line = lines.get(timeout=0.5)
    except queue.Empty:
        continue
    if line is None:
        record("eof")
        break
    message = json.loads(line)
    method = message.get("method")
    if method == "notifications/cancelled":
        request_id = message["params"]["requestId"]
        if request_id in slow_calls:
            record(f"cancelled {request_id}")
        continue
    if method is None or "id" not in message:
        continue
    if method == "initialize":
        answer(message["id"], {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                               "serverInfo": {"name": "conformance", "version": "1"}})
    elif method == "tools/call" and message["params"]["name"] == "ping_back":
        call_id = message["id"]
        send({"jsonrpc": "2.0", "id": call_id, "method": "ping"})
        send({"jsonrpc": "2.0", "id": "r-1", "method": "roots/list"})
        replies = []
        while len(replies) < 2:
            reply = json.loads(lines.get(timeout=5))
            if "method" not in reply:
                replies.append(reply)
        answer(call_id, {"content": [{"type": "text", "text": json.dumps(replies)}]})
    elif method == "tools/call" and message["params"]["name"] == "slow":
        slow_calls.add(message["id"])
    else:
        answer(message["id"], {"content": []})
sys.exit(0)
