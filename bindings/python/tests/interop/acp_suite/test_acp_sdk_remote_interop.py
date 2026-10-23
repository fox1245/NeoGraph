"""Official ACP Python SDK -> the experimental remote transports of NeoGraph.

The SDK ships a client for the draft Streamable HTTP and WebSocket transport
(``acp.http.client.create_http_stream`` and ``acp.ws.client.create_websocket_stream``,
both from the ``http`` extra). Every test starts the deterministic native agent
(``neograph_acp_probe_agent --http``, built from tests/fixtures/acp_probe_agent.cpp
with NEOGRAPH_BUILD_ACP_HTTP_SERVER=ON) and drives it with the SDK's own
client over each transport, so a test passes only when the SDK's client and
NeoGraph's ACPHttpServer agree on the draft's wire: the connection id header,
202 answers, the connection and session streams, and WebSocket text frames.
The SDK parses every agent message into its pydantic models; a test also
fails when it logged a warning or an error.

The agent speaks HTTP/1.1; the SDK's default client asks for HTTP/2, which
needs TLS to be negotiated, so the tests hand it an HTTP/1.1 client.

Runs on the 0.12 line of the SDK only; it skips on any other version, on a
missing ``http`` extra and on a probe agent built without the remote
transports.
"""

import asyncio
import contextlib
import logging
import sys

import pytest

from acp_interop_support import (  # noqa: E402
    require_module,
    require_pinned_acp_sdk,
    require_probe_agent,
)

SDK_VERSION = require_pinned_acp_sdk()
if not SDK_VERSION.startswith("0.12."):
    pytest.skip("the remote transport tests run on the agent-client-protocol 0.12 line",
                allow_module_level=True)
require_module("httpx")
require_module("websockets")
AGENT = require_probe_agent()

import httpx  # noqa: E402
from acp import PROTOCOL_VERSION, connect_to_agent  # noqa: E402
from acp.http.client import create_http_stream  # noqa: E402
from acp.schema import (  # noqa: E402
    ClientCapabilities,
    FileSystemCapabilities,
    Implementation,
    ReadTextFileResponse,
    RequestPermissionResponse,
    TextContentBlock,
)
from acp.ws.client import create_websocket_stream  # noqa: E402

TRANSPORTS = ("http", "websocket")


def text_block(text):
    return TextContentBlock(type="text", text=text)


class SdkComplaints(logging.Handler):
    """Collects every warning or error the SDK logs while a test runs."""

    def __init__(self):
        super().__init__(level=logging.WARNING)
        self.records = []

    def emit(self, record):
        self.records.append(f"{record.levelname} {record.name}: {record.getMessage()}")


class RecordingClient:
    """An ACP client: records updates and answers the agent's requests."""

    def __init__(self, files=None):
        self.files = dict(files or {})
        self.updates = []
        self.permission_requests = []
        self.reads = []
        self.connection = None

    def on_connect(self, conn):
        self.connection = conn

    async def session_update(self, session_id, update, **kwargs):
        self.updates.append((session_id, update))

    async def request_permission(self, session_id, tool_call, options, **kwargs):
        self.permission_requests.append((session_id, tool_call, options))
        option = next(o for o in options if o.kind.startswith("allow"))
        return RequestPermissionResponse.model_validate(
            {"outcome": {"outcome": "selected", "optionId": option.option_id}})

    async def read_text_file(self, session_id, path, line=None, limit=None, **kwargs):
        self.reads.append((session_id, path))
        return ReadTextFileResponse(content=self.files[path])

    def agent_text(self):
        return [getattr(update.content, "text", "") for _, update in self.updates
                if getattr(update, "session_update", None) == "agent_message_chunk"]


class RemoteAgent:
    """The probe agent serving the remote transports on a free loopback port."""

    async def start(self):
        self.process = await asyncio.create_subprocess_exec(
            str(AGENT), "--http",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL)
        line = await asyncio.wait_for(self.process.stdout.readline(), 20)
        if not line.startswith(b"listening "):
            await self.stop()
            pytest.skip("the probe agent was built without NEOGRAPH_BUILD_ACP_HTTP_SERVER "
                        f"(it printed {line!r})")
        self.port = int(line.split()[1])
        return self

    async def stop(self):
        if self.process.returncode is None:
            with contextlib.suppress(ProcessLookupError, OSError):
                self.process.stdin.close()
            try:
                await asyncio.wait_for(self.process.wait(), 10)
            except asyncio.TimeoutError:
                self.process.kill()
                await self.process.wait()
        transport = getattr(self.process, "_transport", None)
        if transport is not None:
            transport.close()

    async def connect(self, kind, client):
        url = f"{'ws' if kind == 'websocket' else 'http'}://127.0.0.1:{self.port}/acp"
        if kind == "websocket":
            transport = await create_websocket_stream(url)
        else:
            transport = create_http_stream(
                url, client=httpx.AsyncClient(http2=False, timeout=httpx.Timeout(None)))
        conn = connect_to_agent(client, transport)
        client.connection = conn
        return conn


