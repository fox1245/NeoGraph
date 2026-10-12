"""Official a2a-sdk (Python) client -> NeoGraph native A2AServer, both wire dialects.

The SDK is the oracle: a feature conforms only when the SDK accepts it with no
exception, no Python warning and no WARNING-level log record.  The 1.0 run uses
the card as served (SDK prefers the 1.0 interface); the 0.3 run hands the SDK a
card that only lists the 0.3 interface, so its CompatJsonRpcTransport is used.
"""

from __future__ import annotations

import asyncio
import logging
import warnings

import pytest

from a2a_harness import Transcript, native_server  # skips unless the pinned SDK is installed

import httpx  # noqa: E402
from a2a.client import A2ACardResolver, ClientConfig, create_client  # noqa: E402
from a2a.helpers import new_text_message  # noqa: E402
from a2a.types import (  # noqa: E402
    CancelTaskRequest,
    GetExtendedAgentCardRequest,
    GetTaskRequest,
    ListTasksRequest,
    Role,
    SendMessageRequest,
    SubscribeToTaskRequest,
    TaskState,
)
from a2a.utils import errors as a2a_errors  # noqa: E402

T = Transcript("sdk_py_client_native_server")
DIALECTS = ["1.0", "0.3"]


@pytest.fixture(scope="module")
def srv():
    with native_server() as base:
        yield base


class _Warnings(logging.Handler):
    def __init__(self):
        super().__init__(logging.WARNING)
        self.records = []

    def emit(self, record):
        self.records.append(f"{record.name}: {record.getMessage()[:300]}")


async def _client(base: str, dialect: str, streaming: bool):
    async with httpx.AsyncClient() as http:
        card = await A2ACardResolver(http, base).get_agent_card()
    if dialect == "0.3":
        keep = [i for i in card.supported_interfaces if i.protocol_version.startswith("0.3")]
        del card.supported_interfaces[:]
        card.supported_interfaces.extend(keep)
    return await create_client(agent=card, client_config=ClientConfig(streaming=streaming)), card


def _run(test: str, coro_fn):
    """Run one scenario; FAIL on exception, warning or WARNING log record."""
    handler = _Warnings()
    logging.getLogger().addHandler(handler)
    try:
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            outcome = asyncio.run(coro_fn())
    except Exception as e:  # noqa: BLE001 - the SDK rejection is the finding
        T.add(test=test, result="FAIL", sdk_exception=f"{type(e).__module__}.{type(e).__name__}",
              sdk_message=str(e)[:2000], warnings=handler.records)
        print(f"FAIL {test} :: {type(e).__name__}: {str(e)[:300]}")
        raise
    finally:
        logging.getLogger().removeHandler(handler)
    noise = handler.records + [str(w.message)[:300] for w in caught]
    ok = not noise
    T.add(test=test, result="PASS" if ok else "FAIL", outcome=str(outcome)[:2000], warnings=noise)
    print(("PASS " if ok else "FAIL ") + test + ("" if ok else f" :: warnings {noise}"))
    if not ok:
        pytest.fail(f"SDK accepted but warned: {noise}")
    return outcome


def _req(text, **kw):
    msg = new_text_message(text, role=Role.ROLE_USER)
    for k, v in kw.items():
        setattr(msg, k, v)
    return SendMessageRequest(message=msg)


@pytest.mark.parametrize("dialect", DIALECTS)
def test_card_resolution(srv, dialect):
    async def go():
        client, card = await _client(srv, dialect, False)
        await client.close()
        return card
    card = _run(f"[{dialect}] card.resolve", go)
    assert card.capabilities.streaming


@pytest.mark.parametrize("dialect", DIALECTS)
def test_send_message_unary(srv, dialect):
    async def go():
        client, _ = await _client(srv, dialect, False)
        chunks = [c async for c in client.send_message(_req("echo unary"))]
        await client.close()
        return chunks
    chunks = _run(f"[{dialect}] SendMessage.unary", go)
    task = chunks[-1].task
    assert task.status.state == TaskState.TASK_STATE_COMPLETED, chunks


@pytest.mark.parametrize("dialect", DIALECTS)
def test_send_message_stream_with_artifact(srv, dialect):
    async def go():
        client, _ = await _client(srv, dialect, True)
        chunks = [c async for c in client.send_message(_req("artifact stream"))]
        await client.close()
        return chunks
    chunks = _run(f"[{dialect}] SendStreamingMessage", go)
    kinds = [c.WhichOneof("payload") for c in chunks]
    T.add(test=f"[{dialect}] SendStreamingMessage.kinds", kinds=kinds)
    if dialect == "1.0":
        assert kinds[0] == "task", kinds  # spec 3.1.2: a task lifecycle stream begins with the Task
    seen_artifact = "artifact_update" in kinds or any(
        c.WhichOneof("payload") == "task" and len(c.task.artifacts) for c in chunks)
    assert seen_artifact, f"no artifact reached the SDK client: {kinds}"
    last = chunks[-1]
    final_state = last.status_update.status.state if kinds[-1] == "status_update" else last.task.status.state
    assert final_state == TaskState.TASK_STATE_COMPLETED, chunks


