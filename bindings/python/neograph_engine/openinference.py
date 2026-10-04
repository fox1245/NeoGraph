"""OpenInference graph and provider spans backed by OpenTelemetry.

``openinference_tracer`` connects an OpenTelemetry tracer to graph streaming
events. ``OpenInferenceProvider`` uses the native typed provider wrapper to
trace each prepared dispatch without changing its owned outcome or events.
Only public role/text projections, declared controls and known usage counters
enter LLM spans; native replay, reasoning and raw envelopes remain private.

OpenTelemetry remains optional; install ``opentelemetry-api`` and
``opentelemetry-sdk`` to use these integrations.
"""

from __future__ import annotations

import json as _json
import logging as _logging
import threading as _threading
from contextlib import contextmanager
from typing import Any, Callable, Iterator, Optional

from . import GraphEvent  # type: ignore[attr-defined]
from ._neograph import OpenInferenceProvider as _NativeOpenInferenceProvider


def _ctx_id() -> tuple:
    # OTel context tokens are bound to the contextvars Context they were
    # created in. With async callers + StreamMode.ALL the engine fires
    # NODE_END from a different asyncio.Task than the one that ran
    # NODE_START, so the detach lands in a foreign Context. Match
    # attach and detach by (thread, task) and skip detach on mismatch
    # — the original task's contextvar dies with the task anyway.
    try:
        import asyncio
        try:
            task = asyncio.current_task()
        except RuntimeError:
            task = None
    except Exception:
        task = None
    return (_threading.get_ident(), id(task) if task is not None else None)


# Silences the "Failed to detach context" stderr noise that OTel's SDK
# emits when our cross-Context detach fails. Active only during our own
# `_safe_detach` window (per-thread flag) so other code calling OTel
# detach is unaffected. See issue #2.
_silencing = _threading.local()


class _DetachContextLogFilter(_logging.Filter):
    _MSG = "Failed to detach context"

    def filter(self, record: _logging.LogRecord) -> bool:
        if getattr(_silencing, "active", False) and self._MSG in record.getMessage():
            return False
        return True


def _install_detach_silencer_once() -> None:
    logger = _logging.getLogger("opentelemetry.context")
    for f in logger.filters:
        if isinstance(f, _DetachContextLogFilter):
            return
    logger.addFilter(_DetachContextLogFilter())


# OpenInference attribute keys. Hard-coded as strings rather than
# imported from ``openinference-semantic-conventions`` so this module
# has zero extra runtime dep — Phoenix / Langfuse only require the keys
# match the spec, not the import path.
_OI_SPAN_KIND = "openinference.span.kind"
_OI_INPUT_VALUE = "input.value"
_OI_INPUT_MIME = "input.mime_type"
_OI_OUTPUT_VALUE = "output.value"
_OI_OUTPUT_MIME = "output.mime_type"


def _require_otel():
    try:
        from opentelemetry import trace  # noqa: F401
        from opentelemetry.trace import Status, StatusCode  # noqa: F401
    except ImportError as e:
        raise ImportError(
            "opentelemetry-api is required for "
            "neograph_engine.openinference. Install with: "
            "pip install opentelemetry-api opentelemetry-sdk"
        ) from e


