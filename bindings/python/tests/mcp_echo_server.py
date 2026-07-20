"""A minimal MCP server over stdio, for tests. No dependencies, no network.

Speaks the subset the client actually exercises: `initialize`, the
`notifications/initialized` acknowledgement, `tools/list`, and `tools/call`.
That is the whole of MCP that matters here — it is JSON-RPC with a handshake and
a tool-listing convention, which is precisely why "Python users can just reach
for fastmcp" was never a real design boundary.

`slow_echo` sleeps, so a test can prove that several MCP tool calls in one turn
overlap rather than queueing.
"""

import json
import sys
import threading
import time

_write_lock = threading.Lock()
_state_lock = threading.Lock()
_initialized = False
_initializing = False


TOOLS = [
    {
        "name": "echo",
        "description": "Echo the input back",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "slow_echo",
        "description": "Echo the input back, slowly",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"},
                           "delay": {"type": "number"}},
            "required": ["text"],
        },
    },
]


def write_response(response):
    with _write_lock:
        sys.stdout.write(json.dumps(response) + "\n")
        sys.stdout.flush()


def handle(request):
    global _initialized, _initializing
    method = request.get("method")
    params = request.get("params") or {}

    if method == "initialize":
        with _state_lock:
            if _initialized or _initializing:
                return None
            _initializing = True
        return {
            "protocolVersion": "2025-11-25",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "echo-server", "version": "0.1.0"},
        }

    if method == "tools/list":
        with _state_lock:
            if not _initialized:
                return None
        return {"tools": TOOLS}

    if method == "tools/call":
        with _state_lock:
            if not _initialized:
                return None
        name = params.get("name")
        args = params.get("arguments") or {}
        if name == "slow_echo":
            time.sleep(float(args.get("delay", 0.3)))
            text = args.get("text", "")
        elif name == "echo":
            text = args.get("text", "")
        else:
            return {"isError": True,
                    "content": [{"type": "text", "text": f"no such tool: {name}"}]}
        return {"content": [{"type": "text", "text": text}], "isError": False}

    return None


def handle_request(request):
    result = handle(request)
    if result is None:
        response = {"jsonrpc": "2.0", "id": request["id"],
                    "error": {"code": -32601, "message": "method not found"}}
    else:
        response = {"jsonrpc": "2.0", "id": request["id"], "result": result}
    write_response(response)


def main():
    global _initialized, _initializing
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            continue

        # Notifications carry no id and get no reply — replying to one is a
        # protocol violation the client is entitled to be upset about.
        if "id" not in request:
            if request.get("method") == "notifications/initialized":
                with _state_lock:
                    if _initializing:
                        _initialized = True
                        _initializing = False
            continue

        # The write lock preserves one-line framing while request IDs let the
        # client correlate out-of-order replies.
        threading.Thread(target=handle_request, args=(request,),
                         daemon=True).start()


if __name__ == "__main__":
    main()
