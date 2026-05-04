"""Streaming helpers for engine.run_stream().

Provides:

- ``emit_token(cb, node, data)`` — one-line replacement for the
  4-line ``GraphEvent`` construction ritual a streaming-aware node
  has to perform every time it wants to push an LLM token through
  the graph callback.
- ``message_stream`` — wraps a callback so LLM_TOKEN events are also
  delivered in LangChain's ``stream_mode="messages"`` shape (a dict
  per token chunk with role / content / per-node provenance).
"""

from __future__ import annotations

from typing import Any, Callable, Optional

from . import GraphEvent  # type: ignore[attr-defined]


def emit_token(cb: Callable[[Any], None], node: str, data: Any) -> None:
    """Emit a single LLM_TOKEN event through a graph stream callback.

    Replaces the 4-line construction ritual::

        ev = ng.GraphEvent()
        ev.type = ng.GraphEvent.Type.LLM_TOKEN
        ev.node_name = self._name
        ev.data = token
        cb(ev)

    with::

        from neograph_engine.streaming import emit_token
        emit_token(cb, self._name, token)

    No-ops if ``cb`` is None — the engine passes None when streaming is
    disabled, and node implementations frequently want to forward the
    cb they received without checking.

    Args:
        cb:   the stream callback received by ``execute_stream`` /
              ``execute_full_stream``. Pass through whatever the engine
              handed you. May be ``None``.
        node: the emitting node's name. Conventionally ``self.get_name()``.
        data: the token payload — typically the delta string, but any
              JSON-serializable value works (the engine will round-trip
              it through the C++ ``GraphEvent.data`` field).
    """
    if cb is None:
        return
    ev = GraphEvent()
    ev.type      = GraphEvent.Type.LLM_TOKEN
    ev.node_name = node
    ev.data      = data
    cb(ev)


def message_stream(
    on_message: Optional[Callable[[dict], None]] = None,
    *,
    on_event: Optional[Callable[[Any], None]] = None,
    accumulate: bool = True,
) -> Callable[[Any], None]:
    """Build a graph stream callback that surfaces LLM token chunks as
    LangChain-style message dicts.

    Each message dict has::

        {
            "role":            "assistant",
            "content":         "<delta token>",
            "content_so_far":  "<full content so far>"   # if accumulate=True
            "node":            "<emitting node name>",
            "metadata":        {"langgraph_node": ...},  # mirror LG shape
        }

    The shape mirrors LangGraph's ``stream_mode="messages"`` output —
    a delta token per call, with per-node provenance — so a frontend
    written against either engine can render with the same handler.

    Args:
        on_message: invoked with the message dict for each LLM_TOKEN event.
            If None, message dicts are silently dropped (only useful when
            you also pass ``on_event`` to consume the raw events directly).
        on_event:   if set, also receives every original GraphEvent so you
            can fold non-token events (NODE_END, INTERRUPT, ...) into the
            same callback chain.
        accumulate: include ``content_so_far`` (running per-node concat)
            in each chunk. Set False on long-running graphs to avoid
            holding the full transcript in memory.

    Returns:
        A callable suitable for ``engine.run_stream(cfg, cb)``.
    """
    so_far: dict[str, str] = {}

    def _cb(ev: Any) -> None:
        if on_event is not None:
            on_event(ev)
        if on_message is None:
            return
        if ev.type != GraphEvent.Type.LLM_TOKEN:
            return
        node = ev.node_name
        token = ev.data
        # ev.data is JSON; the C++ side wraps the raw token string but
        # may also pass through structured payloads from custom nodes.
        # We forward whatever's there but only accumulate strings.
        if not isinstance(token, str):
            on_message({
                "role":     "assistant",
                "content":  token,
                "node":     node,
                "metadata": {"langgraph_node": node},
            })
            return
        msg: dict = {
            "role":     "assistant",
            "content":  token,
            "node":     node,
            "metadata": {"langgraph_node": node},
        }
        if accumulate:
            so_far[node] = so_far.get(node, "") + token
            msg["content_so_far"] = so_far[node]
        on_message(msg)

    return _cb


__all__ = ["emit_token", "message_stream"]
