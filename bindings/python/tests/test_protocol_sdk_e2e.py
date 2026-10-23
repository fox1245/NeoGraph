"""Official A2A/ACP SDK integration tests.

The normal test suite skips this module when protocol extras are absent. CI has
a dedicated Python 3.12 job that installs both SDKs and runs these tests.
"""

import asyncio
import contextlib
import os
import runpy
import sys
from pathlib import Path
from types import SimpleNamespace
from uuid import uuid4

import pytest


pytest.importorskip("a2a")
pytest.importorskip("acp")

import httpx
from a2a.client import A2ACardResolver, ClientConfig, create_client
from a2a.helpers import get_message_text, new_text_message
from a2a.types import (
    CancelTaskRequest,
    Role,
    SendMessageRequest,
    Task,
    TaskArtifactUpdateEvent,
    TaskState,
    TaskStatusUpdateEvent,
)
from acp import (
    PROTOCOL_VERSION,
    RequestError,
    connect_to_agent,
)
from acp.schema import (
    AllowedOutcome,
    ClientCapabilities,
    DeniedOutcome,
    ImageContentBlock,
    Implementation,
    RequestPermissionResponse,
    TextContentBlock,
)
from google.protobuf.json_format import MessageToDict

import neograph_engine as ng
import server_ports


# agent-client-protocol 1.0 removed the text_block / image_block helpers of
# 0.12; the schema models work with both lines.
def text_block(text):
    return TextContentBlock(type="text", text=text)


def image_block(data, mime_type):
    return ImageContentBlock(type="image", data=data, mime_type=mime_type)


ROOT = Path(__file__).resolve().parents[3]
EXAMPLES = ROOT / "bindings" / "python" / "examples"
sys.path.insert(0, str(EXAMPLES))


def _subprocess_env(**updates):
    env = os.environ.copy()
    paths = [str(EXAMPLES)]
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
    async def exercise(base_url):
        async with httpx.AsyncClient() as http:
            card = await A2ACardResolver(http, base_url).get_agent_card()

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

    # The example takes its port from the environment and does not report it, so the
    # port contract applies (server_ports.py): a taken number starts the server again on
    # a fresh one, and the test only talks to the port once this process listens on it.
    launch = lambda port: server_ports.Launch(  # noqa: E731
        [sys.executable, str(EXAMPLES / "27_a2a_server.py")],
        _subprocess_env(NEOGRAPH_A2A_PORT=str(port)),
    )
    with server_ports.start_server(launch, reports_port=False) as server:
        asyncio.run(exercise(server.url()))


class _ACPClient:
    """The editor side: records what the agent streams and answers permission requests.

    `decision` is the option id to select, or "cancelled" for the cancelled outcome.
    """

    def __init__(self, decision="approve"):
        self.messages = []
        self.requests = []
        self.decision = decision

    async def session_update(self, session_id, update, **kwargs):
        self.messages.append(getattr(update.content, "text", ""))

    async def request_permission(self, session_id, tool_call, options, **kwargs):
        self.requests.append((session_id, tool_call, options))
        if self.decision == "cancelled":
            return RequestPermissionResponse(outcome=DeniedOutcome(outcome="cancelled"))
        return RequestPermissionResponse(
            outcome=AllowedOutcome(outcome="selected", option_id=self.decision)
        )

    async def ext_method(self, method, params):
        raise RuntimeError(method)

    async def ext_notification(self, method, params):
        raise RuntimeError(method)


