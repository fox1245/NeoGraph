"""Host a NeoGraph agent through the official A2A Python SDK.

Install the optional server dependency first:
    pip install "neograph-engine[a2a]"

Then run this file and send A2A 1.0 JSON-RPC requests to
http://127.0.0.1:9999/. The agent card is served from the SDK's standard
well-known route.
"""

import os

import uvicorn

from a2a.helpers import (
    get_message_text,
    new_task_from_user_message,
    new_text_message,
    new_text_part,
)
from a2a.server.agent_execution import AgentExecutor, RequestContext
from a2a.server.events import EventQueue
from a2a.server.request_handlers import DefaultRequestHandler
from a2a.server.routes import create_agent_card_routes, create_jsonrpc_routes
from a2a.server.tasks import InMemoryTaskStore, TaskUpdater
from a2a.types import (
    AgentCapabilities,
    AgentCard,
    AgentInterface,
    AgentSkill,
    TaskState,
)
from starlette.applications import Starlette

from _protocol_demo import make_adapter


HOST = os.environ.get("NEOGRAPH_A2A_HOST", "127.0.0.1")
PORT = int(os.environ.get("NEOGRAPH_A2A_PORT", "9999"))
PUBLIC_URL = os.environ.get(
    "NEOGRAPH_A2A_PUBLIC_URL", f"http://127.0.0.1:{PORT}/"
)


class NeoGraphAgentExecutor(AgentExecutor):
    def __init__(self):
        self.adapter = make_adapter()

    async def execute(self, context: RequestContext, event_queue: EventQueue):
        task = context.current_task
        if task is None:
            task = new_task_from_user_message(context.message)
            await event_queue.enqueue_event(task)

        updater = TaskUpdater(
            event_queue=event_queue,
            task_id=task.id,
            context_id=task.context_id,
        )
        await updater.update_status(
            state=TaskState.TASK_STATE_WORKING,
            message=new_text_message("NeoGraph is running"),
        )
        pending_token = None
        emitted = False
        final_answer = ""
        async for event in self.adapter.stream(
            get_message_text(context.message) or "",
            thread_id=task.context_id,
            request_id=task.id,
        ):
            if event.kind == "token":
                if pending_token is not None:
                    await updater.add_artifact(
                        parts=[new_text_part(
                            text=pending_token, media_type="text/plain"
                        )],
                        artifact_id=f"{task.id}-response",
                        append=emitted,
                        last_chunk=False,
                    )
                    emitted = True
                pending_token = event.data
            else:
                final_answer = event.data

        await updater.add_artifact(
            parts=[new_text_part(
                text=pending_token if pending_token is not None else final_answer,
                media_type="text/plain",
            )],
            artifact_id=f"{task.id}-response",
            append=emitted,
            last_chunk=True,
        )
        await updater.update_status(state=TaskState.TASK_STATE_COMPLETED)

    async def cancel(self, context: RequestContext, event_queue: EventQueue):
        task_id = context.task_id or ""
        self.adapter.cancel(task_id)
        updater = TaskUpdater(
            event_queue=event_queue,
            task_id=task_id,
            context_id=context.context_id or "",
        )
        await updater.cancel()


skill = AgentSkill(
    id="neograph_echo",
    name="NeoGraph echo",
    description="Runs a stateful NeoGraph chat graph.",
    tags=["neograph", "chat"],
    examples=["hello"],
    input_modes=["text/plain"],
    output_modes=["text/plain"],
)
card = AgentCard(
    name="NeoGraph A2A Agent",
    description="A NeoGraph engine hosted by the official A2A SDK.",
    version="1.0.0",
    capabilities=AgentCapabilities(streaming=True),
    default_input_modes=["text/plain"],
    default_output_modes=["text/plain"],
    supported_interfaces=[
        AgentInterface(
            protocol_binding="JSONRPC",
            protocol_version="1.0",
            url=PUBLIC_URL,
        )
    ],
    skills=[skill],
)
handler = DefaultRequestHandler(
    agent_executor=NeoGraphAgentExecutor(),
    task_store=InMemoryTaskStore(),
    agent_card=card,
)
routes = [
    *create_agent_card_routes(card),
    *create_jsonrpc_routes(handler, "/"),
]
app = Starlette(routes=routes)


if __name__ == "__main__":
    uvicorn.run(app, host=HOST, port=PORT)
