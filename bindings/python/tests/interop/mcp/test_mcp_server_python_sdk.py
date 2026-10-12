"""Official Python MCP SDK client (mcp==2.3.0) -> NeoGraph MCP server.

Every test talks to a real NeoGraph server binary over stdio or Streamable HTTP
through a recording tee/proxy, so failures carry the exact NeoGraph wire
message. Tests assert the behaviour the official SDK accepts *and* the spec
(MCP 2025-11-25, the revision NeoGraph negotiates) requires.
"""

import asyncio
import contextlib
import json
import logging
import os
import tempfile
import time
import warnings

import pytest

from mcp_sdk_requirement import require_official_python_sdk

# The pinned official SDK (requirements-mcp-interop.txt); skipped without it.
require_official_python_sdk()
anyio = pytest.importorskip("anyio")
pydantic = pytest.importorskip("pydantic")
from mcp import Client, MCPError, StdioServerParameters  # noqa: E402

from mcp_server_interop import (EXAMPLE59, HARNESS, PROBE, missing_binaries,  # noqa: E402
                                stdio_command, transcript_path, wire)

if missing_binaries():
    pytest.skip(f"NEOGRAPH_BUILD_DIR lacks {missing_binaries()}", allow_module_level=True)
TMP = tempfile.gettempdir()

MODES = ["auto", "legacy"]  # auto = SDK default: server/discover probe, then initialize
TRANSPORTS = ["stdio", "http"]


class SdkDiagnostics(logging.Handler):
    """Collect SDK WARNING+ log records: 'accepted without warnings' is part of the oracle."""

    def __init__(self):
        super().__init__(logging.WARNING)
        self.records: list[str] = []

    def emit(self, record):
        if record.name.startswith("mcp"):
            self.records.append(f"{record.name}: {record.getMessage()}")


def run(coro):
    return asyncio.run(coro)


def _target(transport, request, recorded, probe_http):
    """(server, log) for a probe-server connection on `transport`."""
    if transport == "stdio":
        log = transcript_path(f"py-{request.node.name}")
        cmd, args = stdio_command(log, [str(PROBE), "stdio"])
        return StdioServerParameters(command=cmd, args=args, cwd=TMP), log
    url, log = recorded(probe_http)
    return url, log


@contextlib.asynccontextmanager
async def client_for(server, mode):
    diag = SdkDiagnostics()
    logging.getLogger().addHandler(diag)
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            async with Client(server, mode=mode, read_timeout_seconds=20) as client:
                yield client, diag
    finally:
        logging.getLogger().removeHandler(diag)


def responses_for(log, request_id):
    return [r["msg"] for r in wire(log, "s2c")
            if isinstance(r.get("msg"), dict) and r["msg"].get("id") == request_id]


def request_id_of(log, method, name=None):
    for r in wire(log, "c2s"):
        m = r.get("msg")
        if isinstance(m, dict) and m.get("method") == method and (
                name is None or (m.get("params") or {}).get("name") == name):
            return m.get("id")
    return None


@pytest.fixture(params=TRANSPORTS)
def transport(request):
    return request.param


@pytest.fixture(params=MODES)
def mode(request):
    return request.param


# --- lifecycle --------------------------------------------------------------

def test_connect_negotiates_2025_11_25(transport, mode, request, recorded, probe_http):
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, mode) as (c, diag):
            assert c.protocol_version == "2025-11-25"
            assert c.server_info.name == "neograph-mcp-probe"
            assert c.server_capabilities.tools is not None
            assert c.instructions == "NeoGraph MCP interop probe server."
            assert diag.records == [], diag.records
    run(go())
    init = [r["msg"] for r in wire(log, "s2c")
            if isinstance(r.get("msg"), dict) and "protocolVersion" in (r["msg"].get("result") or {})]
    assert init, "no InitializeResult on the wire"


@pytest.fixture
def asks_for_2024_11_05(monkeypatch):
    """The official client, made to offer 2024-11-05 in its initialize request."""
    import mcp.client.session as client_session

    monkeypatch.setattr(client_session, "LATEST_HANDSHAKE_VERSION", "2024-11-05")


