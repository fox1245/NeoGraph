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
pays. Hosts still declare that repeated calls to a named remote tool are safe:
the default policy is keyed-exclusive, so this module explicitly marks
``slow_echo`` re-entrant. The transports then differ:

  - **stdio has one pipe, multiplexed by JSON-RPC id.** Writes are framed and a
    single reader routes out-of-order replies to their callers. Calls overlap
    when the subprocess itself handles them concurrently.

  - **HTTP has no such bottleneck.** Each call is its own request, and MCPTool
    is a real C++ AsyncTool, so declared re-entrant calls overlap.
"""

import json
import os
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

    def do_GET(self):
        # No event stream for messages between calls.
        self.send_response(405)
        self.send_header("Allow", "POST, DELETE")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_DELETE(self):
        # The client ends its session.
        self.send_response(204)
        self.end_headers()

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


@pytest.fixture(scope="module")
def http_url():
    # The server binds port 0 itself: no number is chosen before it holds the socket.
    server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    port = server.server_address[1]
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


def _reentrant_tool_controller(*tool_names):
    """Explicit host policy for remote tools known to support parallel calls."""
    policies = ng.ToolExecutionPolicyRegistry()
    policy = ng.ToolExecutionPolicy()
    policy.concurrency = ng.ToolConcurrency.Reentrant
    for tool_name in tool_names:
        policies.upsert(tool_name, policy)
    return ng.ToolExecutionController(policies)


def _run(definition, tools, thread_id, expect_text=None, reentrant_tools=()):
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(tools=tools))
    if reentrant_tools:
        engine.set_tool_execution_controller(
            _reentrant_tool_controller(*reentrant_tools))
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
    controller = ng.ToolExecutionController()
    assert controller.policies.resolve("unknown").concurrency == (
        ng.ToolConcurrency.KeyedExclusive)


def test_tool_execution_policy_validates_default():
    """The Python method crosses the core shared-library boundary."""
    assert ng.ToolExecutionPolicy().validate() is None

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
                              "mcp-stdio-concurrent", expect_text="x",
                              reentrant_tools=("slow_echo",))

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
                              "mcp-stdio-serial", expect_text="x",
                              reentrant_tools=("slow_echo",))

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



def test_http_tool_calls_serialize_by_default(http_url):
    """Async I/O does not override the host's safe named-tool default."""
    delay = 0.25
    client = mcp.MCPClient(http_url)
    assert client.initialize("neograph-test")
    definition = _graph_calling(
        "slow_echo", 3, '{"text": "x", "delay": %s}' % delay)

    tool_msgs, elapsed = _run(definition, client.get_tools(),
                              "mcp-http-default", expect_text="x")

    assert len(tool_msgs) == 3
    assert elapsed >= 2.5 * delay, (
        f"default named-tool policy unexpectedly overlapped calls: "
        f"{elapsed:.2f}s")


def test_http_tool_calls_overlap(http_url):
    """A server-safe explicit policy unlocks native MCP async overlap.

    Three 0.4 s round-trips take ~0.4 s, not 1.2 s. MCPTool is a real C++
    AsyncTool — it suspends — and the host marks this threaded test server's
    ``slow_echo`` tool Reentrant. Preserving the tool's C++ type keeps that
    opt-in effective; wrapping it as a Python tool would silently lose it.
    """
    delay = 0.4
    client = mcp.MCPClient(http_url)
    assert client.initialize("neograph-test")

    definition = _graph_calling(
        "slow_echo", 3, '{"text": "x", "delay": %s}' % delay)

    tool_msgs, elapsed = _run(definition, client.get_tools(),
                              "mcp-http-concurrent", expect_text="x",
                              reentrant_tools=("slow_echo",))

    assert len(tool_msgs) == 3
    print(f"\n[MEASURE] http, 3 MCP calls x {delay}s: {elapsed:.2f}s "
          f"(serial would be {3 * delay:.1f}s, "
          f"speedup {3 * delay / elapsed:.1f}x)")

    assert elapsed < 2 * delay, (
        f"3 HTTP MCP calls took {elapsed:.2f}s; serial is {3 * delay:.1f}s — "
        "the reentrant policy or native MCP async path regressed")


# ── Cancellation and typed errors (normalization follow-ups) ────────────────

def test_cancelling_the_run_stops_an_in_flight_mcp_call(stdio_client):
    """The run's cancel token reaches the MCP call: the run stops at once as
    cancelled instead of waiting for the server's reply."""
    definition = _graph_calling("slow_echo", 1, '{"text": "x", "delay": 5}')
    engine = ng.GraphEngine.compile(definition,
                                    ng.NodeContext(tools=stdio_client.get_tools()))
    token = ng.CancelToken()
    cfg = ng.RunConfig()
    cfg.thread_id = "mcp-cancel"
    cfg.cancel_token = token
    timer = threading.Timer(0.3, token.cancel)
    timer.start()
    started = time.perf_counter()
    try:
        with pytest.raises(RuntimeError, match="cancel"):
            engine.run(cfg)
    finally:
        timer.cancel()
    assert time.perf_counter() - started < 3.0


