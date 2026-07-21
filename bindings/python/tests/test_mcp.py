"""MCP reaches Python (issue #95).

The C++ side has a full MCP client — stdio and HTTP, with the parts that are
actually fiddly already handled: fd hygiene across fork, SSE frame parsing, the
protocol-version header, the 16 MB line cap. Python had none of it, and the
reason was never a design decision — "Python users can just reach for fastmcp"
was an assumption nobody revisited.

These talk to real MCP servers: a subprocess over stdio, and a threaded HTTP
server, both doing a real handshake. Not mocks — a mock would prove the binding
compiles, not that it speaks the protocol.

ON CONCURRENCY, WHICH IS WHERE #95 MEETS #96

MCP tools are network round-trips, exactly the case where concurrent dispatch
pays. But the two transports are not the same, and pretending otherwise would be
a lie told with a passing test:

  - **stdio has one pipe, multiplexed by JSON-RPC id.** Writes are framed and a
    single reader routes out-of-order replies to their callers. Calls overlap
    when the subprocess itself handles them concurrently.

  - **HTTP has no such bottleneck.** Each call is its own request, and MCPTool
    is a real C++ AsyncTool, so three calls overlap.
"""

import json
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

import neograph_engine as ng


SERVER = os.path.join(os.path.dirname(__file__), "mcp_echo_server.py")

# `mcp` is a submodule of the C extension, not an importable package path, so
# importorskip cannot see it.
if not hasattr(ng, "mcp"):
    pytest.skip("binding built with -DNEOGRAPH_BUILD_MCP=OFF",
                allow_module_level=True)
mcp = ng.mcp


# ── A real MCP server over HTTP, threaded so calls can actually overlap ──────

TOOLS = [
    {"name": "echo", "description": "Echo the input back",
     "inputSchema": {"type": "object",
                     "properties": {"text": {"type": "string"}},
                     "required": ["text"]}},
    {"name": "slow_echo", "description": "Echo the input back, slowly",
     "inputSchema": {"type": "object",
                     "properties": {"text": {"type": "string"},
                                    "delay": {"type": "number"}},
                      "required": ["text"]}},
    {"name": "rich", "title": "Rich result",
     "description": "Return structured and non-text content",
     "icons": [{"src": "data:image/png;base64,AA==", "mimeType": "image/png"}],
     "inputSchema": {"type": "object"},
     "outputSchema": {"type": "object", "required": ["answer"],
                      "properties": {"answer": {"type": "integer"}}},
     "annotations": {"readOnlyHint": True},
     "execution": {"taskSupport": "forbidden"},
     "_meta": {"catalog": "python"}},
]


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass   # keep pytest's output readable

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        request = json.loads(self.rfile.read(length) or b"{}")
        method = request.get("method")
        params = request.get("params") or {}

        if method != "initialize":
            if (self.headers.get("Mcp-Session-Id") != "python-session" or
                    self.headers.get("MCP-Protocol-Version") != "2025-11-25"):
                body = json.dumps({"jsonrpc": "2.0", "id": request.get("id"),
                                   "error": {"code": -32002,
                                             "message": "missing session state"}}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

        if method == "initialize":
            result = {"protocolVersion": "2025-11-25",
                      "capabilities": {"tools": {}},
                      "serverInfo": {"name": "http-echo", "version": "0.1.0"}}
        elif method == "tools/list":
            if "cursor" not in params:
                result = {"tools": TOOLS[:2], "nextCursor": "page-2",
                          "_meta": {"page": 1}}
            else:
                assert params["cursor"] == "page-2"
                result = {"tools": TOOLS[2:], "_meta": {"page": 2}}
        elif method == "tools/call":
            args = params.get("arguments") or {}
            if params.get("name") == "slow_echo":
                time.sleep(float(args.get("delay", 0.3)))
            if params.get("name") == "rich":
                result = {"content": [
                              {"type": "text", "text": "answer: 42"},
                              {"type": "image", "data": "AA==",
                               "mimeType": "image/png"}],
                          "structuredContent": {"answer": 42},
                          "isError": False,
                          "_meta": {"trace": "python-trace"}}
            else:
                result = {"content": [{"type": "text",
                                        "text": args.get("text", "")}],
                          "isError": False}
        else:
            result = None

        if "id" not in request:          # a notification gets no reply
            self.send_response(202)
            self.end_headers()
            return

        body = json.dumps({"jsonrpc": "2.0", "id": request["id"],
                           "result": result}).encode()
        self.send_response(200)
        if method == "initialize":
            self.send_header("Mcp-Session-Id", "python-session")
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module")
def http_url():
    port = _free_port()
    server = ThreadingHTTPServer(("127.0.0.1", port), _Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{port}"
    server.shutdown()
    server.server_close()


@pytest.fixture
def stdio_client():
    client = mcp.MCPClient([sys.executable, SERVER])
    assert client.initialize("neograph-test"), "the MCP handshake failed"
    return client


@pytest.fixture
def serial_stdio_client():
    client = mcp.MCPClient([sys.executable, SERVER, "--serial"])
    assert client.initialize("neograph-test"), "the MCP handshake failed"
    return client


# ── Helpers ─────────────────────────────────────────────────────────────────

def _graph_calling(tool_name, count, arguments='{"text": "x"}'):
    class Caller(ng.GraphNode):
        def run(self, _input):
            return [ng.ChannelWrite("messages", [{
                "role": "assistant",
                "content": "",
                "tool_calls": [
                    {"id": str(i), "name": tool_name, "arguments": arguments}
                    for i in range(count)
                ],
            }])]

        def get_name(self):
            return "llm"

    ng.NodeFactory.register_type("mcp_llm", lambda _n, _c, _x: Caller())
    return {
        "name": "mcp_graph",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "mcp_llm"}, "tools": {"type": "tool_dispatch"}},
        "edges": [
            {"from": "__start__", "to": "llm"},
            {"from": "llm", "to": "tools"},
            {"from": "tools", "to": "__end__"},
        ],
    }


