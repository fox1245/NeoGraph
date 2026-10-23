"""Async bridge for hosting NeoGraph agents through Python protocol SDKs.

The official A2A and ACP Python SDKs already own their transports, wire
formats, and protocol lifecycle. ``ProtocolHostAdapter`` connects those
SDK callbacks to NeoGraph without duplicating either protocol in pybind11.

A graph can pause for a human (``interrupt_before``, a ``NodeInterrupt`` or a
tool gate). The adapter reports that pause as a protocol-neutral
``ProtocolInterrupt`` and resumes the checkpoint it paused at when the answer
arrives; rendering the pause is the host's part, because A2A shows it as an
``input-required`` task and ACP as a permission request.
"""

from __future__ import annotations

import asyncio
import hashlib
import weakref
from contextlib import suppress
from dataclasses import dataclass
from typing import Any, AsyncIterator, Callable, Mapping, Optional, Union

from ._neograph import GraphEvent, RunConfig, StreamMode


InputBuilder = Callable[[Any], Mapping[str, Any]]
OutputReader = Callable[[Any], str]
ResumeValueBuilder = Callable[[Any, Optional["ProtocolInterrupt"]], Any]

DEFAULT_INTERRUPT_PROMPT = "The agent needs your input to continue"


def message_input(content: Any, channel: str = "messages") -> dict[str, Any]:
    """Build a chat-channel input from text or JSON-safe content blocks."""
    return {channel: [{"role": "user", "content": content}]}


def last_message_text(result: Any, channel: str = "messages") -> str:
    """Read the last chat message's text from a ``RunResult``."""
    try:
        messages = result.output["channels"][channel]["value"]
        last = messages[-1]
        text = last["content"] if isinstance(last, dict) else last
    except (AttributeError, KeyError, IndexError, TypeError) as exc:
        raise ValueError(
            f"run output has no readable final message in channel {channel!r}; "
            "pass output_reader= for a different graph shape"
        ) from exc
    if not isinstance(text, str):
        raise ValueError(
            f"final message content in channel {channel!r} is not text; "
            "pass output_reader= for a different graph shape"
        )
    return text


@dataclass(frozen=True)
class ProtocolInterrupt:
    """A graph pause that waits for an answer from the protocol's client.

    ``reason`` and ``payload`` come from the run's ``interrupt_value`` the way
    the native A2A and ACP servers read it: ``reason`` is its ``reason`` (or
    ``message``) text and ``payload`` is its ``value``, which is what a
    ``NodeInterrupt`` or ``ToolDecision.interrupt`` carried out of the node.
    ``checkpoint_id`` names the checkpoint the run paused at and
    ``interrupt_id`` the pause that is being asked (empty for an
    ``interrupt_before`` pause); a resume uses both, so a later checkpoint is
    never substituted. A host that restarts between the question and the
    answer can build this object from what it stored and pass it to
    ``resume_stream``/``resume_payload``.
    """

    thread_id: str
    node: str = ""
    reason: str = ""
    payload: Any = None
    checkpoint_id: str = ""
    interrupt_id: str = ""

    @property
    def prompt(self) -> str:
        """The question to show: ``reason``, or a neutral text when it is empty."""
        return self.reason or DEFAULT_INTERRUPT_PROMPT


class ProtocolInterrupted(Exception):
    """A run that the caller expected to finish paused for an answer.

    ``ProtocolHostAdapter.run()``, ``run_payload()``, ``resume_payload()`` and
    ``stream()`` without ``interrupts=True`` raise it instead of returning the
    text of a turn that did not happen. ``interrupt`` says what is being asked;
    the adapter has recorded it, so ``resume_payload()`` can answer it.
    """

    def __init__(self, interrupt: ProtocolInterrupt) -> None:
        where = f" at node {interrupt.node!r}" if interrupt.node else ""
        super().__init__(
            f"the graph paused{where} and waits for an answer: {interrupt.prompt}"
        )
        self.interrupt = interrupt


