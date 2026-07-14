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
import time


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


def handle(request):
    method = request.get("method")
    params = request.get("params") or {}

    if method == "initialize":
        return {
            "protocolVersion": "2025-11-25",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "echo-server", "version": "0.1.0"},
        }

    if method == "tools/list":
        return {"tools": TOOLS}

    if method == "tools/call":
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


def main():
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
            continue

        result = handle(request)
        if result is None:
            response = {"jsonrpc": "2.0", "id": request["id"],
                        "error": {"code": -32601, "message": "method not found"}}
        else:
            response = {"jsonrpc": "2.0", "id": request["id"], "result": result}

        sys.stdout.write(json.dumps(response) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
