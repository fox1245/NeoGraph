"""Official a2a-sdk (Python) A2A server with a deterministic stub executor.

Usage: python -I sdk_py_server.py <port> [--compat03] [--v03-first] [--rpc-path /a2a/v1]
                                        [--record FILE.jsonl]

The executor picks its behaviour from the first word of the user text:
  echo      working -> text artifact -> completed (with a status message)
  message   replies with a direct Message (no Task)
  fail      failed          reject   rejected
  interrupt input-required  auth     auth-required
  slow      working, 3 s (cancelable) -> completed
Every HTTP request (method, path, headers, body) is appended to --record so the
caller can validate the requests NeoGraph's client puts on the wire.
"""

from __future__ import annotations

import argparse
import asyncio
import json

import uvicorn
from starlette.applications import Starlette

from a2a.helpers import get_message_text, new_task_from_user_message, new_text_message, new_text_part
from a2a.server.agent_execution import AgentExecutor, RequestContext
from a2a.server.events import EventQueue
from a2a.server.request_handlers import DefaultRequestHandler
from a2a.server.routes import create_agent_card_routes, create_jsonrpc_routes
from a2a.server.tasks import InMemoryTaskStore, TaskUpdater
from a2a.types import AgentCapabilities, AgentCard, AgentInterface, AgentSkill, Role, TaskState


class StubExecutor(AgentExecutor):
    def __init__(self):
        self.cancelled: set[str] = set()

    async def execute(self, context: RequestContext, event_queue: EventQueue):
        text = get_message_text(context.message) or ""
        word = (text.split() or [""])[0]
        if word == "message":
            await event_queue.enqueue_event(new_text_message(
                f"direct reply to {text}", role=Role.ROLE_AGENT, context_id=context.context_id))
            return
        task = context.current_task
        if task is None:
            task = new_task_from_user_message(context.message)
            await event_queue.enqueue_event(task)
        up = TaskUpdater(event_queue=event_queue, task_id=task.id, context_id=task.context_id)
        reply = lambda s: new_text_message(s, role=Role.ROLE_AGENT, context_id=task.context_id,  # noqa: E731
                                           task_id=task.id)
        await up.update_status(state=TaskState.TASK_STATE_WORKING, message=reply("working"))
        if word == "fail":
            await up.failed(reply("stub failure"))
        elif word == "reject":
            await up.reject(reply("stub rejection"))
        elif word == "interrupt":
            await up.requires_input(reply("need more input"))
        elif word == "auth":
            await up.requires_auth(reply("need auth"))
        else:
            if word == "slow":
                for _ in range(30):
                    if task.id in self.cancelled:
                        return
                    await asyncio.sleep(0.1)
            await up.add_artifact(parts=[new_text_part(text=f"artifact for {text}", media_type="text/plain")],
                                  artifact_id=f"{task.id}-out", last_chunk=True)
            await up.complete(reply(f"done: {text}"))

    async def cancel(self, context: RequestContext, event_queue: EventQueue):
        self.cancelled.add(context.task_id or "")
        up = TaskUpdater(event_queue=event_queue, task_id=context.task_id or "",
                         context_id=context.context_id or "")
        await up.cancel()


class Recorder:
    """ASGI middleware: append every HTTP request to a JSONL file."""

    def __init__(self, app, path):
        self.app, self.path = app, path

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http" or not self.path:
            return await self.app(scope, receive, send)
        chunks, more = [], True
        while more:
            m = await receive()
            chunks.append(m.get("body", b""))
            more = m.get("more_body", False)
        body = b"".join(chunks)
        with open(self.path, "a") as f:
            f.write(json.dumps({
                "method": scope["method"], "path": scope["path"],
                "query": scope.get("query_string", b"").decode(),
                "headers": {k.decode(): v.decode() for k, v in scope["headers"]},
                "body": body.decode(errors="replace"),
            }) + "\n")
        sent = False

        async def replay():
            nonlocal sent
            if not sent:
                sent = True
                return {"type": "http.request", "body": body, "more_body": False}
            return await receive()

        return await self.app(scope, replay, send)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", type=int)
    ap.add_argument("--compat03", action="store_true")
    ap.add_argument("--v03-first", action="store_true")
    ap.add_argument("--record", default="")
    ap.add_argument("--rpc-path", default="/")
    a = ap.parse_args()
    url = f"http://127.0.0.1:{a.port}{a.rpc_path}"
    ifaces = [AgentInterface(protocol_binding="JSONRPC", protocol_version="1.0", url=url)]
    if a.compat03:
        legacy = AgentInterface(protocol_binding="JSONRPC", protocol_version="0.3", url=url)
        ifaces = [legacy, *ifaces] if a.v03_first else [*ifaces, legacy]
    card = AgentCard(
        name="sdk-stub", description="a2a-sdk reference server for NeoGraph client probes",
        version="1.0.0", capabilities=AgentCapabilities(streaming=True),
        default_input_modes=["text/plain"], default_output_modes=["text/plain"],
        supported_interfaces=ifaces,
        skills=[AgentSkill(id="stub", name="stub", description="stub skill", tags=["stub"])],
    )
    handler = DefaultRequestHandler(agent_executor=StubExecutor(), task_store=InMemoryTaskStore(),
                                    agent_card=card)
    app = Starlette(routes=[*create_agent_card_routes(card),
                            *create_jsonrpc_routes(handler, a.rpc_path, enable_v0_3_compat=a.compat03)])
    uvicorn.run(Recorder(app, a.record), host="127.0.0.1", port=a.port, log_level="warning")


if __name__ == "__main__":
    main()