def _run(definition, tools, thread_id, expect_text=None):
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(tools=tools))
    cfg = ng.RunConfig()
    cfg.thread_id = thread_id
    started = time.perf_counter()
    result = engine.run(cfg)
    elapsed = time.perf_counter() - started
    tool_msgs = [m for m in result.output["channels"]["messages"]["value"]
                 if m.get("role") == "tool"]

    # Timing assertions are worthless without this. A tool that fails instantly
    # is very fast, and would otherwise sail straight through "it overlapped" —
    # which is exactly what happened when I mutated the C++-tool passthrough
    # away: every call errored, the run took 0.00s, and the concurrency test
    # still passed. So every run here must prove the calls actually SUCCEEDED.
    for m in tool_msgs:
        assert "error" not in m["content"], (
            f"the tool call failed: {m['content']}")
        if expect_text is not None:
            assert expect_text in m["content"], (
                f"expected {expect_text!r} in the tool result, got {m['content']!r}")

    return tool_msgs, elapsed


# ── The binding exists at all ───────────────────────────────────────────────

def test_the_module_exists():
    """And it must survive `pip install`, not only a source build — which is why
    pyproject.toml had to flip NEOGRAPH_BUILD_MCP to ON at the same time.
    Binding the classes while the wheel compiles them out would be worse than
    the honest absence."""
    assert hasattr(ng, "mcp")
    assert hasattr(ng.mcp, "MCPClient")


# ── stdio ───────────────────────────────────────────────────────────────────

def test_tools_are_discovered_from_the_server(stdio_client):
    tools = stdio_client.get_tools()

    assert sorted(t.get_name() for t in tools) == ["echo", "slow_echo"]

    echo = next(t for t in tools if t.get_name() == "echo")
    assert echo.get_definition().name == "echo"
    assert echo.get_definition().description == "Echo the input back"


def test_a_tool_can_be_called_directly(stdio_client):
    result = stdio_client.call_tool("echo", {"text": "hello mcp"})

    assert result["content"][0]["text"] == "hello mcp"


def test_a_graph_can_use_a_remote_tool(stdio_client):
    """The point of the exercise: a remote catalogue, handed to a node.

    The tools come back as C++ Tool objects, not Python ones. They have to reach
    the engine as what they are — wrapping them in the Python-tool trampoline
    would work, and would quietly cost them their concurrency.
    """
    definition = _graph_calling("echo", 1, '{"text": "from the graph"}')

    tool_msgs, _ = _run(definition, stdio_client.get_tools(), "mcp-graph",
                        expect_text="from the graph")

    assert len(tool_msgs) == 1


