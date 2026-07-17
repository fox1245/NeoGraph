"""Official A2A/ACP SDK integration tests.

The normal test suite skips this module when protocol extras are absent. CI has
a dedicated Python 3.12 job that installs both SDKs and runs these tests.
"""

import asyncio
import contextlib
import os
import runpy
import socket
import sys
from pathlib import Path
from uuid import uuid4

import pytest


pytest.importorskip("a2a")
pytest.importorskip("acp")

import httpx
from a2a.client import A2ACardResolver, ClientConfig, create_client
from a2a.client.errors import AgentCardResolutionError
from a2a.helpers import new_text_message
from a2a.types import Role, SendMessageRequest
from acp import (
    PROTOCOL_VERSION,
    RequestError,
    connect_to_agent,
    image_block,
    text_block,
)
from acp.schema import ClientCapabilities, Implementation

import neograph_engine as ng


ROOT = Path(__file__).resolve().parents[3]
EXAMPLES = ROOT / "bindings" / "python" / "examples"
sys.path.insert(0, str(EXAMPLES))


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _subprocess_env(**updates):
    env = os.environ.copy()
    paths = [str(ROOT / "build"), str(EXAMPLES)]
    if env.get("PYTHONPATH"):
        paths.append(env["PYTHONPATH"])
    env["PYTHONPATH"] = os.pathsep.join(paths)
    env.update(updates)
    return env


async def _stop_process(process, connection=None):
    if connection is not None:
        with contextlib.suppress(Exception):
            await connection.close()
    if process.returncode is None:
        process.terminate()
        with contextlib.suppress(ProcessLookupError):
            await process.wait()


