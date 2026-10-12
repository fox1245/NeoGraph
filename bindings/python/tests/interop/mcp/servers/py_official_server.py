"""Official Python MCP SDK server used as the oracle peer for NeoGraph's MCP client.

Pinned SDK: mcp==2.3.0 (see ../requirements.txt).  Two flavours:

  --flavor lowlevel   mcp.server.lowlevel.Server with hand-built results, so every
                      content type, isError, structuredContent, pagination and
                      server->client requests can be exercised exactly.
  --flavor mcpserver  mcp.server.mcpserver.MCPServer (the v2 name of FastMCP), the
                      path most real servers use: decorated functions, generated
                      inputSchema/outputSchema, ToolError -> isError.

Transports: --transport stdio | http.  HTTP options: --stateless, --json-response,
--path, --port (0: the OS picks; the server prints `READY <port>` once it listens),
--idle-timeout (seconds; small values reproduce session expiry).

--protocol-version V   answer every initialize with revision V (such as 2024-11-05),
                       as a server that supports only that revision does.

--record FILE   HTTP only: append one JSON line per HTTP exchange (request
                method/path/headers/body, response status/headers/body).
--events FILE   append one JSON line per server-side observation (cancellations,
                answers to server->client requests, notifications received).

No network access beyond the loopback listener; no model calls.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import threading
import time
from pathlib import Path
from typing import Any

import anyio

import mcp.types as t
from mcp.server.lowlevel import NotificationOptions, Server
from mcp.shared.exceptions import MCPError
from mcp.shared.message import ServerMessageMetadata

ARGS: argparse.Namespace
_events_lock = threading.Lock()

PNG_1PX = base64.b64encode(
    bytes.fromhex(
        "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
        "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082"
    )
).decode()
WAV_EMPTY = base64.b64encode(
    b"RIFF$\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00@\x1f\x00\x00\x80>\x00\x00\x02\x00\x10\x00data\x00\x00\x00\x00"
).decode()


def offer_only(version: str) -> None:
    """Make the SDK's initialize negotiation offer only `version`: a client
    that asks for anything else gets it as the counter-offer. The HTTP
    transport still accepts every handshake revision in MCP-Protocol-Version."""
    import mcp.server.runner as runner

    runner.HANDSHAKE_PROTOCOL_VERSIONS = (version,)
    runner.LATEST_HANDSHAKE_VERSION = version


def event(kind: str, **fields: Any) -> None:
    if not ARGS.events:
        return
    line = json.dumps({"t": time.time(), "event": kind, **fields}, default=str)
    with _events_lock:
        with open(ARGS.events, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")


# --------------------------------------------------------------------------- lowlevel

OBJ = {"type": "object"}
TOOLS: list[t.Tool] = [
    t.Tool(name="echo", description="Echo text", input_schema={
        "type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}),
    t.Tool(name="structured", title="Structured answer", description="Returns structuredContent",
           input_schema={"type": "object", "properties": {"n": {"type": "integer"}}},
           output_schema={"type": "object", "properties": {"answer": {"type": "integer"}},
                          "required": ["answer"]},
           annotations=t.ToolAnnotations(read_only_hint=True, idempotent_hint=True),
           icons=[t.Icon(src="data:image/png;base64," + PNG_1PX, mime_type="image/png")]),
    t.Tool(name="tool_error", description="Returns isError=true", input_schema=OBJ),
    t.Tool(name="tool_error_with_schema", description="isError while an outputSchema is declared",
           input_schema=OBJ,
           output_schema={"type": "object", "properties": {"answer": {"type": "integer"}},
                           "required": ["answer"]}),
    t.Tool(name="protocol_error", description="Raises a JSON-RPC error", input_schema=OBJ),
    t.Tool(name="all_content", description="Every content block type", input_schema=OBJ),
    t.Tool(name="log_then_answer", description="Request-scoped log notifications, then a result",
           input_schema=OBJ),
    t.Tool(name="ping_client", description="Server->client ping inside a tool call", input_schema=OBJ),
    t.Tool(name="roots_client", description="Server->client roots/list (undeclared capability)",
           input_schema=OBJ),
    t.Tool(name="slow", description="Sleeps; records cancellation",
           input_schema={"type": "object", "properties": {"seconds": {"type": "number"}}}),
    t.Tool(name="poll_then_answer", description="Closes the SSE stream (SEP-1699 polling), then answers",
           input_schema=OBJ),
    t.Tool(name="structured_only", description="structuredContent with empty content",
           input_schema=OBJ,
           output_schema={"type": "object", "properties": {"answer": {"type": "integer"}},
                          "required": ["answer"]}),
]
PAGE = 4


async def list_tools(ctx, params: t.PaginatedRequestParams | None) -> t.ListToolsResult:
    cursor = params.cursor if params is not None else None
    event("tools_list", cursor=cursor)
    start = int(cursor) if cursor else 0
    page = TOOLS[start:start + PAGE] if ARGS.paginate else TOOLS
    nxt = str(start + PAGE) if ARGS.paginate and start + PAGE < len(TOOLS) else None
    return t.ListToolsResult(tools=page, next_cursor=nxt)


async def call_tool(ctx, params: t.CallToolRequestParams):
    name = params.name
    args = params.arguments or {}
    meta = ServerMessageMetadata(related_request_id=ctx.request_id)
    if name == "echo":
        return t.CallToolResult(content=[t.TextContent(type="text", text=str(args.get("text", "")))])
    if name == "structured":
        n = int(args.get("n", 41))
        return t.CallToolResult(content=[t.TextContent(type="text", text=json.dumps({"answer": n + 1}))],
                                structured_content={"answer": n + 1})
    if name == "structured_only":
        return t.CallToolResult(content=[], structured_content={"answer": 7})
    if name == "tool_error":
        return t.CallToolResult(content=[t.TextContent(type="text", text="boom: tool failed")], is_error=True)
    if name == "tool_error_with_schema":
        return t.CallToolResult(content=[t.TextContent(type="text", text="boom: schema tool failed")],
                                is_error=True)
    if name == "protocol_error":
        raise MCPError(code=-32602, message="Invalid params: probe", data={"probe": True})
    if name == "all_content":
        return t.CallToolResult(content=[
            t.TextContent(type="text", text="text block"),
            t.ImageContent(type="image", data=PNG_1PX, mime_type="image/png"),
            t.AudioContent(type="audio", data=WAV_EMPTY, mime_type="audio/wav"),
            t.ResourceLink(type="resource_link", uri="file:///probe/readme.txt", name="readme",
                           mime_type="text/plain"),
            t.EmbeddedResource(type="resource", resource=t.TextResourceContents(
                uri="file:///probe/inline.txt", mime_type="text/plain", text="inline text")),
            t.EmbeddedResource(type="resource", resource=t.BlobResourceContents(
                uri="file:///probe/inline.bin", mime_type="application/octet-stream", blob=PNG_1PX)),
        ])
    if name == "log_then_answer":
        for i in range(3):
            await ctx.session.send_log_message("info", {"step": i}, logger="probe",
                                               related_request_id=ctx.request_id)
        return t.CallToolResult(content=[t.TextContent(type="text", text="answered after logs")])
    if name == "ping_client":
        try:
            count = int(args.get("count", 1))
            per_ping = float(args.get("timeout", ARGS.server_request_timeout))
            for i in range(count):
                try:
                    with anyio.fail_after(per_ping):
                        await ctx.session.send_request(t.PingRequest(), t.EmptyResult, metadata=meta)
                except TimeoutError:
                    if i + 1 == count:
                        raise
                    event("ping_client_attempt", attempt=i, ok=False)
            event("ping_client", ok=True)
            return t.CallToolResult(content=[t.TextContent(type="text", text="client answered ping")])
        except TimeoutError:
            event("ping_client", ok=False, error="timeout")
            return t.CallToolResult(content=[t.TextContent(type="text",
                                    text="client never answered ping")], is_error=True)
        except Exception as exc:  # noqa: BLE001 - observation only
            event("ping_client", ok=False, error=repr(exc))
            return t.CallToolResult(content=[t.TextContent(type="text", text=f"ping failed: {exc!r}")],
                                    is_error=True)
    if name == "roots_client":
        try:
            with anyio.fail_after(ARGS.server_request_timeout):
                await ctx.session.send_request(t.ListRootsRequest(), t.ListRootsResult, metadata=meta)
            event("roots_client", ok=True)
            return t.CallToolResult(content=[t.TextContent(type="text", text="client listed roots")])
        except TimeoutError:
            event("roots_client", ok=False, error="timeout")
            return t.CallToolResult(content=[t.TextContent(type="text", text="client never answered")],
                                    is_error=True)
        except MCPError as exc:
            event("roots_client", ok=False, error="jsonrpc", code=exc.code, message=str(exc))
            return t.CallToolResult(content=[t.TextContent(type="text",
                                    text=f"client rejected roots/list: {exc.code}")])
        except Exception as exc:  # noqa: BLE001
            event("roots_client", ok=False, error=repr(exc))
            return t.CallToolResult(content=[t.TextContent(type="text", text=repr(exc))], is_error=True)
    if name == "poll_then_answer":
        closer = getattr(ctx, "close_sse_stream", None)
        event("poll_close", available=closer is not None)
        if closer is not None:
            await closer()
        await anyio.sleep(0.5)
        return t.CallToolResult(content=[t.TextContent(type="text", text="answered after poll")])
    if name == "slow":
        seconds = float(args.get("seconds", 3))
        event("slow_started", request_id=ctx.request_id, seconds=seconds)
        try:
            await anyio.sleep(seconds)
        except anyio.get_cancelled_exc_class():
            event("slow_cancelled", request_id=ctx.request_id)
            raise
        event("slow_finished", request_id=ctx.request_id)
        return t.CallToolResult(content=[t.TextContent(type="text", text="slow done")])
    raise MCPError(code=-32602, message=f"Unknown tool: {name}")


async def on_progress(ctx, params) -> None:  # pragma: no cover - observation
    event("client_progress", params=params.model_dump(mode="json"))


def build_lowlevel() -> Server:
    return Server("ng-probe-lowlevel", version="1.0.0", instructions="probe server",
                  on_list_tools=list_tools, on_call_tool=call_tool)


# --------------------------------------------------------------------------- MCPServer

from pydantic import BaseModel  # noqa: E402

from mcp.server.mcpserver import Context, MCPServer  # noqa: E402
from mcp.server.mcpserver.exceptions import ToolError  # noqa: E402


class Weather(BaseModel):
    city: str
    celsius: float


def build_mcpserver():
    server = MCPServer("ng-probe-mcpserver", version="1.0.0", instructions="probe server")

    @server.tool()
    def echo(text: str) -> str:
        """Echo text"""
        return text

    @server.tool()
    def add(a: int, b: int) -> int:
        """Add two integers (wrapped structured output)"""
        return a + b

    @server.tool()
    def weather(city: str) -> Weather:
        """Structured output from a pydantic model"""
        return Weather(city=city, celsius=21.5)

    @server.tool()
    def fails(reason: str = "nope") -> str:
        """Raises ToolError -> isError result"""
        raise ToolError(f"tool refused: {reason}")

    @server.tool()
    async def chatty(ctx: Context) -> str:
        """Logs through the context before answering"""
        await ctx.info("first log")
        await ctx.info("second log")
        return "chatty done"

    return server


# --------------------------------------------------------------------------- HTTP recording

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _recorder import Recorder  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
import server_ports  # noqa: E402


def make_event_store():
    """Minimal in-memory EventStore (resumability) built on the SDK's abstract interface."""
    import itertools

    from mcp.server.streamable_http import EventMessage, EventStore

    class MemoryEventStore(EventStore):
        def __init__(self):
            self.events: list[tuple[str, str, Any]] = []
            self.counter = itertools.count(1)

        async def store_event(self, stream_id, message):
            event_id = str(next(self.counter))
            self.events.append((event_id, stream_id, message))
            event("store_event", event_id=event_id, stream_id=stream_id, priming=message is None)
            return event_id

        async def replay_events_after(self, last_event_id, send_callback):
            event("replay_requested", last_event_id=last_event_id)
            ids = [e[0] for e in self.events]
            if last_event_id not in ids:
                return None
            idx = ids.index(last_event_id)
            stream_id = self.events[idx][1]
            for event_id, sid, message in self.events[idx + 1:]:
                if sid == stream_id and message is not None:
                    await send_callback(EventMessage(message, event_id))
            return stream_id

    return MemoryEventStore()