@pytest.mark.parametrize("dialect", DIALECTS)
def test_get_task_and_not_found(srv, dialect):
    async def go():
        client, _ = await _client(srv, dialect, False)
        chunks = [c async for c in client.send_message(_req("echo get"))]
        tid = chunks[-1].task.id
        got = await client.get_task(GetTaskRequest(id=tid))
        try:
            await client.get_task(GetTaskRequest(id="no-such-task"))
            nf = None
        except a2a_errors.TaskNotFoundError as e:
            nf = e
        await client.close()
        return got, nf
    got, nf = _run(f"[{dialect}] GetTask", go)
    assert got.status.state == TaskState.TASK_STATE_COMPLETED
    assert nf is not None


@pytest.mark.parametrize("dialect", DIALECTS)
def test_cancel_running_task(srv, dialect):
    async def go():
        client, _ = await _client(srv, dialect, True)
        events = []
        async for c in client.send_message(_req("slow cancel")):
            events.append(c)
            if len(events) == 1:
                tid = c.task.id if c.WhichOneof("payload") == "task" else c.status_update.task_id
                canceled = await client.cancel_task(CancelTaskRequest(id=tid))
        await client.close()
        return canceled, events
    canceled, events = _run(f"[{dialect}] CancelTask.running", go)
    assert canceled.status.state == TaskState.TASK_STATE_CANCELED


@pytest.mark.parametrize("dialect", DIALECTS)
def test_cancel_terminal_task_raises_not_cancelable(srv, dialect):
    async def go():
        client, _ = await _client(srv, dialect, False)
        chunks = [c async for c in client.send_message(_req("echo terminal"))]
        tid = chunks[-1].task.id
        try:
            t = await client.cancel_task(CancelTaskRequest(id=tid))
            raise AssertionError(f"SDK expected TaskNotCancelableError (spec 3.1.5); NeoGraph "
                                 f"returned task in state {TaskState.Name(t.status.state)}")
        except a2a_errors.TaskNotCancelableError as e:
            return str(e)
        finally:
            await client.close()
    _run(f"[{dialect}] CancelTask.terminal->TaskNotCancelableError", go)


@pytest.mark.parametrize("dialect", DIALECTS)
def test_list_tasks(srv, dialect):
    if dialect == "0.3":
        pytest.skip("0.3 defines no tasks/list method (SDK: NotImplementedError)")
    async def go():
        client, _ = await _client(srv, dialect, False)
        try:
            return await client.list_tasks(ListTasksRequest())
        finally:
            await client.close()
    _run(f"[{dialect}] ListTasks", go)


@pytest.mark.parametrize("dialect", DIALECTS)
def test_subscribe_to_running_task(srv, dialect):
    async def go():
        client, _ = await _client(srv, dialect, True)
        try:
            agen = client.send_message(_req("slow subscribe"))
            first = await agen.__anext__()
            tid = first.task.id if first.WhichOneof("payload") == "task" else first.status_update.task_id
            sub = [c async for c in client.subscribe(SubscribeToTaskRequest(id=tid))]
            async for _ in agen:
                pass
            return sub
        finally:
            await client.close()
    _run(f"[{dialect}] SubscribeToTask", go)


@pytest.mark.parametrize("dialect", DIALECTS)
def test_extended_card_not_declared(srv, dialect):
    # The SDK short-circuits to the public card when capabilities.extendedAgentCard
    # is unset, so it never reaches the server; the raw-wire test covers the code.
    async def go():
        client, card = await _client(srv, dialect, False)
        try:
            return await client.get_extended_agent_card(GetExtendedAgentCardRequest())
        finally:
            await client.close()
    _run(f"[{dialect}] GetExtendedAgentCard(not declared)", go)


@pytest.mark.parametrize("dialect", DIALECTS)
@pytest.mark.parametrize("prompt,state", [
    ("fail", TaskState.TASK_STATE_FAILED),
    ("interrupt", TaskState.TASK_STATE_INPUT_REQUIRED),
])
def test_failure_states(srv, dialect, prompt, state):
    async def go():
        client, _ = await _client(srv, dialect, True)
        try:
            return [c async for c in client.send_message(_req(prompt))]
        finally:
            await client.close()
    chunks = _run(f"[{dialect}] stream.{prompt}", go)
    last = chunks[-1]
    got = (last.status_update.status.state if last.WhichOneof("payload") == "status_update"
           else last.task.status.state)
    assert got == state, chunks


@pytest.mark.parametrize("dialect", DIALECTS)
def test_follow_up_on_terminal_task_rejected(srv, dialect):
    async def go():
        client, _ = await _client(srv, dialect, False)
        try:
            chunks = [c async for c in client.send_message(_req("echo one"))]
            t = chunks[-1].task
            try:
                again = [c async for c in client.send_message(
                    _req("echo two", task_id=t.id, context_id=t.context_id))]
            except a2a_errors.UnsupportedOperationError as e:
                return str(e)
            raise AssertionError("expected UnsupportedOperationError for a terminal task (3.1.1); "
                                 f"NeoGraph re-ran it: {again[-1]}")
        finally:
            await client.close()
    _run(f"[{dialect}] SendMessage.terminal_task->UnsupportedOperationError", go)