def test_a2a_streaming_round_trip():
    async def exercise():
        port = _free_port()
        process = await asyncio.create_subprocess_exec(
            sys.executable,
            str(EXAMPLES / "27_a2a_server.py"),
            env=_subprocess_env(NEOGRAPH_A2A_PORT=str(port)),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        assert process.stderr is not None
        try:
            base_url = f"http://127.0.0.1:{port}"
            async with httpx.AsyncClient() as http:
                resolver = A2ACardResolver(http, base_url)
                for _ in range(50):
                    try:
                        card = await resolver.get_agent_card()
                        break
                    except AgentCardResolutionError:
                        if process.returncode is not None:
                            stderr = await process.stderr.read()
                            raise AssertionError(stderr.decode())
                        await asyncio.sleep(0.1)
                else:
                    raise AssertionError("A2A server did not start")

            assert card.capabilities.streaming is True
            client = await create_client(
                agent=card, client_config=ClientConfig(streaming=True)
            )
            request = SendMessageRequest(
                message=new_text_message("probe", role=Role.ROLE_USER)
            )
            chunks = [chunk async for chunk in client.send_message(request)]
            await client.close()
            rendered = repr(chunks)
            assert "NeoGraph " in rendered
            assert "received: " in rendered
            assert "probe" in rendered
            assert " (turn 1)" in rendered
            assert len(chunks) >= 4
        finally:
            await _stop_process(process)

    asyncio.run(exercise())


class _ACPClient:
    def __init__(self):
        self.messages = []

    async def session_update(self, session_id, update, **kwargs):
        self.messages.append(getattr(update.content, "text", ""))

    async def ext_method(self, method, params):
        raise RuntimeError(method)

    async def ext_notification(self, method, params):
        raise RuntimeError(method)


async def _start_acp(extra_env=None):
    process = await asyncio.create_subprocess_exec(
        sys.executable,
        str(EXAMPLES / "28_acp_agent.py"),
        env=_subprocess_env(**(extra_env or {})),
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    assert process.stdin is not None and process.stdout is not None
    client = _ACPClient()
    connection = connect_to_agent(client, process.stdin, process.stdout)
    initialized = await connection.initialize(
        protocol_version=PROTOCOL_VERSION,
        client_capabilities=ClientCapabilities(),
        client_info=Implementation(name="test-client", version="1.0"),
    )
    return process, client, connection, initialized


async def _wait_messages(client, count):
    for _ in range(100):
        if len(client.messages) >= count:
            return
        await asyncio.sleep(0.01)
    raise AssertionError(
        f"expected {count} ACP message chunks, got {client.messages!r}"
    )


def test_acp_streaming_and_durable_session_load(tmp_path):
    async def exercise():
        postgres_url = os.environ.get("NEOGRAPH_TEST_POSTGRES_URL")
        if postgres_url:
            env = {"NEOGRAPH_ACP_POSTGRES_URL": postgres_url}
        else:
            if not hasattr(ng, "SqliteCheckpointStore"):
                pytest.skip("test build has no durable checkpoint backend")
            env = {"NEOGRAPH_ACP_SQLITE_PATH": str(tmp_path / "acp.db")}
        process, client, connection, initialized = await _start_acp(env)
        try:
            assert initialized.agent_capabilities.load_session is True
            session = await connection.new_session(cwd=str(tmp_path), mcp_servers=[])
            await connection.prompt(
                session_id=session.session_id,
                prompt=[text_block("first")],
            )
            await _wait_messages(client, 4)
            assert "".join(client.messages) == (
                "NeoGraph received: first (turn 1)"
            )
            assert len(client.messages) == 4
        finally:
            await _stop_process(process, connection)

        process, client, connection, initialized = await _start_acp(env)
        try:
            try:
                await connection.load_session(
                    cwd=str(tmp_path),
                    session_id=session.session_id,
                    mcp_servers=[],
                )
            except Exception:
                process.terminate()
                await process.wait()
                assert process.stderr is not None
                stderr = (await process.stderr.read()).decode()
                raise AssertionError(stderr) from None
            await _wait_messages(client, 2)
            assert client.messages == [
                "first",
                "NeoGraph received: first (turn 1)",
            ]
            client.messages.clear()
            await connection.prompt(
                session_id=session.session_id,
                prompt=[text_block("second")],
            )
            await _wait_messages(client, 4)
            assert "".join(client.messages) == (
                "NeoGraph received: second (turn 2)"
            )
        finally:
            await _stop_process(process, connection)

    asyncio.run(exercise())


def test_acp_rich_content_reaches_input_builder():
    namespace = runpy.run_path(str(EXAMPLES / "28_acp_agent.py"))

    class CapturingAdapter:
        def __init__(self):
            self.payload = None

        async def stream(self, payload, **kwargs):
            self.payload = payload
            self.thread_id = kwargs["thread_id"]
            yield ng.ProtocolStreamEvent("final", "handled")

        def cancel(self, request_id):
            return 0

    async def exercise():
        agent = namespace["NeoGraphACPAgent"]()
        agent.adapter = CapturingAdapter()
        agent.connection = _ACPClient()
        session_id = uuid4().hex
        agent.sessions.add(session_id)
        response = await agent.prompt(
            session_id,
            [
                text_block("look"),
                image_block("aGVsbG8=", "image/png"),
            ],
        )
        assert response.stop_reason == "end_turn"
        assert agent.adapter.payload == [
            {"text": "look", "type": "text"},
            {"data": "aGVsbG8=", "mimeType": "image/png", "type": "image"},
        ]
        assert agent.adapter.thread_id == f"acp:{session_id}"
        assert agent.connection.messages == ["handled"]

    asyncio.run(exercise())


def test_acp_cancel_completes_original_prompt_without_final_message():
    namespace = runpy.run_path(str(EXAMPLES / "28_acp_agent.py"))

    class BlockingAdapter:
        def __init__(self):
            self.started = asyncio.Event()

        async def stream(self, payload, **kwargs):
            self.started.set()
            await asyncio.Future()
            if False:
                yield None

        def cancel(self, request_id):
            return 0

    async def exercise():
        agent = namespace["NeoGraphACPAgent"]()
        agent.adapter = BlockingAdapter()
        agent.connection = _ACPClient()
        session_id = uuid4().hex
        agent.sessions.add(session_id)
        prompt = asyncio.create_task(
            agent.prompt(session_id, [text_block("wait")])
        )
        await agent.adapter.started.wait()
        await agent.cancel(session_id)
        response = await asyncio.wait_for(prompt, timeout=1)
        assert response.stop_reason == "cancelled"
        assert agent.connection.messages == []

    asyncio.run(exercise())


def test_acp_prompt_rejects_session_not_admitted_by_new_or_load():
    namespace = runpy.run_path(str(EXAMPLES / "28_acp_agent.py"))

    async def exercise():
        agent = namespace["NeoGraphACPAgent"]()
        agent.connection = _ACPClient()
        with pytest.raises(RequestError) as exc_info:
            await agent.prompt(uuid4().hex, [text_block("probe")])
        assert getattr(exc_info.value, "code", None) == -32002

    asyncio.run(exercise())
