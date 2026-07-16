"""Host a NeoGraph agent through the official ACP Python SDK over stdio.

Install the optional protocol dependency first:
    pip install "neograph-engine[acp]"

ACP clients launch this script as a subprocess. Protocol messages use stdout,
so application diagnostics must go to stderr or a logging handler. Set
NEOGRAPH_ACP_POSTGRES_URL for durable session/load support. Source builds with
SQLite enabled may use NEOGRAPH_ACP_SQLITE_PATH instead.
"""

import asyncio
import os
import re
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
    text_block,
    update_agent_message,
    update_agent_message_text,
    update_user_message,
    update_user_message_text,
)
from acp.interfaces import Client
from acp.schema import (
    AgentCapabilities,
    AudioContentBlock,
    ClientCapabilities,
    EmbeddedResourceContentBlock,
    ImageContentBlock,
    Implementation,
    ResourceContentBlock,
    TextContentBlock,
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


def checkpoint_thread_id(session_id: str) -> str:
    if SESSION_ID.fullmatch(session_id) is None:
        raise RequestError.resource_not_found(session_id)
    return f"acp:{session_id}"


class NeoGraphACPAgent(Agent):
    def __init__(self, database_url=None, sqlite_path=None, checkpoint_store=None):
        database_url = database_url or os.environ.get(
            "NEOGRAPH_ACP_POSTGRES_URL"
        )
        sqlite_path = sqlite_path or os.environ.get("NEOGRAPH_ACP_SQLITE_PATH")
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
        try:
            saw_token = False
            final_answer = ""
            async for event in self.adapter.stream(
                payload,
                thread_id=checkpoint_thread_id(session_id),
                request_id=session_id,
            ):
                if event.kind == "token":
                    saw_token = True
                    await self.connection.session_update(
                        session_id, update_agent_message_text(event.data)
                    )
                else:
                    final_answer = event.data
            if not saw_token:
                await self.connection.session_update(
                    session_id, update_agent_message(text_block(final_answer))
                )
            return PromptResponse(stop_reason="end_turn")
        except asyncio.CancelledError:
            return PromptResponse(stop_reason="cancelled")
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


if __name__ == "__main__":
    asyncio.run(main())
