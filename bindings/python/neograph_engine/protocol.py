"""Async bridge for hosting NeoGraph agents through Python protocol SDKs.

The official A2A and ACP Python SDKs already own their transports, wire
formats, and protocol lifecycle. ``ProtocolHostAdapter`` connects those
SDK callbacks to NeoGraph without duplicating either protocol in pybind11.
"""

from __future__ import annotations

import asyncio
import hashlib
import weakref
from contextlib import suppress
from dataclasses import dataclass
from typing import Any, AsyncIterator, Callable, Mapping, Optional

from ._neograph import GraphEvent, RunConfig, StreamMode


InputBuilder = Callable[[Any], Mapping[str, Any]]
OutputReader = Callable[[Any], str]


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
class ProtocolStreamEvent:
    """One protocol-facing token or the final response text."""

    kind: str
    data: str


class ProtocolHostAdapter:
    """Run one NeoGraph engine behind an async agent-protocol server.

    Protocol conversation IDs become NeoGraph ``thread_id`` values and
    checkpoint resume is enabled by default. Active asyncio tasks are tracked
    by request ID so SDK cancellation callbacks can stop the underlying
    ``engine.run_async()`` call instead of merely marking a request cancelled.

    By default the adapter reads and writes a ``messages`` chat channel. Pass
    ``input_builder`` and ``output_reader`` for a different graph state shape.
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
    ) -> None:
        if stream_queue_size < 1:
            raise ValueError("stream_queue_size must be at least 1")
        self._engine = engine
        self._input_builder = input_builder or message_input
        self._output_reader = output_reader or last_message_text
        self._resume_if_exists = resume_if_exists
        self._stream_queue_size = stream_queue_size
        self._stream_node = stream_node
        self._active: dict[str, set[asyncio.Task[Any]]] = {}
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
        return await self._execute(
            payload, thread_id=thread_id, request_id=request_id
        )

    async def stream(
        self,
        payload: Any,
        *,
        thread_id: str,
        request_id: Optional[str] = None,
    ) -> AsyncIterator[ProtocolStreamEvent]:
        """Yield token events followed by one final response event."""
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

        async def produce() -> str:
            try:
                return await self._execute(
                    payload,
                    thread_id=thread_id,
                    request_id=request_id,
                    event_handler=on_event,
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
                final = await task
            except asyncio.CancelledError:
                if overflowed:
                    raise RuntimeError(
                        "protocol stream queue overflowed; increase "
                        "stream_queue_size or apply transport backpressure"
                    ) from None
                raise
            final_encoded = final.encode("utf-8")
            if token_bytes and (
                token_bytes != len(final_encoded)
                or token_digest.digest()
                != hashlib.sha256(final_encoded).digest()
            ):
                raise ValueError(
                    "streamed token text does not match the final response; "
                    "the configured stream_node must emit only the final answer"
                )
            yield ProtocolStreamEvent("final", final)
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
    ) -> str:
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
                return self._output_reader(result)
        finally:
            tasks = self._active.get(key)
            if tasks is not None:
                tasks.discard(task)
                if not tasks:
                    self._active.pop(key, None)

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
