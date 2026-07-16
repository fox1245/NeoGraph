"""Async bridge for hosting NeoGraph agents through Python protocol SDKs.

The official A2A and ACP Python SDKs already own their transports, wire
formats, and protocol lifecycle. ``ProtocolHostAdapter`` connects those
SDK callbacks to NeoGraph without duplicating either protocol in pybind11.
"""

from __future__ import annotations

import asyncio
import weakref
from typing import Any, Callable, Mapping, Optional

from ._neograph import RunConfig


InputBuilder = Callable[[str], Mapping[str, Any]]
OutputReader = Callable[[Any], str]


def message_input(text: str, channel: str = "messages") -> dict[str, Any]:
    """Build the usual chat-channel input for one user message."""
    return {channel: [{"role": "user", "content": text}]}


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
    ) -> None:
        self._engine = engine
        self._input_builder = input_builder or message_input
        self._output_reader = output_reader or last_message_text
        self._resume_if_exists = resume_if_exists
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
                    input=dict(self._input_builder(text)),
                    resume_if_exists=self._resume_if_exists,
                )
                result = await self._engine.run_async(cfg)
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
