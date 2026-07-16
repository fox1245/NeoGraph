"""Host a NeoGraph agent through the official ACP Python SDK over stdio.

Install the optional protocol dependency first:
    pip install "neograph-engine[acp]"

ACP clients launch this script as a subprocess. Protocol messages use stdout,
so application diagnostics must go to stderr or a logging handler.
"""

import asyncio
from typing import Any
from uuid import uuid4

from acp import (
    PROTOCOL_VERSION,
    Agent,
    InitializeResponse,
    NewSessionResponse,
    PromptResponse,
    run_agent,
    text_block,
    update_agent_message,
)
from acp.interfaces import Client
from acp.schema import AgentCapabilities, ClientCapabilities, Implementation

from _protocol_demo import make_adapter


class NeoGraphACPAgent(Agent):
    def __init__(self):
        self.adapter = make_adapter()
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
            agent_capabilities=AgentCapabilities(),
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

    async def prompt(self, session_id: str, prompt: list, **kwargs: Any):
        if self.connection is None:
            raise RuntimeError("ACP client is not connected")
        self.sessions.add(session_id)
        task = asyncio.current_task()
        if task is None:
            raise RuntimeError("ACP prompt needs an asyncio task")
        self.prompts.setdefault(session_id, set()).add(task)
        text = "\n".join(
            block.get("text", "")
            if isinstance(block, dict)
            else getattr(block, "text", "")
            for block in prompt
        )
        try:
            answer = await self.adapter.run(
                text, thread_id=session_id, request_id=session_id
            )
            await self.connection.session_update(
                session_id, update_agent_message(text_block(answer))
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