@dataclass(frozen=True)
class ProtocolStreamEvent:
    """One protocol-facing token, the final response text, or a pause.

    A stream ends with exactly one ``final`` event, or with one ``interrupt``
    event when the graph paused for an answer; ``data`` is then the question
    and ``interrupt`` the pause itself. ``interrupt`` is ``None`` for every
    other kind.
    """

    kind: str
    data: str
    interrupt: Optional[ProtocolInterrupt] = None


def read_interrupt(result: Any, thread_id: str) -> ProtocolInterrupt:
    """Describe the pause of an interrupted ``RunResult``.

    The text is ``reason`` (else ``message``) of ``interrupt_value`` and the
    payload its ``value``, as the native A2A and ACP servers read them.
    """
    value = getattr(result, "interrupt_value", None)
    if isinstance(value, Mapping):
        reason = value.get("reason") or value.get("message") or ""
        payload = value.get("value")
    else:
        reason = ""
        payload = value
    return ProtocolInterrupt(
        thread_id=thread_id,
        node=str(getattr(result, "interrupt_node", "") or ""),
        reason=reason if isinstance(reason, str) else str(reason),
        payload=payload,
        checkpoint_id=str(getattr(result, "checkpoint_id", "") or ""),
        interrupt_id=str(getattr(result, "interrupt_id", "") or ""),
    )


@dataclass(frozen=True)
class _Resume:
    """Marks a turn that answers a pause.

    ``interrupt`` is the one the caller named; without it the pause recorded
    for the thread is used, looked up once the thread's lock is held so that a
    run queued in front of this one has already updated the record.
    """

    interrupt: Optional[ProtocolInterrupt]


def _answer_as_resume_value(answer: Any, interrupt: Optional[ProtocolInterrupt]) -> Any:
    """The default resume value: the protocol's answer as it came."""
    return answer