def test_stdio_calls_overlap_when_server_is_concurrent(stdio_client):
    """The pipe is framed once, while request IDs demultiplex replies."""
    delay = 0.3
    definition = _graph_calling(
        "slow_echo", 3, '{"text": "x", "delay": %s}' % delay)

    tool_msgs, elapsed = _run(definition, stdio_client.get_tools(),
                              "mcp-stdio-concurrent", expect_text="x")

    assert len(tool_msgs) == 3
    print(f"\n[MEASURE] stdio, 3 MCP calls x {delay}s: {elapsed:.2f}s "
          f"(request-ID multiplexed)")
    assert elapsed < 2 * delay, (
        f"stdio requests serialized unexpectedly: {elapsed:.2f}s")


def test_stdio_calls_wait_when_server_is_serial(serial_stdio_client):
    """Request-ID multiplexing cannot make a serial server execute in parallel."""
    delay = 0.3
    definition = _graph_calling(
        "slow_echo", 3, '{"text": "x", "delay": %s}' % delay)

    tool_msgs, elapsed = _run(definition, serial_stdio_client.get_tools(),
                              "mcp-stdio-serial", expect_text="x")

    assert len(tool_msgs) == 3
    print(f"\n[MEASURE] serial stdio server, 3 MCP calls x {delay}s: "
          f"{elapsed:.2f}s")
    assert elapsed >= 2.5 * delay, (
        f"serial stdio server unexpectedly overlapped requests: {elapsed:.2f}s")


# ── HTTP ────────────────────────────────────────────────────────────────────

def test_http_tools_are_discovered(http_url):
    client = mcp.MCPClient(http_url)
    assert client.initialize("neograph-test")

    tools = client.get_tools()

    assert sorted(t.get_name() for t in tools) == ["echo", "rich", "slow_echo"]


def test_http_typed_metadata_pagination_and_session(http_url):
    config = mcp.MCPClientConfig()
    config.headers = [("X-Test", "python")]
    client = mcp.MCPClient(http_url + "/mcp", config)

    assert client.initialize("neograph-test")
    assert client.initialize("idempotent")
    init = client.get_initialize_result()
    assert init.protocol_version == "2025-11-25"
    assert init.server_info["name"] == "http-echo"

    first = client.list_tools()
    assert first.next_cursor == "page-2"
    assert first.meta == {"page": 1}
    definitions = client.get_tool_definitions()
    rich_definition = next(d for d in definitions if d.name == "rich")
    assert rich_definition.title == "Rich result"
    assert rich_definition.annotations["readOnlyHint"] is True
    assert rich_definition.meta == {"catalog": "python"}

    rich = next(t for t in client.get_tools() if t.get_name() == "rich")
    result = rich.execute_result({})
    assert result.structured_content == {"answer": 42}
    assert result.content[1]["type"] == "image"
    assert result.meta == {"trace": "python-trace"}
    assert result.is_error is False


def test_http_tool_calls_overlap(http_url):
    """Where #95 meets #96, and the reason anyone wants MCP bound at all.

    Three 0.4 s round-trips take ~0.4 s, not 1.2 s. MCPTool is a real C++
    AsyncTool — it suspends — so the dispatcher overlaps the calls. This only
    holds because the tools reach the engine as C++ tools; had the binding
    wrapped them as Python tools, they would have serialized and MCP would have
    arrived without the thing that makes it worth having.
    """
    delay = 0.4
    client = mcp.MCPClient(http_url)
    assert client.initialize("neograph-test")

    definition = _graph_calling(
        "slow_echo", 3, '{"text": "x", "delay": %s}' % delay)

    tool_msgs, elapsed = _run(definition, client.get_tools(),
                              "mcp-http-concurrent", expect_text="x")

    assert len(tool_msgs) == 3
    print(f"\n[MEASURE] http, 3 MCP calls x {delay}s: {elapsed:.2f}s "
          f"(serial would be {3 * delay:.1f}s, "
          f"speedup {3 * delay / elapsed:.1f}x)")

    assert elapsed < 2 * delay, (
        f"3 HTTP MCP calls took {elapsed:.2f}s; serial is {3 * delay:.1f}s — "
        "the tools were serialized by the binding, not by the server")