def run_http(server_obj, lowlevel: bool) -> None:
    kwargs = dict(streamable_http_path=ARGS.path, json_response=ARGS.json_response,
                  stateless_http=ARGS.stateless)
    if ARGS.event_store:
        kwargs["event_store"] = make_event_store()
        kwargs["retry_interval"] = ARGS.retry_interval
    if not ARGS.stateless:
        kwargs["session_idle_timeout"] = ARGS.idle_timeout
        if ARGS.max_sessions:
            kwargs["max_sessions"] = ARGS.max_sessions
    app = server_obj.streamable_http_app(**kwargs)
    # The listener is bound first and kept, so --port 0 yields a port nobody else can take.
    server_ports.serve_uvicorn(Recorder(app, ARGS.record), server_ports.bind_listener(ARGS.port),
                               lifespan="on")


def main() -> None:
    global ARGS
    p = argparse.ArgumentParser()
    p.add_argument("--flavor", choices=["lowlevel", "mcpserver"], default="lowlevel")
    p.add_argument("--transport", choices=["stdio", "http"], default="stdio")
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--path", default="/mcp")
    p.add_argument("--stateless", action="store_true")
    p.add_argument("--json-response", action="store_true")
    p.add_argument("--idle-timeout", type=float, default=1800.0)
    p.add_argument("--max-sessions", type=int, default=0, help="0 = SDK default (10000)")
    p.add_argument("--paginate", action="store_true")
    p.add_argument("--record")
    p.add_argument("--events")
    p.add_argument("--server-request-timeout", type=float, default=4.0)
    p.add_argument("--event-store", action="store_true", help="enable resumability (priming events)")
    p.add_argument("--retry-interval", type=int, default=200, help="SSE retry field in ms")
    p.add_argument("--protocol-version", help="the only revision the handshake offers")
    ARGS = p.parse_args()
    if ARGS.protocol_version:
        offer_only(ARGS.protocol_version)

    if ARGS.flavor == "mcpserver":
        server = build_mcpserver()
        if ARGS.transport == "stdio":
            server.run("stdio")
        else:
            run_http(server, lowlevel=False)
        return

    server = build_lowlevel()
    if ARGS.transport == "stdio":
        from mcp.server.stdio import stdio_server

        async def serve() -> None:
            async with stdio_server() as (read, write):
                await server.run(read, write, server.create_initialization_options(
                    notification_options=NotificationOptions(tools_changed=True)))

        anyio.run(serve)
    else:
        run_http(server, lowlevel=True)


if __name__ == "__main__":
    main()
