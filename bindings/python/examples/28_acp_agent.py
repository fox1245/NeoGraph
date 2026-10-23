"""Host a NeoGraph agent through the official ACP Python SDK over stdio.

Install the optional protocol dependency first:
    pip install "neograph-engine[acp]"

ACP clients launch this script as a subprocess. Protocol messages use stdout,
so application diagnostics must go to stderr or a logging handler. Set
NEOGRAPH_ACP_POSTGRES_URL for durable session/load support. Source builds with
SQLite enabled may use NEOGRAPH_ACP_SQLITE_PATH instead.

A prompt that starts with "approve:" pauses the graph. The agent asks the client with
session/request_permission in the same turn (options "approve" and "reject"; the interrupt payload
is the tool call's rawInput) and resumes the graph with the option the client selected, streaming
the rest of the turn. If the client answers "cancelled", the turn ends with stop reason cancelled.

    python 28_acp_agent.py                  serve over stdio until the client closes it
    python 28_acp_agent.py --self-check     start this agent as a child process the way an editor
                                            does, have two turns and one approval with the SDK's
                                            client, and exit

smoke-requires: acp
smoke-args: --self-check
smoke-expect: NeoGraph received: hello (turn 1)
smoke-expect: NeoGraph received: hello again (turn 2)
smoke-expect: asked: Run `deploy`?
smoke-expect: NeoGraph ran: deploy (turn 3)
"""

from __future__ import annotations

import asyncio
import os
import re
import sys
from typing import Any
from uuid import uuid4

from acp import (
    PROTOCOL_VERSION,
    Agent,
    InitializeResponse,
    LoadSessionResponse,
    NewSessionResponse,
    PromptResponse,
    RequestError,
    run_agent,
)
from acp.interfaces import Client
from acp.schema import (
    AgentCapabilities,
    AgentMessageChunk,
    AudioContentBlock,
    ClientCapabilities,
    EmbeddedResourceContentBlock,
    ImageContentBlock,
    Implementation,
    PermissionOption,
    ResourceContentBlock,
    TextContentBlock,
    ToolCallUpdate,
    UserMessageChunk,
)
import neograph_engine as ng
from pydantic import TypeAdapter

from _protocol_demo import make_adapter


CONTENT_BLOCK = TypeAdapter(
    TextContentBlock
    | ImageContentBlock
    | AudioContentBlock
    | ResourceContentBlock
    | EmbeddedResourceContentBlock
)
SESSION_ID = re.compile(r"[0-9a-f]{32}")


# agent-client-protocol 1.0 removed the text_block / update_* helpers that
# 0.12 exported; the schema models are the same in both lines.
def text_block(text: str) -> TextContentBlock:
    return TextContentBlock(type="text", text=text)


def update_agent_message(block) -> AgentMessageChunk:
    return AgentMessageChunk(session_update="agent_message_chunk", content=block)


def update_agent_message_text(text: str) -> AgentMessageChunk:
    return update_agent_message(text_block(text))


def update_user_message(block) -> UserMessageChunk:
    return UserMessageChunk(session_update="user_message_chunk", content=block)


def update_user_message_text(text: str) -> UserMessageChunk:
    return update_user_message(text_block(text))


def permission_request(interrupt):
    """The ACP permission request for a graph interrupt.

    The question is the tool call's title and the interrupt payload its rawInput. The option
    ids are what the paused node reads as its resume value.
    """
    tool_call = ToolCallUpdate(
        tool_call_id="neograph-interrupt-" + (interrupt.checkpoint_id or interrupt.thread_id),
        title=interrupt.prompt,
        kind="other",
        status="pending",
        raw_input=interrupt.payload,
        field_meta={"neograph/interrupt": {
            "node": interrupt.node,
            "reason": interrupt.reason,
            "payload": interrupt.payload,
            "checkpointId": interrupt.checkpoint_id,
        }},
    )
    options = [
        PermissionOption(option_id="approve", name="Approve", kind="allow_once"),
        PermissionOption(option_id="reject", name="Reject", kind="reject_once"),
    ]
    return tool_call, options


def checkpoint_thread_id(session_id: str) -> str:
    if SESSION_ID.fullmatch(session_id) is None:
        raise RequestError.resource_not_found(session_id)
    return f"acp:{session_id}"