def test_an_attempt_timeout_stops_an_in_flight_mcp_call(stdio_client):
    """An attempt timeout cancels the attempt's own token, which reaches the
    MCP call: the attempt stops in time and fails as NG.Timeout, a routable
    failure, instead of outliving its stop deadline (NG.EffectUncertain)."""
    class Caller(ng.GraphNode):
        def run(self, _input):
            return [ng.ChannelWrite("messages", [{
                "role": "assistant", "content": "",
                "tool_calls": [{"id": "slow", "name": "slow_echo",
                                "arguments": '{"text": "x", "delay": 5}'}],
            }])]

        def get_name(self):
            return "llm"

    class Handler(ng.GraphNode):
        def run(self, _input):
            return []

        def get_name(self):
            return "handler"

    ng.NodeFactory.register_type("mcp_timeout_llm", lambda _n, _c, _x: Caller())
    ng.NodeFactory.register_type("mcp_timeout_handler", lambda _n, _c, _x: Handler())
    definition = {
        "schema_version": 1,
        "name": "mcp_attempt_timeout",
        "channels": {"messages": {"reducer": "append"},
                     "failures": {"reducer": "append", "initial": []}},
        "nodes": {"llm": {"type": "mcp_timeout_llm"}, "tools": {"type": "tool_dispatch"},
                  "handler": {"type": "mcp_timeout_handler"}},
        "edges": [{"from": "__start__", "to": "llm"}, {"from": "llm", "to": "tools"},
                  {"from": "tools", "to": "__end__"}, {"from": "handler", "to": "__end__"}],
        "node_retry_policies": {"tools": {"attempt_timeout_ms": 200}},
        "failure_edges": [{"from": "tools", "to": "handler", "error_channel": "failures"}],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(tools=stdio_client.get_tools()))
    started = time.perf_counter()
    result = engine.run(ng.RunConfig(thread_id="mcp-attempt-timeout"))
    assert time.perf_counter() - started < 3.0
    failures = result.output["channels"]["failures"]["value"]
    assert [failure["code"] for failure in failures] == ["NG.Timeout"]


def test_mcp_error_keeps_its_code_and_data(stdio_client):
    with pytest.raises(mcp.MCPError) as raised:
        stdio_client.call_tool_result("rpc_error", {})
    assert raised.value.code == -32099
    assert raised.value.data == {"why": "test"}
    assert "refused by policy" in str(raised.value)


# ── The client's typed failures, bounds and lifecycle (MCP-C-14) ────────────

BOUNDARY = os.path.join(os.path.dirname(__file__), os.pardir, os.pardir, os.pardir,
                        "tests", "fixtures", "mcp_stdio_boundary.py")


def test_a_bounded_call_raises_a_typed_transport_error(stdio_client):
    with pytest.raises(mcp.MCPTransportError) as timed_out:
        stdio_client.call_tool_result("slow_echo", {"text": "x", "delay": 2},
                                      timeout=0.2)
    assert timed_out.value.failure == "timeout"
    assert timed_out.value.http_status == 0

    token = ng.CancelToken()
    threading.Timer(0.2, token.cancel).start()
    with pytest.raises(mcp.MCPTransportError) as cancelled:
        stdio_client.call_tool_result("slow_echo", {"text": "x", "delay": 2},
                                      cancel_token=token)
    assert cancelled.value.failure == "cancelled"
    # The session survives both: the late responses are dropped.
    assert stdio_client.call_tool_result("echo", {"text": "after"}).content[0]["text"] == "after"


def test_shutdown_closes_the_client_for_good():
    client = mcp.MCPClient([sys.executable, SERVER])
    assert client.initialize("neograph-test")
    client.shutdown()
    with pytest.raises(mcp.MCPTransportError) as closed:
        client.call_tool("echo", {"text": "x"})
    assert closed.value.failure == "shutdown"


def test_mcp_error_reports_the_http_status_that_carried_it(stdio_client):
    with pytest.raises(mcp.MCPError) as info:
        stdio_client.call_tool("rpc_error", {})
    assert info.value.http_status == 0


@pytest.mark.skipif(os.name == "nt", reason="the boundary fixture reports POSIX environments")
def test_stdio_config_inherits_the_environment_with_overrides(monkeypatch):
    monkeypatch.setenv("NG_MCP_PY_INHERITED", "parent")
    monkeypatch.setenv("NG_MCP_PY_OVERRIDDEN", "parent")
    config = mcp.StdioClientConfig()
    config.argv = [sys.executable, BOUNDARY, "report"]
    config.environment = [("NG_MCP_PY_OVERRIDDEN", "child")]
    client = mcp.MCPClient(config)
    environment = client.call_tool("probe", {})["environment"]
    assert environment["NG_MCP_PY_INHERITED"] == "parent"
    assert environment["NG_MCP_PY_OVERRIDDEN"] == "child"

    config = mcp.StdioClientConfig()
    config.argv = [sys.executable, BOUNDARY, "report"]
    config.replace_environment = True
    config.environment = [("ONLY", "this")]
    replaced = mcp.MCPClient(config).call_tool("probe", {})["environment"]
    assert replaced.get("ONLY") == "this"
    assert "NG_MCP_PY_INHERITED" not in replaced


def test_tool_list_changed_reaches_the_handler(stdio_client):
    changed = threading.Event()
    stdio_client.set_tool_list_changed_handler(changed.set)
    assert stdio_client.call_tool_result("announce_change", {}).content[0]["text"] == "changed"
    assert changed.wait(5), "notifications/tools/list_changed did not reach the handler"
    stdio_client.set_tool_list_changed_handler(None)