async def _start_acp(extra_env=None, decision="approve"):
    process = await asyncio.create_subprocess_exec(
        sys.executable,
        str(EXAMPLES / "28_acp_agent.py"),
        env=_subprocess_env(**(extra_env or {})),
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    assert process.stdin is not None and process.stdout is not None
    client = _ACPClient(decision)
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


# -- a graph that pauses for a human -----------------------------------------
#
# The demo graph (examples/_protocol_demo.py) pauses on a prompt that starts with
# "approve:" and runs or skips the command according to the answer it is resumed with.


async def _a2a_client(base_url):
    async with httpx.AsyncClient() as http:
        card = await A2ACardResolver(http, base_url).get_agent_card()
    return await create_client(agent=card, client_config=ClientConfig(streaming=True))


async def _a2a_turn(client, text, context_id=None, task_id=None):
    """Send one A2A message; return what the server reported for it."""
    request = SendMessageRequest(
        message=new_text_message(
            text, role=Role.ROLE_USER, context_id=context_id, task_id=task_id
        )
    )
    turn = SimpleNamespace(
        task_id=task_id, context_id=context_id, states=[], chunks=[],
        question=None, payload=None, interrupt_metadata=None,
    )
    async for chunk in client.send_message(request):
        if chunk.HasField("task"):
            turn.task_id, turn.context_id = chunk.task.id, chunk.task.context_id
        elif chunk.HasField("artifact_update"):
            turn.chunks.append(
                "".join(part.text for part in chunk.artifact_update.artifact.parts)
            )
        elif chunk.HasField("status_update"):
            status = chunk.status_update.status
            turn.states.append(status.state)
            if status.state == TaskState.TASK_STATE_INPUT_REQUIRED:
                turn.question = get_message_text(status.message)
                turn.payload = [
                    MessageToDict(part.data)
                    for part in status.message.parts
                    if part.HasField("data")
                ]
                turn.interrupt_metadata = MessageToDict(chunk.status_update.metadata)
    return turn


def _serve_a2a_example():
    launch = lambda port: server_ports.Launch(  # noqa: E731
        [sys.executable, str(EXAMPLES / "27_a2a_server.py")],
        _subprocess_env(NEOGRAPH_A2A_PORT=str(port)),
    )
    return server_ports.start_server(launch, reports_port=False)


def test_a2a_graph_interrupt_is_input_required_and_a_follow_up_resumes_it():
    async def exercise(base_url):
        client = await _a2a_client(base_url)
        paused = await _a2a_turn(client, "approve: deploy")
        assert paused.states[-1] == TaskState.TASK_STATE_INPUT_REQUIRED
        assert TaskState.TASK_STATE_COMPLETED not in paused.states
        assert paused.question == "Run `deploy`?"
        assert paused.payload == [{"command": "deploy"}]
        assert paused.interrupt_metadata["neograph/interrupt"]["node"] == "echo"
        assert paused.interrupt_metadata["neograph/interrupt"]["checkpointId"]
        assert paused.interrupt_metadata["neograph/interrupt"]["interruptId"]
        assert paused.chunks == []

        # The next message to the same task answers the question and the turn
        # continues from the checkpoint, streaming like any other turn.
        done = await _a2a_turn(client, "approve", paused.context_id, paused.task_id)
        assert done.task_id == paused.task_id
        assert done.states[-1] == TaskState.TASK_STATE_COMPLETED
        assert done.chunks == ["NeoGraph ", "ran: ", "deploy", " (turn 1)"]

        # The conversation goes on in a new task of the same context.
        after = await _a2a_turn(client, "hello", paused.context_id)
        assert after.task_id != paused.task_id
        assert "received: hello" in "".join(after.chunks)
        await client.close()

    with _serve_a2a_example() as server:
        asyncio.run(exercise(server.url()))


def test_a2a_follow_up_text_is_the_resume_value():
    async def exercise(base_url):
        client = await _a2a_client(base_url)
        paused = await _a2a_turn(client, "approve: rollback")
        done = await _a2a_turn(client, "no", paused.context_id, paused.task_id)
        assert done.states[-1] == TaskState.TASK_STATE_COMPLETED
        assert "".join(done.chunks) == "NeoGraph skipped: rollback (turn 1)"
        await client.close()

    with _serve_a2a_example() as server:
        asyncio.run(exercise(server.url()))


def test_a2a_cancel_of_a_task_that_waits_for_input():
    async def exercise(base_url):
        client = await _a2a_client(base_url)
        paused = await _a2a_turn(client, "approve: deploy")
        cancelled = await client.cancel_task(CancelTaskRequest(id=paused.task_id))
        assert cancelled.status.state == TaskState.TASK_STATE_CANCELED
        with pytest.raises(Exception):
            await _a2a_turn(client, "approve", paused.context_id, paused.task_id)
        await client.close()

    with _serve_a2a_example() as server:
        asyncio.run(exercise(server.url()))


def test_a2a_executor_resumes_from_the_task_metadata_after_a_restart():
    namespace = runpy.run_path(str(EXAMPLES / "27_a2a_server.py"))

    class Queue:
        def __init__(self):
            self.events = []

        async def enqueue_event(self, event):
            self.events.append(event)

    def request(message, task=None):
        return SimpleNamespace(
            message=message, current_task=task,
            task_id=task.id if task else None,
            context_id=task.context_id if task else None,
        )

    async def exercise():
        executor = namespace["NeoGraphAgentExecutor"]()
        first = Queue()
        await executor.execute(
            request(new_text_message("approve: deploy", role=Role.ROLE_USER)), first
        )
        task = next(e for e in first.events if isinstance(e, Task))
        asked = [e for e in first.events if isinstance(e, TaskStatusUpdateEvent)][-1]
        assert asked.status.state == TaskState.TASK_STATE_INPUT_REQUIRED
        # What an SDK task store keeps for the waiting task.
        task.status.state = TaskState.TASK_STATE_INPUT_REQUIRED
        task.metadata.MergeFrom(asked.metadata)

        # The agent restarted: it no longer remembers the question, the task does.
        executor.adapter.discard_interrupt(task.context_id)
        assert executor.adapter.pending_interrupt(task.context_id) is None

        second = Queue()
        answer = new_text_message(
            "approve", role=Role.ROLE_USER,
            task_id=task.id, context_id=task.context_id,
        )
        await executor.execute(request(answer, task), second)
        states = [
            e.status.state for e in second.events if isinstance(e, TaskStatusUpdateEvent)
        ]
        text = "".join(
            part.text
            for e in second.events if isinstance(e, TaskArtifactUpdateEvent)
            for part in e.artifact.parts
        )
        assert states[-1] == TaskState.TASK_STATE_COMPLETED
        assert text == "NeoGraph ran: deploy (turn 1)"

    asyncio.run(exercise())


def test_a2a_executor_cancel_forgets_the_question_of_a_waiting_task():
    namespace = runpy.run_path(str(EXAMPLES / "27_a2a_server.py"))

    class Queue:
        def __init__(self):
            self.events = []

        async def enqueue_event(self, event):
            self.events.append(event)

    async def exercise():
        executor = namespace["NeoGraphAgentExecutor"]()
        asked = Queue()
        await executor.execute(
            SimpleNamespace(
                message=new_text_message("approve: deploy", role=Role.ROLE_USER),
                current_task=None, task_id=None, context_id=None,
            ),
            asked,
        )
        task = next(e for e in asked.events if isinstance(e, Task))
        assert executor.adapter.pending_interrupt(task.context_id) is not None

        cancelled = Queue()
        await executor.cancel(
            SimpleNamespace(task_id=task.id, context_id=task.context_id), cancelled
        )

        assert executor.adapter.pending_interrupt(task.context_id) is None
        assert [
            e.status.state for e in cancelled.events
            if isinstance(e, TaskStatusUpdateEvent)
        ] == [TaskState.TASK_STATE_CANCELED]

    asyncio.run(exercise())


class _TaskStoreQueue:
    """The event queue of an executor test, with what the SDK does to a task it keeps.

    The SDK applies each status update to the task object it handed to the executor, in
    place, when its consumer takes the event from the queue. `apply=True` does that at once.
    """

    def __init__(self, tasks, apply=True):
        self.events = []
        self.tasks = tasks
        self.apply = apply

    async def enqueue_event(self, event):
        self.events.append(event)
        if isinstance(event, Task):
            self.tasks[event.id] = event
        elif self.apply and isinstance(event, TaskStatusUpdateEvent):
            task = self.tasks[event.task_id]
            task.metadata.MergeFrom(event.metadata)
            task.status.CopyFrom(event.status)

    def states(self):
        return [
            e.status.state for e in self.events if isinstance(e, TaskStatusUpdateEvent)
        ]

    def text(self):
        return "".join(
            part.text
            for e in self.events if isinstance(e, TaskArtifactUpdateEvent)
            for part in e.artifact.parts
        )


async def _execute_message(executor, queue, text, task=None, context_id=None):
    message = new_text_message(
        text, role=Role.ROLE_USER,
        task_id=task.id if task else None,
        context_id=task.context_id if task else context_id,
    )
    await executor.execute(
        SimpleNamespace(
            message=message, current_task=task,
            task_id=task.id if task else None,
            context_id=task.context_id if task else context_id,
        ),
        queue,
    )


def test_a2a_executor_reads_the_state_of_the_task_before_it_publishes_working():
    """The SDK updates the task in place, so a state read after the update is `working`."""
    namespace = runpy.run_path(str(EXAMPLES / "27_a2a_server.py"))

    async def exercise():
        executor = namespace["NeoGraphAgentExecutor"]()
        tasks = {}
        asked = _TaskStoreQueue(tasks)
        await _execute_message(executor, asked, "approve: deploy")
        task, = tasks.values()
        assert task.status.state == TaskState.TASK_STATE_INPUT_REQUIRED

        answered = _TaskStoreQueue(tasks)
        await _execute_message(executor, answered, "approve", task=task)

        assert answered.states()[-1] == TaskState.TASK_STATE_COMPLETED
        assert answered.text() == "NeoGraph ran: deploy (turn 1)"

    asyncio.run(exercise())


def test_a2a_task_is_answered_from_its_own_pause_not_from_a_newer_question_of_its_context():
    """The adapter keeps one question per thread and the thread is the context."""
    namespace = runpy.run_path(str(EXAMPLES / "27_a2a_server.py"))

    async def exercise():
        executor = namespace["NeoGraphAgentExecutor"]()
        tasks = {}
        await _execute_message(executor, _TaskStoreQueue(tasks), "approve: one")
        first, = tasks.values()
        await _execute_message(
            executor, _TaskStoreQueue(tasks), "approve: two", context_id=first.context_id
        )
        second = next(task for task in tasks.values() if task.id != first.id)
        assert second.context_id == first.context_id
        assert second.status.state == TaskState.TASK_STATE_INPUT_REQUIRED
        # The adapter now remembers the question of the second task only.
        assert executor.adapter.pending_interrupt(first.context_id).checkpoint_id == (
            MessageToDict(second.metadata)["neograph/interrupt"]["checkpointId"]
        )

        # The first task still resumes the checkpoint it reported. (The SDK would hand the
        # executor the stored task; nothing is applied to it here.)
        answered = _TaskStoreQueue(tasks, apply=False)
        await _execute_message(executor, answered, "approve", task=first)
        assert answered.states()[-1] == TaskState.TASK_STATE_COMPLETED
        assert answered.text().startswith("NeoGraph ran: one ")

    asyncio.run(exercise())


def test_acp_graph_interrupt_asks_permission_and_the_selected_option_resumes_it(tmp_path):
    async def exercise(decision, expected):
        process, client, connection, _ = await _start_acp(decision=decision)
        try:
            session = await connection.new_session(cwd=str(tmp_path), mcp_servers=[])
            reply = await connection.prompt(
                session_id=session.session_id,
                prompt=[text_block("approve: deploy")],
            )
            assert reply.stop_reason == "end_turn"
            await _wait_messages(client, 4)
            assert "".join(client.messages) == expected
            assert len(client.messages) == 4  # streamed, not sent once more at the end

            (asked_session, tool_call, options), = client.requests
            assert asked_session == session.session_id
            assert tool_call.title == "Run `deploy`?"
            assert tool_call.raw_input == {"command": "deploy"}
            assert tool_call.kind == "other"
            assert tool_call.status == "pending"
            assert tool_call.field_meta["neograph/interrupt"]["node"] == "echo"
            assert [(o.option_id, o.kind) for o in options] == [
                ("approve", "allow_once"), ("reject", "reject_once"),
            ]
        finally:
            await _stop_process(process, connection)

    asyncio.run(exercise("approve", "NeoGraph ran: deploy (turn 1)"))
    asyncio.run(exercise("reject", "NeoGraph skipped: deploy (turn 1)"))


def test_acp_cancelled_permission_cancels_the_turn_and_the_session_goes_on(tmp_path):
    async def exercise():
        process, client, connection, _ = await _start_acp(decision="cancelled")
        try:
            session = await connection.new_session(cwd=str(tmp_path), mcp_servers=[])
            reply = await connection.prompt(
                session_id=session.session_id,
                prompt=[text_block("approve: deploy")],
            )
            assert reply.stop_reason == "cancelled"
            assert len(client.requests) == 1
            assert client.messages == []

            # The question is gone: the next prompt is a new turn, not an answer.
            reply = await connection.prompt(
                session_id=session.session_id, prompt=[text_block("hello")]
            )
            assert reply.stop_reason == "end_turn"
            await _wait_messages(client, 4)
            assert "".join(client.messages) == "NeoGraph received: hello (turn 2)"
            assert len(client.requests) == 1
        finally:
            await _stop_process(process, connection)

    asyncio.run(exercise())


def test_acp_cancel_while_waiting_for_the_permission_ends_the_turn_cancelled():
    namespace = runpy.run_path(str(EXAMPLES / "28_acp_agent.py"))

    class WaitingClient(_ACPClient):
        def __init__(self):
            super().__init__()
            self.asked = asyncio.Event()

        async def request_permission(self, session_id, tool_call, options, **kwargs):
            self.requests.append((session_id, tool_call, options))
            self.asked.set()
            await asyncio.Future()

    async def exercise():
        agent = namespace["NeoGraphACPAgent"]()
        agent.connection = WaitingClient()
        session_id = uuid4().hex
        agent.sessions.add(session_id)
        thread_id = f"acp:{session_id}"
        prompt = asyncio.create_task(
            agent.prompt(session_id, [text_block("approve: deploy")])
        )
        await asyncio.wait_for(agent.connection.asked.wait(), timeout=5)
        assert agent.adapter.pending_interrupt(thread_id) is not None

        await agent.cancel(session_id)
        response = await asyncio.wait_for(prompt, timeout=1)

        assert response.stop_reason == "cancelled"
        assert agent.connection.messages == []
        assert agent.adapter.pending_interrupt(thread_id) is None
        assert agent.prompts == {}

    asyncio.run(exercise())


def test_acp_a_client_that_goes_away_while_asked_leaves_no_question_behind():
    namespace = runpy.run_path(str(EXAMPLES / "28_acp_agent.py"))

    class GoneClient(_ACPClient):
        async def request_permission(self, session_id, tool_call, options, **kwargs):
            self.requests.append((session_id, tool_call, options))
            raise ConnectionError("the client went away")

    async def exercise():
        agent = namespace["NeoGraphACPAgent"]()
        agent.connection = GoneClient()
        session_id = uuid4().hex
        agent.sessions.add(session_id)

        with pytest.raises(ConnectionError):
            await agent.prompt(session_id, [text_block("approve: deploy")])

        assert len(agent.connection.requests) == 1
        assert agent.adapter.pending_interrupt(f"acp:{session_id}") is None
        assert agent.prompts == {}

    asyncio.run(exercise())


def test_acp_cancel_of_the_resumed_turn_cancels_the_run():
    namespace = runpy.run_path(str(EXAMPLES / "28_acp_agent.py"))
    from neograph_engine.protocol import ProtocolInterrupt

    class ResumeBlocks:
        def __init__(self):
            self.resumed = asyncio.Event()
            self.cancelled = False
            self.answer = None

        async def stream(self, payload, **kwargs):
            yield ng.ProtocolStreamEvent(
                "interrupt", "ok?", ProtocolInterrupt(thread_id=kwargs["thread_id"])
            )

        async def resume_stream(self, answer, **kwargs):
            self.answer = answer
            self.resumed.set()
            try:
                await asyncio.Future()
            except asyncio.CancelledError:
                self.cancelled = True
                raise
            yield None

        def discard_interrupt(self, thread_id):
            self.discarded = thread_id

        def cancel(self, request_id):
            return 0

    async def exercise():
        agent = namespace["NeoGraphACPAgent"]()
        agent.adapter = ResumeBlocks()
        agent.connection = _ACPClient()
        session_id = uuid4().hex
        agent.sessions.add(session_id)
        prompt = asyncio.create_task(agent.prompt(session_id, [text_block("go")]))
        await asyncio.wait_for(agent.adapter.resumed.wait(), timeout=5)
        await agent.cancel(session_id)
        response = await asyncio.wait_for(prompt, timeout=1)
        assert response.stop_reason == "cancelled"
        assert agent.adapter.answer == "approve"
        assert agent.adapter.cancelled is True
        assert agent.adapter.discarded == f"acp:{session_id}"

    asyncio.run(exercise())