def test_a_2024_11_05_client_gets_that_revision(transport, request, recorded, probe_http,
                                                asks_for_2024_11_05):
    """lifecycle: the server answers 2024-11-05, as the official SDK servers do, and sends
    only what that revision defines: no tool title, icons, annotations, execution, _meta or
    outputSchema, no structuredContent, and content blocks it lacks as text."""
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, "legacy") as (c, diag):
            assert c.protocol_version == "2024-11-05"
            listed = await c.list_tools()
            by = {t.name: t for t in listed.tools}
            d = by["decorated"]
            assert (d.title, d.icons, d.annotations, d.execution, d.meta) == (None,) * 5
            assert by["echo"].output_schema is None
            r = await c.call_tool("echo", {"message": "old"})
            assert r.is_error is False and r.structured_content is None
            assert json.loads(r.content[0].text) == {"message": "old"}
            r = await c.call_tool("decorated", {})
            assert [b.type for b in r.content] == ["text", "image", "text"]
            assert diag.records == [], diag.records
    run(go())
    sent = [r["msg"] for r in wire(log, "s2c") if isinstance(r.get("msg"), dict)]
    for msg in sent:
        for tool in (msg.get("result") or {}).get("tools", []):
            assert not {"title", "icons", "annotations", "execution", "_meta",
                        "outputSchema"} & set(tool), tool


# --- tools/list --------------------------------------------------------------

def test_list_tools_preserves_2025_11_25_tool_fields(transport, mode, request, recorded, probe_http):
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, mode) as (c, diag):
            listed = await c.list_tools()
            by = {t.name: t for t in listed.tools}
            assert set(by) == {"bad_output", "decorated", "echo", "echo_text", "fail", "slow",
                               "soft_error"}
            d = by["decorated"]
            assert d.title == "Decorated tool"
            assert d.icons and d.icons[0].src == "https://example.com/icon.png"
            assert d.annotations.read_only_hint is True and d.annotations.open_world_hint is False
            assert d.execution is not None and d.execution.task_support == "forbidden"
            assert d.meta == {"io.neograph.probe/kind": "decorated"}
            assert by["echo"].output_schema["required"] == ["message"]
            assert listed.next_cursor is None
            assert diag.records == [], diag.records
    run(go())


# --- tools/call --------------------------------------------------------------

def test_call_structured_output_passes_sdk_output_schema_validation(transport, mode, request,
                                                                    recorded, probe_http):
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, mode) as (c, diag):
            r = await c.call_tool("echo", {"message": "hi"})
            assert r.is_error is False
            assert r.structured_content == {"message": "hi"}
            # 2025-11-25 tools: structured results SHOULD also carry the serialized JSON as text
            assert json.loads(r.content[0].text) == {"message": "hi"}
            r = await c.call_tool("echo_text", {"message": "plain"})
            assert r.content[0].text == "plain" and r.structured_content is None
            r = await c.call_tool("decorated", {})
            assert [b.type for b in r.content] == ["text", "image", "resource_link"]
            assert diag.records == [], diag.records
    run(go())


def test_tool_failures_are_iserror_results(transport, mode, request, recorded, probe_http):
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, mode) as (c, diag):
            # Only the failure's public message crosses the wire, with its code.
            r = await c.call_tool("fail", {})
            assert r.is_error is True and "probe tool failure" in r.content[0].text
            assert r.meta["io.neograph/failure"]["code"] == "probe.failure"
            r = await c.call_tool("soft_error", {})
            assert r.is_error is True
            # server-side outputSchema violation must not reach the client as success
            r = await c.call_tool("bad_output", {})
            assert r.is_error is True and r.structured_content is None
            # SEP-1303 (2025-11-25): argument validation failures are tool execution errors
            r = await c.call_tool("echo", {"message": 3})
            assert r.is_error is True
            assert diag.records == [], diag.records
    run(go())


def test_unknown_tool_is_a_protocol_error(transport, mode, request, recorded, probe_http):
    """2025-11-25 server/tools 'Error Handling': unknown tools are protocol errors (-32602)."""
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, mode) as (c, _):
            try:
                r = await c.call_tool("no_such_tool", {})
            except MCPError as e:
                assert e.error.code == -32602
                return
            rid = request_id_of(log, "tools/call", "no_such_tool")
            pytest.fail("unknown tool answered with a CallToolResult instead of a JSON-RPC "
                        f"error: {json.dumps(responses_for(log, rid))}")
    run(go())


def test_ping(transport, request, recorded, probe_http):
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, "legacy") as (c, diag):
            await c.session.send_ping()
            assert diag.records == [], diag.records
    run(go())


def test_progress_token_is_accepted(transport, mode, request, recorded, probe_http):
    """A request carrying _meta.progressToken must still succeed (progress itself is optional)."""
    server, log = _target(transport, request, recorded, probe_http)
    seen = []

    async def on_progress(progress, total, message):
        seen.append(progress)

    async def go():
        async with client_for(server, mode) as (c, diag):
            r = await c.call_tool("echo", {"message": "p"}, progress_callback=on_progress)
            assert r.structured_content == {"message": "p"}
            assert diag.records == [], diag.records
    run(go())
    sent = [r["msg"] for r in wire(log, "c2s") if isinstance(r.get("msg"), dict)
            and r["msg"].get("method") == "tools/call"]
    assert "progressToken" in sent[-1]["params"].get("_meta", {})


