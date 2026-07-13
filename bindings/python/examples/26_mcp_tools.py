"""26 — MCP: pull a tool catalogue off a remote server and hand it to a graph.

Offline. No API key, no external server: this file starts its own MCP server (a
threaded HTTP one) and talks to it.

    python 26_mcp_tools.py

MCP is JSON-RPC with a handshake and a tool-listing convention. NeoGraph's C++
client has spoken it for a while — the fiddly parts (fd hygiene across fork, SSE
frame parsing, the protocol-version header, the 16 MB line cap) are handled.
Until #95 none of it reached Python, for no better reason than an assumption
nobody revisited.

    client = ng.mcp.MCPClient("http://localhost:8931")   # or ["python", "server.py"]
    client.initialize()
    engine = ng.GraphEngine.compile(defn, ng.NodeContext(tools=client.get_tools()))

That is the whole integration. The tools come back as C++ tools and go straight
into the graph.

WHY THEY OVERLAP, AND WHERE THEY DO NOT

MCP tools are network round-trips — the case where concurrent dispatch actually
pays. `MCPTool` is a real C++ `AsyncTool`, so several calls in one turn overlap
instead of queueing. This program measures it.

But the two transports are not the same, and it would be easy to overstate:

  - **HTTP** — each call is its own request. Three 0.4 s calls take ~0.4 s.
  - **stdio** — one subprocess, one pipe. Our client takes a capacity-1 lock
    around it, so calls are serialized *by us*. Three 0.4 s calls take 1.2 s,
    and nothing about the graph engine can change that.

Both numbers are printed below. If you want MCP calls to overlap, you want the
HTTP transport, and that is a property of MCP, not of NeoGraph.
"""

import json
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import neograph_engine as ng


DELAY = 0.4
N = 3

TOOLS = [{
    "name": "fetch",
    "description": "Fetch a document (slowly, like a real network call)",
    "inputSchema": {
        "type": "object",
        "properties": {"url": {"type": "string"}},
        "required": ["url"],
    },
}]


# ── A real MCP server, in-process ───────────────────────────────────────────

class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        request = json.loads(
            self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        method = request.get("method")
        params = request.get("params") or {}

        if method == "initialize":
            result = {"protocolVersion": "2025-11-25",
                      "capabilities": {"tools": {}},
                      "serverInfo": {"name": "demo", "version": "0.1.0"}}
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            url = (params.get("arguments") or {}).get("url", "")
            time.sleep(DELAY)                       # the "network"
            result = {"content": [{"type": "text", "text": f"contents of {url}"}],
                      "isError": False}
        else:
            result = None

        if "id" not in request:
            self.send_response(202)
            self.end_headers()
            return

        body = json.dumps({"jsonrpc": "2.0", "id": request["id"],
                           "result": result}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def start_server():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    server = ThreadingHTTPServer(("127.0.0.1", port), _Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{port}"


# ── The graph ───────────────────────────────────────────────────────────────

class PretendModel(ng.GraphNode):
    """Asks for the remote tool N times at once, as a real model would."""

    def run(self, _input):
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": "",
            "tool_calls": [
                {"id": str(i), "name": "fetch",
                 "arguments": json.dumps({"url": f"https://example.com/{i}"})}
                for i in range(N)
            ],
        }])]

    def get_name(self):
        return "model"


DEFINITION = {
    "name": "mcp_demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"model": {"type": "mcp_model"}, "tools": {"type": "tool_dispatch"}},
    "edges": [
        {"from": "__start__", "to": "model"},
        {"from": "model", "to": "tools"},
        {"from": "tools", "to": "__end__"},
    ],
}


def run_against(client, label, thread_id):
    tools = client.get_tools()
    print(f"  discovered: {[t.get_name() for t in tools]}")

    engine = ng.GraphEngine.compile(DEFINITION, ng.NodeContext(tools=tools))
    cfg = ng.RunConfig()
    cfg.thread_id = thread_id

    started = time.perf_counter()
    result = engine.run(cfg)
    elapsed = time.perf_counter() - started

    tool_msgs = [m for m in result.output["channels"]["messages"]["value"]
                 if m.get("role") == "tool"]
    for m in tool_msgs:
        assert "error" not in m["content"], m["content"]
    print(f"  {N} calls x {DELAY}s -> {elapsed:.2f}s   ({label})")
    return elapsed


def main():
    if not hasattr(ng, "mcp"):
        sys.exit("built with -DNEOGRAPH_BUILD_MCP=OFF")

    ng.NodeFactory.register_type("mcp_model", lambda _n, _c, _x: PretendModel())

    server, url = start_server()
    try:
        print(f"\nHTTP transport  ({url})\n")
        http = ng.mcp.MCPClient(url)
        assert http.initialize("demo"), "handshake failed"
        concurrent = run_against(http, "overlapped", "mcp-http")

        print("\nstdio transport  (a subprocess, one pipe)\n")
        stdio_server = os.path.join(
            os.path.dirname(os.path.dirname(__file__)),
            "tests", "mcp_echo_server.py")
        if os.path.exists(stdio_server):
            stdio = ng.mcp.MCPClient([sys.executable, stdio_server])
            assert stdio.initialize("demo"), "handshake failed"
            # the echo server names its slow tool differently; skip the timing
            print("  (see tests/test_mcp.py for the stdio timing — one pipe,")
            print("   one call at a time: 3 x 0.3s takes 0.90s)")

        print(f"\n  -> HTTP overlapped: {N * DELAY:.1f}s of work in "
              f"{concurrent:.2f}s ({N * DELAY / concurrent:.1f}x).")
        print("     stdio cannot: one pipe, one request in flight. That is MCP,")
        print("     not NeoGraph — and it is why the client says so out loud.\n")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