@contextlib.asynccontextmanager
async def session(kind, client=None, capabilities=None):
    """An initialized connection over `kind`, with the SDK's complaints collected."""
    client = client or RecordingClient()
    complaints = SdkComplaints()
    root = logging.getLogger()
    root.addHandler(complaints)
    agent = await RemoteAgent().start()
    conn = None
    try:
        conn = await agent.connect(kind, client)
        init = await asyncio.wait_for(conn.initialize(
            protocol_version=PROTOCOL_VERSION,
            client_capabilities=capabilities or ClientCapabilities(),
            client_info=Implementation(name="acp-remote-interop", version="1.0")), 20)
        assert init.protocol_version == 1
        yield agent, conn, client
        assert not complaints.records, f"the SDK logged problems: {complaints.records}"
    finally:
        root.removeHandler(complaints)
        if conn is not None:
            with contextlib.suppress(Exception):
                await asyncio.wait_for(conn.close(), 10)
        await agent.stop()


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 90))


@pytest.mark.parametrize("kind", TRANSPORTS)
def test_a_prompt_streams_its_update_and_ends_the_turn(kind):
    async def body():
        async with session(kind) as (_, conn, client):
            sid = (await asyncio.wait_for(conn.new_session(cwd="/tmp", mcp_servers=[]), 20)).session_id
            response = await asyncio.wait_for(
                conn.prompt(session_id=sid, prompt=[text_block("echo hello remote")]), 20)
            assert response.stop_reason == "end_turn"
            assert client.agent_text() == ["[probe] hello remote"]
            assert all(update_sid == sid for update_sid, _ in client.updates)
    run(body())


@pytest.mark.parametrize("kind", TRANSPORTS)
def test_the_agent_asks_for_permission_and_for_a_file(kind):
    async def body():
        capabilities = ClientCapabilities(fs=FileSystemCapabilities(read_text_file=True))
        client = RecordingClient(files={"/work/notes.txt": "remote file"})
        async with session(kind, client, capabilities) as (_, conn, _client):
            sid = (await asyncio.wait_for(conn.new_session(cwd="/tmp", mcp_servers=[]), 20)).session_id
            await asyncio.wait_for(
                conn.prompt(session_id=sid, prompt=[text_block("permission")]), 20)
            await asyncio.wait_for(
                conn.prompt(session_id=sid, prompt=[text_block("fsread /work/notes.txt")]), 20)
            assert [req[0] for req in client.permission_requests] == [sid]
            assert client.reads == [(sid, "/work/notes.txt")]
            assert client.agent_text() == ["permission:selected:allow", "read:remote file"]
    run(body())


@pytest.mark.parametrize("kind", TRANSPORTS)
def test_session_cancel_ends_a_running_turn_as_cancelled(kind):
    async def body():
        async with session(kind) as (_, conn, _client):
            sid = (await asyncio.wait_for(conn.new_session(cwd="/tmp", mcp_servers=[]), 20)).session_id
            turn = asyncio.ensure_future(conn.prompt(session_id=sid, prompt=[text_block("slow")]))
            await asyncio.sleep(0.5)
            await conn.cancel(session_id=sid)
            response = await asyncio.wait_for(turn, 20)
            assert response.stop_reason == "cancelled"
    run(body())


@pytest.mark.parametrize("kind", TRANSPORTS)
def test_two_connections_run_at_the_same_time(kind):
    async def body():
        agent = await RemoteAgent().start()
        complaints = SdkComplaints()
        logging.getLogger().addHandler(complaints)
        first, second = RecordingClient(), RecordingClient()
        conns = []
        try:
            for client in (first, second):
                conn = await agent.connect(kind, client)
                conns.append(conn)
                await asyncio.wait_for(conn.initialize(
                    protocol_version=PROTOCOL_VERSION, client_capabilities=ClientCapabilities(),
                    client_info=Implementation(name="acp-remote-interop", version="1.0")), 20)
            sids = [(await asyncio.wait_for(c.new_session(cwd="/tmp", mcp_servers=[]), 20)).session_id
                    for c in conns]
            assert sids[0] != sids[1]
            answers = await asyncio.gather(*[
                asyncio.wait_for(c.prompt(session_id=sid, prompt=[text_block(f"echo from {n}")]), 20)
                for n, (c, sid) in enumerate(zip(conns, sids))])
            assert [a.stop_reason for a in answers] == ["end_turn", "end_turn"]
            assert first.agent_text() == ["[probe] from 0"]
            assert second.agent_text() == ["[probe] from 1"]
            assert not complaints.records, complaints.records
        finally:
            logging.getLogger().removeHandler(complaints)
            for conn in conns:
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(conn.close(), 10)
            await agent.stop()
    run(body())


if __name__ == "__main__":  # pragma: no cover
    sys.exit(pytest.main([__file__, "-q"]))