def test_cancellation_stops_the_tool_and_sends_no_response(transport, mode, request, recorded,
                                                           probe_http):
    """basic/utilities/cancellation: receivers SHOULD stop and SHOULD NOT respond."""
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, mode) as (c, diag):
            with anyio.move_on_after(0.3):
                await c.call_tool("slow", {"seconds": 5})
            t0 = time.monotonic()
            r = await c.call_tool("echo", {"message": "after"})  # worker must be free again
            assert r.structured_content == {"message": "after"}
            assert time.monotonic() - t0 < 2
            await anyio.sleep(0.6)  # let any late response arrive before the session closes
            return diag.records
    records = run(go())
    rid = request_id_of(log, "tools/call", "slow")
    cancels = [r["msg"] for r in wire(log, "c2s") if isinstance(r.get("msg"), dict)
               and r["msg"].get("method") == "notifications/cancelled"]
    assert cancels and cancels[0]["params"]["requestId"] == rid, "SDK did not send cancellation"
    late = responses_for(log, rid)
    assert records == [], records
    assert late == [], (f"server answered the cancelled request {rid}: {json.dumps(late)}")


def test_concurrent_calls_overlap(transport, request, recorded, probe_http):
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, "legacy") as (c, _):
            t0 = time.monotonic()
            async with anyio.create_task_group() as tg:
                for _ in range(4):
                    tg.start_soon(c.call_tool, "slow", {"seconds": 0.5})
            return time.monotonic() - t0
    assert run(go()) < 1.5


def test_unadvertised_features_answer_method_not_found(transport, request, recorded, probe_http):
    server, log = _target(transport, request, recorded, probe_http)

    async def go():
        async with client_for(server, "legacy") as (c, _):
            caps = c.server_capabilities
            assert caps.resources is None and caps.prompts is None and caps.logging is None
            with pytest.raises(MCPError) as e:
                await c.list_resources()
            assert e.value.error.code == -32601
    run(go())


# --- other NeoGraph servers ----------------------------------------------------

def test_example_59_echo_over_stdio(request):
    log = transcript_path(f"py-{request.node.name}")
    cmd, args = stdio_command(log, [str(EXAMPLE59)])

    async def go():
        async with client_for(StdioServerParameters(command=cmd, args=args), "auto") as (c, d):
            r = await c.call_tool("echo", {"message": "x"})
            assert r.structured_content == {"message": "x"} and d.records == []
    run(go())


@pytest.mark.parametrize("t", TRANSPORTS)
def test_harness_server_tools(t, request, recorded, harness_http):
    if t == "stdio":
        log = transcript_path(f"py-{request.node.name}")
        cmd, args = stdio_command(log, [str(HARNESS)])
        server = StdioServerParameters(command=cmd, args=args, cwd=TMP,
                                       env={"NEOGRAPH_HARNESS_SMOKE": "1", "PATH": os.defpath})
    else:
        server, log = recorded(harness_http)

    async def go():
        async with client_for(server, "auto") as (c, diag):
            assert c.server_info.name == "neograph-harness"
            names = {x.name for x in (await c.list_tools()).tools}
            assert {"neograph_schema", "neograph_compile", "neograph_start", "neograph_get",
                    "neograph_resume", "neograph_cancel"} <= names
            r = await c.call_tool("neograph_schema", {})  # SDK validates outputSchema
            assert r.is_error is False and "request_schema" in r.structured_content
            r = await c.call_tool("neograph_get", {"run_id": "missing"})
            assert r.is_error is True
            r = await c.call_tool("neograph_cancel", {"run_id": "missing"})
            assert r.structured_content == {"run_id": "missing", "cancelled": False}
            assert diag.records == [], diag.records
    run(go())


def test_tool_registered_with_default_input_schema_is_listable(request):
    """ToolDefinition's default input_schema ({}) must not poison tools/list."""
    log = transcript_path(f"py-{request.node.name}")
    cmd, args = stdio_command(log, [str(PROBE), "stdio", "--untyped-tool"])

    async def go():
        async with client_for(StdioServerParameters(command=cmd, args=args), "legacy") as (c, _):
            try:
                await c.list_tools()
            except pydantic.ValidationError as e:
                listed = [r["msg"] for r in wire(log, "s2c") if isinstance(r.get("msg"), dict)
                          and "tools" in (r["msg"].get("result") or {})]
                bad = [t for t in listed[-1]["result"]["tools"] if t["name"] == "untyped"]
                pytest.fail(f"SDK rejected the whole tools/list: {e}\noffending tool: "
                            f"{json.dumps(bad)}")
    run(go())