class NeoGraphACPAgent(Agent):
    def __init__(self, database_url=None, sqlite_path=None, checkpoint_store=None):
        database_url = database_url or os.environ.get("NEOGRAPH_ACP_POSTGRES_URL") or None
        sqlite_path = sqlite_path or os.environ.get("NEOGRAPH_ACP_SQLITE_PATH") or None
        configured = sum(
            value is not None
            for value in (database_url, sqlite_path, checkpoint_store)
        )
        if configured > 1:
            raise ValueError(
                "configure only one ACP checkpoint backend: PostgreSQL, "
                "SQLite, or checkpoint_store"
            )
        if checkpoint_store is None and database_url:
            checkpoint_store = ng.PostgresCheckpointStore(
                database_url, pool_size=1
            )
        elif checkpoint_store is None and sqlite_path:
            if not hasattr(ng, "SqliteCheckpointStore"):
                raise RuntimeError(
                    "NEOGRAPH_ACP_SQLITE_PATH needs a wheel/source build with "
                    "NEOGRAPH_BUILD_SQLITE=ON"
                )
            checkpoint_store = ng.SqliteCheckpointStore(sqlite_path)
        self.durable = checkpoint_store is not None
        checkpoint_store = checkpoint_store or ng.InMemoryCheckpointStore()
        self.adapter = make_adapter(checkpoint_store=checkpoint_store)
        self.sessions = set()
        self.connection = None
        self.prompts = {}

    def on_connect(self, conn: Client):
        self.connection = conn

    async def initialize(
        self,
        protocol_version: int,
        client_capabilities: ClientCapabilities | None = None,
        client_info: Implementation | None = None,
        **kwargs: Any,
    ) -> InitializeResponse:
        return InitializeResponse(
            protocol_version=PROTOCOL_VERSION,
            agent_capabilities=AgentCapabilities(load_session=self.durable),
            agent_info=Implementation(
                name="neograph-agent",
                title="NeoGraph Agent",
                version="1.0.0",
            ),
        )

    async def new_session(self, cwd: str, **kwargs: Any) -> NewSessionResponse:
        session_id = uuid4().hex
        self.sessions.add(session_id)
        return NewSessionResponse(session_id=session_id, modes=None)

    async def load_session(
        self, cwd: str, session_id: str, **kwargs: Any
    ) -> LoadSessionResponse:
        if self.connection is None:
            raise RuntimeError("ACP client is not connected")
        if not self.durable:
            raise RequestError.method_not_found("session/load")
        thread_id = checkpoint_thread_id(session_id)
        if not self.adapter.has_state(thread_id):
            raise RequestError.resource_not_found(session_id)
        self.sessions.add(session_id)
        state = self.adapter.get_state(thread_id)
        messages = state["channels"]["messages"]["value"]
        for message in messages:
            content = message.get("content", "")
            role = message.get("role")
            if role not in ("user", "assistant"):
                continue
            if isinstance(content, str):
                update = (
                    update_user_message_text(content)
                    if role == "user"
                    else update_agent_message_text(content)
                )
                await self.connection.session_update(session_id, update)
            elif isinstance(content, list):
                for raw_block in content:
                    block = CONTENT_BLOCK.validate_python(raw_block)
                    update = (
                        update_user_message(block)
                        if role == "user"
                        else update_agent_message(block)
                    )
                    await self.connection.session_update(session_id, update)
        return LoadSessionResponse()

    async def prompt(self, session_id: str, prompt: list, **kwargs: Any):
        if self.connection is None:
            raise RuntimeError("ACP client is not connected")
        if session_id not in self.sessions:
            raise RequestError.resource_not_found(session_id)
        task = asyncio.current_task()
        if task is None:
            raise RuntimeError("ACP prompt needs an asyncio task")
        self.prompts.setdefault(session_id, set()).add(task)
        blocks = [
            block
            if isinstance(block, dict)
            else block.model_dump(by_alias=True, exclude_none=True)
            for block in prompt
        ]
        text_only = all(block.get("type") == "text" for block in blocks)
        payload = (
            "\n".join(block.get("text", "") for block in blocks)
            if text_only
            else blocks
        )
        thread_id = None
        asked = False
        try:
            thread_id = checkpoint_thread_id(session_id)
            saw_token = False
            final_answer = ""
            events = self.adapter.stream(
                payload,
                thread_id=thread_id,
                request_id=session_id,
                interrupts=True,
            )
            while True:
                interrupt = None
                async for event in events:
                    if event.kind == "token":
                        saw_token = True
                        await self.connection.session_update(
                            session_id, update_agent_message_text(event.data)
                        )
                    elif event.kind == "interrupt":
                        interrupt = event.interrupt
                    else:
                        final_answer = event.data
                if interrupt is None:
                    break
                # The graph paused: ask the client, then resume with its answer.
                asked = True
                tool_call, options = permission_request(interrupt)
                answer = await self.connection.request_permission(
                    session_id=session_id, tool_call=tool_call, options=options
                )
                if answer.outcome.outcome != "selected":
                    self.adapter.discard_interrupt(thread_id)
                    return PromptResponse(stop_reason="cancelled")
                events = self.adapter.resume_stream(
                    answer.outcome.option_id,
                    thread_id=thread_id,
                    request_id=session_id,
                    interrupt=interrupt,
                )
            if not saw_token:
                await self.connection.session_update(
                    session_id, update_agent_message(text_block(final_answer))
                )
            return PromptResponse(stop_reason="end_turn")
        except asyncio.CancelledError:
            if asked:  # a cancelled turn leaves no question waiting
                self.adapter.discard_interrupt(thread_id)
            return PromptResponse(stop_reason="cancelled")
        except Exception:
            if asked:  # nor does a client that went away, or a resumed turn that failed
                self.adapter.discard_interrupt(thread_id)
            raise
        finally:
            prompts = self.prompts.get(session_id)
            if prompts is not None:
                prompts.discard(task)
                if not prompts:
                    self.prompts.pop(session_id, None)

    async def cancel(self, session_id: str, **kwargs: Any):
        self.adapter.cancel(session_id)
        current = asyncio.current_task()
        for task in tuple(self.prompts.get(session_id, ())):
            if task is not current:
                task.cancel()