class ProtocolHostAdapter:
    """Run one NeoGraph engine behind an async agent-protocol server.

    Protocol conversation IDs become NeoGraph ``thread_id`` values and
    checkpoint resume is enabled by default. Active asyncio tasks are tracked
    by request ID so SDK cancellation callbacks can stop the underlying
    ``engine.run_async()`` call instead of merely marking a request cancelled.

    By default the adapter reads and writes a ``messages`` chat channel. Pass
    ``input_builder`` and ``output_reader`` for a different graph state shape.

    A run that pauses for a human (``interrupt_before``, a ``NodeInterrupt`` or
    a tool gate) does not produce a response. ``stream(..., interrupts=True)``
    ends with an ``interrupt`` event that carries a ``ProtocolInterrupt``;
    ``run()``, ``run_payload()`` and a ``stream()`` without ``interrupts``
    raise ``ProtocolInterrupted`` instead. The adapter remembers the pause per
    thread. ``resume_stream()`` and ``resume_payload()`` answer it by resuming
    the checkpoint the run paused at, with the events of the resumed turn
    streamed like those of a run. The answer reaches the graph as
    ``resume_value(answer, interrupt)``, which is the answer itself by
    default; pass ``resume_value`` when the paused node expects another shape.
    """

    def __init__(
        self,
        engine: Any,
        *,
        input_builder: Optional[InputBuilder] = None,
        output_reader: Optional[OutputReader] = None,
        resume_if_exists: bool = True,
        stream_queue_size: int = 1024,
        stream_node: Optional[str] = None,
        resume_value: Optional[ResumeValueBuilder] = None,
    ) -> None:
        if stream_queue_size < 1:
            raise ValueError("stream_queue_size must be at least 1")
        self._engine = engine
        self._input_builder = input_builder or message_input
        self._output_reader = output_reader or last_message_text
        self._resume_if_exists = resume_if_exists
        self._stream_queue_size = stream_queue_size
        self._stream_node = stream_node
        self._resume_value = resume_value or _answer_as_resume_value
        self._active: dict[str, set[asyncio.Task[Any]]] = {}
        self._pending: dict[str, ProtocolInterrupt] = {}
        self._thread_locks: weakref.WeakValueDictionary[str, asyncio.Lock] = (
            weakref.WeakValueDictionary()
        )

    async def run(
        self,
        text: str,
        *,
        thread_id: str,
        request_id: Optional[str] = None,
    ) -> str:
        """Run one protocol request and return response text.

        ``thread_id`` should be the A2A context ID or ACP session ID.
        ``request_id`` should identify the individual task when the protocol
        provides one; otherwise cancellation applies to all work in the thread.
        Raises ``ProtocolInterrupted`` when the graph pauses for an answer.
        """
        return await self.run_payload(
            text, thread_id=thread_id, request_id=request_id
        )

    async def run_payload(
        self,
        payload: Any,
        *,
        thread_id: str,
        request_id: Optional[str] = None,
    ) -> str:
        """Run a text or rich application payload through ``input_builder``."""
        outcome = await self._execute(
            payload, thread_id=thread_id, request_id=request_id
        )
        if isinstance(outcome, ProtocolInterrupt):
            raise ProtocolInterrupted(outcome)
        return outcome

    async def resume_payload(
        self,
        answer: Any,
        *,
        thread_id: str,
        request_id: Optional[str] = None,
        interrupt: Optional[ProtocolInterrupt] = None,
    ) -> str:
        """Answer a paused run and return the response text of the resumed turn.

        ``answer`` is what the protocol's client sent back: the text of an A2A
        follow-up message, the id of the ACP permission option it selected.
        See ``resume_stream`` for ``interrupt``. Raises ``LookupError`` when no
        pause is known for the thread and ``ProtocolInterrupted`` when the
        resumed turn pauses again.
        """
        outcome = await self._execute(
            answer,
            thread_id=thread_id,
            request_id=request_id,
            resume=_Resume(interrupt),
        )
        if isinstance(outcome, ProtocolInterrupt):
            raise ProtocolInterrupted(outcome)
        return outcome

    def stream(
        self,
        payload: Any,
        *,
        thread_id: str,
        request_id: Optional[str] = None,
        interrupts: bool = False,
    ) -> AsyncIterator[ProtocolStreamEvent]:
        """Yield token events followed by one final response event.

        With ``interrupts=True`` a graph that pauses for an answer ends the
        stream with one ``interrupt`` event instead of the final one. Without
        it the pause raises ``ProtocolInterrupted`` after the tokens, so a
        caller that does not handle pauses never takes a pause for an answer.
        """
        return self._events(
            payload,
            thread_id=thread_id,
            request_id=request_id,
            resume=None,
            interrupts=interrupts,
        )

    def resume_stream(
        self,
        answer: Any,
        *,
        thread_id: str,
        request_id: Optional[str] = None,
        interrupt: Optional[ProtocolInterrupt] = None,
    ) -> AsyncIterator[ProtocolStreamEvent]:
        """Answer a paused run and yield the events of the resumed turn.

        The run resumes from the checkpoint it paused at, with
        ``resume_value(answer, interrupt)`` as the value the paused node reads
        from ``input.ctx.resume_value``. ``interrupt`` defaults to the pause the
        adapter recorded for ``thread_id``; pass a ``ProtocolInterrupt`` built
        from stored ids to answer a pause the adapter has not seen, for example
        after a restart. A ``checkpoint_id`` resumes exactly that checkpoint;
        without one the latest checkpoint of the thread is resumed. When
        neither a pause was recorded nor one is passed, ``LookupError`` is
        raised, because resuming a thread that is not paused would return the
        text of its last turn. The stream ends like that of
        ``stream(interrupts=True)``: a ``final`` event, or another
        ``interrupt`` when the graph asks again.
        """
        return self._events(
            answer,
            thread_id=thread_id,
            request_id=request_id,
            resume=_Resume(interrupt),
            interrupts=True,
        )

    async def _events(
        self,
        payload: Any,
        *,
        thread_id: str,
        request_id: Optional[str],
        resume: Optional["_Resume"],
        interrupts: bool,
    ) -> AsyncIterator[ProtocolStreamEvent]:
        queue: asyncio.Queue[str] = asyncio.Queue(
            maxsize=self._stream_queue_size
        )
        done = asyncio.Event()
        token_digest = hashlib.sha256()
        token_bytes = 0
        overflowed = False
        task = None

        def on_event(event: Any) -> None:
            nonlocal overflowed, token_bytes
            if (
                event.type == GraphEvent.Type.LLM_TOKEN
                and self._stream_node is not None
                and event.node_name == self._stream_node
            ):
                token = str(event.data)
                try:
                    queue.put_nowait(token)
                    encoded = token.encode("utf-8")
                    token_digest.update(encoded)
                    token_bytes += len(encoded)
                except asyncio.QueueFull:
                    overflowed = True
                    if task is not None:
                        task.cancel()

        async def produce() -> Union[str, ProtocolInterrupt]:
            try:
                return await self._execute(
                    payload,
                    thread_id=thread_id,
                    request_id=request_id,
                    event_handler=on_event,
                    resume=resume,
                )
            finally:
                done.set()

        task = asyncio.create_task(produce())
        try:
            while True:
                if done.is_set() and queue.empty():
                    break
                token_ready = asyncio.create_task(queue.get())
                run_done = asyncio.create_task(done.wait())
                try:
                    completed, _ = await asyncio.wait(
                        (token_ready, run_done),
                        return_when=asyncio.FIRST_COMPLETED,
                    )
                    if token_ready in completed:
                        yield ProtocolStreamEvent(
                            "token", token_ready.result()
                        )
                finally:
                    for waiter in (token_ready, run_done):
                        if not waiter.done():
                            waiter.cancel()
                            with suppress(asyncio.CancelledError):
                                await waiter
            try:
                outcome = await task
            except asyncio.CancelledError:
                if overflowed:
                    raise RuntimeError(
                        "protocol stream queue overflowed; increase "
                        "stream_queue_size or apply transport backpressure"
                    ) from None
                raise
            if isinstance(outcome, ProtocolInterrupt):
                # The tokens of a paused turn are not a final answer, so there
                # is nothing to compare them with.
                if not interrupts:
                    raise ProtocolInterrupted(outcome)
                yield ProtocolStreamEvent(
                    "interrupt", outcome.prompt, interrupt=outcome
                )
                return
            final_encoded = outcome.encode("utf-8")
            if token_bytes and (
                token_bytes != len(final_encoded)
                or token_digest.digest()
                != hashlib.sha256(final_encoded).digest()
            ):
                raise ValueError(
                    "streamed token text does not match the final response; "
                    "the configured stream_node must emit only the final answer"
                )
            yield ProtocolStreamEvent("final", outcome)
        finally:
            if not task.done():
                task.cancel()
                with suppress(asyncio.CancelledError):
                    await task

    async def _execute(
        self,
        payload: Any,
        *,
        thread_id: str,
        request_id: Optional[str],
        event_handler: Optional[Callable[[Any], None]] = None,
        resume: Optional["_Resume"] = None,
    ) -> Union[str, ProtocolInterrupt]:
        if not thread_id:
            raise ValueError("thread_id must not be empty")
        key = request_id or thread_id
        if not key:
            raise ValueError("request_id must not be empty")

        task = asyncio.current_task()
        if task is None:
            raise RuntimeError("ProtocolHostAdapter.run() needs an asyncio task")
        self._active.setdefault(key, set()).add(task)
        lock = self._thread_locks.get(thread_id)
        if lock is None:
            lock = asyncio.Lock()
            self._thread_locks[thread_id] = lock

        try:
            # NeoGraph permits concurrent runs on different thread IDs, but
            # checkpoint updates for one thread must remain ordered.
            async with lock:
                if resume is not None:
                    interrupt = resume.interrupt or self._pending.get(thread_id)
                    if interrupt is None:
                        raise LookupError(
                            f"no interrupt waits on thread {thread_id!r}; "
                            "pass interrupt= to answer a pause this adapter "
                            "has not recorded"
                        )
                    result = await self._resume(
                        payload, thread_id, event_handler, interrupt
                    )
                else:
                    cfg = RunConfig(
                        thread_id=thread_id,
                        input=dict(self._input_builder(payload)),
                        resume_if_exists=self._resume_if_exists,
                    )
                    if event_handler is None:
                        result = await self._engine.run_async(cfg)
                    else:
                        cfg.stream_mode = StreamMode.TOKENS
                        result = await self._engine.run_stream_async(
                            cfg, event_handler
                        )
                if getattr(result, "interrupted", False):
                    interrupt = read_interrupt(result, thread_id)
                    self._pending[thread_id] = interrupt
                    return interrupt
                self._pending.pop(thread_id, None)
                return self._output_reader(result)
        finally:
            tasks = self._active.get(key)
            if tasks is not None:
                tasks.discard(task)
                if not tasks:
                    self._active.pop(key, None)

    async def _resume(
        self,
        answer: Any,
        thread_id: str,
        event_handler: Optional[Callable[[Any], None]],
        interrupt: ProtocolInterrupt,
    ) -> Any:
        value = self._resume_value(answer, interrupt)
        interrupt_id = interrupt.interrupt_id or None
        if interrupt.checkpoint_id:
            # Exactly the checkpoint the run paused at, never a newer one.
            cfg = RunConfig(thread_id=thread_id)
            if event_handler is not None:
                cfg.stream_mode = StreamMode.TOKENS
            return await self._engine.resume_from_async(
                cfg,
                interrupt.checkpoint_id,
                value,
                interrupt_id=interrupt_id,
                callback=event_handler,
            )
        return await self._engine.resume_async(
            thread_id, value, interrupt_id, callback=event_handler
        )

    def pending_interrupt(self, thread_id: str) -> Optional[ProtocolInterrupt]:
        """Return the pause that waits for an answer on ``thread_id``, if any.

        The record is kept in this process only: it is set when a run pauses
        and dropped when a resumed turn finishes or ``discard_interrupt`` is
        called. A cancelled or failed resume leaves it, so the answer can be
        sent again.
        """
        return self._pending.get(thread_id)

    def discard_interrupt(self, thread_id: str) -> Optional[ProtocolInterrupt]:
        """Forget the pause recorded for ``thread_id`` and return it.

        Call it when the protocol's client withdrew instead of answering (an
        ACP permission request answered ``cancelled``, a cancelled A2A task),
        so a thread nobody will answer keeps no record. The checkpoint stays
        in the engine's store.
        """
        return self._pending.pop(thread_id, None)

    def cancel(self, request_id: str) -> int:
        """Cancel active work for ``request_id`` and return its task count."""
        tasks = tuple(self._active.get(request_id, ()))
        try:
            current = asyncio.current_task()
        except RuntimeError:
            current = None
        for task in tasks:
            if task is not current:
                task.cancel()
        return sum(task is not current for task in tasks)

    def active_count(self, request_id: Optional[str] = None) -> int:
        """Return active request count, mainly for health checks and tests."""
        if request_id is not None:
            return len(self._active.get(request_id, ()))
        return sum(len(tasks) for tasks in self._active.values())

    def has_state(self, thread_id: str) -> bool:
        """Return whether the configured checkpoint store knows this thread."""
        return self._engine.get_state(thread_id) is not None

    def get_state(self, thread_id: str) -> Any:
        """Return the latest checkpoint state for protocol history replay."""
        return self._engine.get_state(thread_id)
