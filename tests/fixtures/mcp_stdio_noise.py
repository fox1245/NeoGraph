"""Stdio MCP server that writes text to stdout that is not JSON-RPC.

Usage: mcp_stdio_noise.py MODE [COUNT]
  banner   a banner before the handshake and a log line before each reply
  flood    COUNT log lines before the initialize reply
  cut      the initialize reply is cut off in the middle
  split    the initialize reply is pretty-printed over several lines
  prefixed a log fragment, without a newline, in front of the initialize reply
  scalar   a JSON string where a message belongs
"""
import json
import os
import sys
import threading
import time


def watchdog():
    time.sleep(10)
    os._exit(0)


threading.Thread(target=watchdog, daemon=True).start()

mode = sys.argv[1]
count = int(sys.argv[2]) if len(sys.argv) > 2 else 0


def say(text):
    sys.stdout.write(text)
    sys.stdout.flush()


if mode == "banner":
    say("example-server 1.0 starting\n")
    say("listening on stdio\n")

for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    method = request["method"]
    if method == "initialize":
        result = {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                  "serverInfo": {"name": "noise", "version": "1"}}
    else:
        result = {"tools": [{"name": "probe", "description": "probe",
                              "inputSchema": {"type": "object"}}]}
    reply = {"jsonrpc": "2.0", "id": request["id"], "result": result}
    if mode == "banner":
        say("[INFO] handling " + method + "\n")
    elif mode == "flood":
        for index in range(count):
            say("log line " + str(index) + "\n")
    elif mode == "cut":
        say(json.dumps(reply)[:40] + "\n")
        continue
    elif mode == "split":
        say(json.dumps(reply, indent=2) + "\n")
        continue
    elif mode == "prefixed":
        say("ready ")
    elif mode == "scalar":
        say('"ready"\n')
        continue
    say(json.dumps(reply) + "\n")