@contextmanager
def openinference_tracer(
    tracer: Any,
    *,
    root_name: str = "graph.run",
    node_span_prefix: str = "node.",
    on_event: Optional[Callable[[Any], None]] = None,
) -> Iterator[Callable[[Any], None]]:
    """Yield a graph-event callback that emits OpenInference-shape spans.

    Identical shape to :func:`neograph_engine.tracing.otel_tracer` — the
    only differences:

      - The root span and each node span carry
        ``openinference.span.kind = "CHAIN"`` so Phoenix / Langfuse
        recognise them as a chain of operations rather than generic
        spans.
      - Node-level event payloads are stuffed into ``input.value`` (on
        NODE_START) and ``output.value`` (on NODE_END) so the trace
        viewer can show node-shape data in the rendering panes.

    Provider-level observations use ``ProviderRequest.on_event``. This graph
    tracer does not export native replay, raw envelopes, or token usage.

    Args:
        tracer:           an ``opentelemetry.trace.Tracer`` instance.
        root_name:        span name for the per-run root span.
        node_span_prefix: prefix for each node-span name.
        on_event:         optional secondary callback receiving every
                          raw GraphEvent.

    Yields:
        A callable suitable for ``engine.run_stream(cfg, cb)``.
    """
    _require_otel()
    from opentelemetry import context as otel_context
    from opentelemetry.trace import Status, StatusCode, set_span_in_context

    _install_detach_silencer_once()

    # Each node-span entry is (span, contextvar_token, attach_ctx_id)
    # so NODE_END can detach the span from the OTel current-context
    # that NODE_START attached, restoring the prior current span (root
    # or an outer node in nested-fanout cases). The ctx_id is checked
    # at detach time — if NODE_END fires in a different asyncio.Task
    # than NODE_START we skip detach to avoid the OTel SDK's noisy
    # "Failed to detach context" stderr (issue #2). Same-task callers
    # (sync, or async without task switching) still get proper LLM-span
    # nesting under the node span.
    pending: dict[str, list] = {}
    root_span = tracer.start_span(root_name)
    root_span.set_attribute(_OI_SPAN_KIND, "CHAIN")
    parent_ctx = set_span_in_context(root_span)
    # Attach the root span as the OTel current span so any LLM-span
    # opens under it BEFORE the first node fires (e.g. graph engine
    # internals or pre-node hooks). NODE_START will replace this with
    # its own attach; NODE_END restores it.
    root_token = otel_context.attach(parent_ctx)
    root_attach_id = _ctx_id()

    def _safe_detach(token, attach_id):
        if token is None:
            return
        if _ctx_id() != attach_id:
            # Cross-task detach — would land in a foreign Context.
            # Skip; the source contextvar dies with its task.
            return
        # Same (thread, task) as attach, but the contextvars Context
        # snapshot may still differ if Python control switched away
        # and back via an unrelated awaitable. Filter the OTel logger
        # for the duration of this call so any "Failed to detach
        # context" record is dropped (semantics already a no-op).
        _silencing.active = True
        try:
            try:
                otel_context.detach(token)
            except Exception:
                pass
        finally:
            _silencing.active = False

    def _unpack(entry):
        # Tolerate older 2-tuple entries that may sneak in via
        # subclasses / monkey-patches.
        if isinstance(entry, tuple):
            if len(entry) == 3:
                return entry
            if len(entry) == 2:
                return entry[0], entry[1], None
        return entry, None, None

    def _close_all():
        for stack in pending.values():
            while stack:
                span, token, attach_id = _unpack(stack.pop())
                _safe_detach(token, attach_id)
                try:
                    span.end()
                except Exception:
                    pass
        pending.clear()

    def _node_input_blob(ev: Any) -> str:
        try:
            data = dict(ev.data) if hasattr(ev.data, "items") else {}
            return _json.dumps({"node": ev.node_name, **data},
                               default=str, ensure_ascii=False)
        except Exception:
            return str(ev.node_name)

    def cb(ev: Any) -> None:
        if on_event is not None:
            try:
                on_event(ev)
            except Exception:
                pass
        t = ev.type
        node = ev.node_name
        try:
            if t == GraphEvent.Type.NODE_START:
                span = tracer.start_span(
                    node_span_prefix + node, context=parent_ctx)
                span.set_attribute(_OI_SPAN_KIND, "CHAIN")
                span.set_attribute("neograph.node", node)
                if hasattr(ev.data, "items"):
                    for k, v in ev.data.items():
                        span.set_attribute(f"neograph.{k}", str(v))
                span.set_attribute(_OI_INPUT_VALUE, _node_input_blob(ev))
                span.set_attribute(_OI_INPUT_MIME, "application/json")
                # Attach as OTel current span so LLM/Tool spans created
                # inside the node body nest as children. Token + the
                # (thread, task) where attach happened are kept so
                # NODE_END can detach only when in the same Context.
                token = otel_context.attach(set_span_in_context(span))
                pending.setdefault(node, []).append(
                    (span, token, _ctx_id()))
            elif t == GraphEvent.Type.NODE_END:
                stack = pending.get(node, [])
                if stack:
                    span, token, attach_id = _unpack(stack.pop())
                    if hasattr(ev.data, "items"):
                        for k, v in ev.data.items():
                            span.set_attribute(f"neograph.{k}", str(v))
                    try:
                        span.set_attribute(
                            _OI_OUTPUT_VALUE,
                            _json.dumps(
                                dict(ev.data) if hasattr(ev.data, "items")
                                else {"node": node},
                                default=str, ensure_ascii=False))
                        span.set_attribute(_OI_OUTPUT_MIME, "application/json")
                    except Exception:
                        pass
                    span.set_status(Status(StatusCode.OK))
                    _safe_detach(token, attach_id)
                    span.end()
            elif t == GraphEvent.Type.ERROR:
                stack = pending.get(node, [])
                if stack:
                    span, token, attach_id = _unpack(stack.pop())
                    msg = str(ev.data)
                    span.set_attribute("neograph.error", msg)
                    span.set_status(Status(StatusCode.ERROR, msg))
                    _safe_detach(token, attach_id)
                    span.end()
            elif t == GraphEvent.Type.INTERRUPT:
                stack = pending.get(node, [])
                if stack:
                    span, token, attach_id = _unpack(stack.pop())
                    span.set_attribute("neograph.interrupted", True)
                    _safe_detach(token, attach_id)
                    span.end()
        except Exception:
            # Tracing must never break the graph run.
            pass

    try:
        yield cb
    finally:
        _close_all()
        _safe_detach(root_token, root_attach_id)
        try:
            root_span.end()
        except Exception:
            pass


class OpenInferenceProvider(_NativeOpenInferenceProvider):
    """Trace real typed provider dispatches using a Python OpenTelemetry tracer.

    Preparing without dispatching creates no LLM span. The native wrapper
    retains the tracer through prepared-request lifetime and records missing
    usage as absent attributes, not zero counts. Tracer failures do not replace
    provider results or exceptions.
    """

    def __init__(self, inner, tracer, *, span_name="llm.complete"):
        _require_otel()
        super().__init__(inner, tracer, span_name=span_name)


__all__ = ["openinference_tracer", "OpenInferenceProvider"]
