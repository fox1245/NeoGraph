"""OpenInference semantic-convention layer for OpenTelemetry traces.

Two pieces:

  - :func:`openinference_tracer` — context manager. Same shape as
    :func:`neograph_engine.tracing.otel_tracer`, but tags the root and
    each node span with ``openinference.span.kind = "CHAIN"``. Phoenix /
    Arize / Langfuse use that attribute to render the trace as an LLM
    chain instead of a generic OTel application.

  - :class:`OpenInferenceProvider` — wraps any
    :class:`neograph_engine.Provider`. On every ``complete()`` call it
    opens an LLM-kind child span, captures the prompt messages /
    response / token usage in OpenInference attribute keys, and
    delegates to the inner provider.

Together they make NeoGraph traces show up in Phoenix the same way a
LangGraph + LangSmith trace does — chain of node spans with LLM
sub-spans carrying the conversation, model name, and token counts.

OpenTelemetry is **not** a hard dependency of neograph_engine —
importing this module without ``opentelemetry-api`` installed raises
an ImportError on first use, not at import time. Install with::

    pip install opentelemetry-api opentelemetry-sdk

For a Phoenix end-to-end run::

    docker run -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest
    pip install opentelemetry-exporter-otlp

    from opentelemetry import trace
    from opentelemetry.sdk.trace import TracerProvider
    from opentelemetry.sdk.trace.export import BatchSpanProcessor
    from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import (
        OTLPSpanExporter,
    )
    from neograph_engine.openinference import (
        OpenInferenceProvider, openinference_tracer,
    )

    provider = TracerProvider()
    provider.add_span_processor(
        BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317"))
    )
    trace.set_tracer_provider(provider)
    tracer = trace.get_tracer("my-app")

    inner = OpenAIProvider(api_key=...)
    wrapped = OpenInferenceProvider(inner, tracer)
    ctx = ng.NodeContext(provider=wrapped)
    engine = ng.GraphEngine.compile(graph, ctx)
    with openinference_tracer(tracer) as cb:
        engine.run_stream(cfg, cb)

    # → http://localhost:6006 shows the trace as an LLM chain.
"""

from __future__ import annotations

import json as _json
import logging as _logging
import threading as _threading
from contextlib import contextmanager
from typing import Any, Callable, Iterator, Optional

from . import GraphEvent, Provider  # type: ignore[attr-defined]


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
_OI_LLM_MODEL = "llm.model_name"
_OI_LLM_INVOCATION = "llm.invocation_parameters"  # JSON blob — temperature, max_tokens, etc.
_OI_LLM_TOKEN_PROMPT = "llm.token_count.prompt"
_OI_LLM_TOKEN_COMPLETION = "llm.token_count.completion"
_OI_LLM_TOKEN_TOTAL = "llm.token_count.total"


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

    LLM-shape attributes (model, messages, tokens) are emitted by
    :class:`OpenInferenceProvider` on the child LLM span when the node
    body calls ``provider.complete()``.

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


class OpenInferenceProvider(Provider):
    """Wrap any :class:`Provider` to emit OpenInference LLM spans.

    On every ``complete(params)`` call:

      1. Opens a child span named ``llm.complete`` under the current
         OTel context (so it nests under the active node span if the
         graph is being traced with :func:`openinference_tracer`).
      2. Tags the span with ``openinference.span.kind = "LLM"``.
      3. Captures input messages → ``llm.input_messages.{i}.message.{role,content}``,
         model → ``llm.model_name``, invocation params → ``llm.invocation_parameters``.
      4. Delegates to the inner provider's ``complete``.
      5. Captures output → ``llm.output_messages.0.message.{role,content}``
         and token usage → ``llm.token_count.{prompt,completion,total}``.
      6. Closes the span.

    The wrapper is itself a :class:`Provider`, so it can be passed
    directly to :class:`neograph_engine.NodeContext` in place of the
    inner provider. No graph code change required.

    Tracing failures are caught and swallowed — observability must
    never break the LLM call.
    """

    def __init__(self, inner: Provider, tracer: Any,
                 *, span_name: str = "llm.complete"):
        super().__init__()
        _require_otel()
        self._inner = inner
        self._tracer = tracer
        self._span_name = span_name

    def get_name(self) -> str:
        try:
            return f"openinference({self._inner.get_name()})"
        except Exception:
            return "openinference(provider)"

    def complete(self, params):
        from opentelemetry.trace import Status, StatusCode

        with self._tracer.start_as_current_span(self._span_name) as span:
            try:
                self._record_input(span, params)
            except Exception:
                pass

            try:
                result = self._inner.complete(params)
            except Exception as e:
                try:
                    span.set_status(Status(StatusCode.ERROR, str(e)))
                except Exception:
                    pass
                raise

            try:
                self._record_output(span, result)
                span.set_status(Status(StatusCode.OK))
            except Exception:
                pass
            return result

    def _record_input(self, span, params) -> None:
        span.set_attribute(_OI_SPAN_KIND, "LLM")
        if getattr(params, "model", ""):
            span.set_attribute(_OI_LLM_MODEL, params.model)

        invocation = {}
        for k in ("temperature", "max_tokens", "top_p", "frequency_penalty",
                  "presence_penalty"):
            v = getattr(params, k, None)
            if v is not None:
                invocation[k] = v
        if invocation:
            span.set_attribute(_OI_LLM_INVOCATION, _json.dumps(invocation))

        msgs = list(getattr(params, "messages", []) or [])
        for i, m in enumerate(msgs):
            role = getattr(m, "role", "") or ""
            content = getattr(m, "content", "") or ""
            span.set_attribute(
                f"llm.input_messages.{i}.message.role", role)
            span.set_attribute(
                f"llm.input_messages.{i}.message.content", content)
        if msgs:
            span.set_attribute(
                _OI_INPUT_VALUE,
                _json.dumps(
                    [{"role": getattr(m, "role", ""),
                      "content": getattr(m, "content", "")} for m in msgs],
                    ensure_ascii=False))
            span.set_attribute(_OI_INPUT_MIME, "application/json")

    def _record_output(self, span, result) -> None:
        msg = getattr(result, "message", None)
        if msg is not None:
            role = getattr(msg, "role", "") or ""
            content = getattr(msg, "content", "") or ""
            span.set_attribute(
                "llm.output_messages.0.message.role", role)
            span.set_attribute(
                "llm.output_messages.0.message.content", content)
            span.set_attribute(_OI_OUTPUT_VALUE, content)
            span.set_attribute(_OI_OUTPUT_MIME, "text/plain")

        usage = getattr(result, "usage", None)
        if usage is not None:
            for src, dst in (("prompt_tokens", _OI_LLM_TOKEN_PROMPT),
                             ("completion_tokens", _OI_LLM_TOKEN_COMPLETION),
                             ("total_tokens", _OI_LLM_TOKEN_TOTAL)):
                v = getattr(usage, src, None)
                if v is not None:
                    span.set_attribute(dst, int(v))


__all__ = ["openinference_tracer", "OpenInferenceProvider"]