async def main():
    await run_agent(NeoGraphACPAgent())


class PrintingClient:
    """The editor side of --self-check: it collects the text the agent streams and approves."""

    def __init__(self):
        self.chunks = []
        self.questions = []

    async def session_update(self, session_id, update, **kwargs):
        self.chunks.append(getattr(update.content, "text", ""))

    async def request_permission(self, session_id, tool_call, options, **kwargs):
        from acp.schema import AllowedOutcome, RequestPermissionResponse

        self.questions.append(tool_call.title)
        approve = next(option for option in options if option.kind == "allow_once")
        return RequestPermissionResponse(
            outcome=AllowedOutcome(outcome="selected", option_id=approve.option_id)
        )

    async def ext_method(self, method, params):
        raise RuntimeError(method)

    async def ext_notification(self, method, params):
        raise RuntimeError(method)


async def self_check():
    """Run this agent as a child process: two turns, then a command the client approves."""
    from acp import connect_to_agent

    process = await asyncio.create_subprocess_exec(
        sys.executable, os.path.abspath(__file__),
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )
    client = PrintingClient()
    connection = connect_to_agent(client, process.stdin, process.stdout)
    try:
        await connection.initialize(
            protocol_version=PROTOCOL_VERSION,
            client_capabilities=ClientCapabilities(),
            client_info=Implementation(name="self-check", version="1.0"),
        )
        session = await connection.new_session(cwd=os.getcwd(), mcp_servers=[])
        for turn, text in enumerate(("hello", "hello again", "approve: deploy"), start=1):
            client.chunks.clear()
            client.questions.clear()
            reply = await connection.prompt(
                session_id=session.session_id,
                prompt=[TextContentBlock(type="text", text=text)],
            )
            expected = f"(turn {turn})"
            for _ in range(200):  # the last chunks can arrive just after the response
                if "".join(client.chunks).endswith(expected):
                    break
                await asyncio.sleep(0.02)
            for question in client.questions:
                print(f"asked: {question} -> approve")
            print("".join(client.chunks), f"[{reply.stop_reason}]")
    finally:
        await connection.close()
        if process.returncode is None:
            process.terminate()
        await process.wait()


if __name__ == "__main__":
    asyncio.run(self_check() if "--self-check" in sys.argv else main())
