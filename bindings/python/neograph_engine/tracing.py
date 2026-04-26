"""OpenTelemetry bridge for engine event streams.

NG emits NODE_START / NODE_END / ERROR / INTERRUPT events as it runs;
``otel_tracer`` opens an OTel span per node and a root span per run, so
existing OTel-shaped infra (Jaeger, Tempo, Honeycomb, Datadog, …) can
visualize and search graph executions like any other traced service.

OpenTelemetry is **not** a hard dependency of neograph_engine.  Importing
this module without ``opentelemetry-api`` installed raises a clear
ImportError on first use, not at import time, so unrelated NG users
don't pay the dependency cost.

Typical usage::

    from opentelemetry import trace
    from neograph_engine.tracing import otel_tracer

    tracer = trace.get_tracer("my-service")
    with otel_tracer(tracer) as cb:
        engine.run_stream(cfg, cb)

The context-manager form ensures the root span is always ended even if
``run_stream`` raises, and any node spans still open when the block
exits are forcibly closed.
"""

from __future__ import annotations

from contextlib import contextmanager
from typing import Any, Callable, Iterator, Optional

from . import GraphEvent  # type: ignore[attr-defined]


def _require_otel():
    try:
        from opentelemetry import trace  # noqa: F401
        from opentelemetry.trace import Status, StatusCode  # noqa: F401
    except ImportError as e:
        raise ImportError(
            "opentelemetry-api is required for neograph_engine.tracing. "
            "Install with: pip install opentelemetry-api opentelemetry-sdk"
        ) from e


@contextmanager
def otel_tracer(
    tracer: Any,
    *,
    root_name: str = "graph.run",
    node_span_prefix: str = "node.",
    attribute_prefix: str = "neograph",
    on_event: Optional[Callable[[Any], None]] = None,
) -> Iterator[Callable[[Any], None]]:
    """Yield a graph-event callback that emits OpenTelemetry spans.

    A root span (``root_name``) is opened on context entry and closed on
    exit. Each NODE_START opens a child span named ``node_span_prefix +
    node_name``; the matching NODE_END closes it, applying any payload
    fields (``command_goto``, ``sends``, ``retry_attempt``) as
    ``{attribute_prefix}.{key}`` attributes on the span.

    Concurrent fan-out: multiple invocations of the same node (e.g. via
    Send) are tracked as a per-node stack — each NODE_END pops the most
    recent matching span. ERROR events record the exception on (and end)
    the most recent open span for that node. INTERRUPT events tag the
    open span with ``{attribute_prefix}.interrupted=true`` and end it.

    Args:
        tracer:           an ``opentelemetry.trace.Tracer`` instance.
        root_name:        span name for the per-run root span.
        node_span_prefix: prefix for each node-span name.
        attribute_prefix: prefix for span attributes derived from event data.
        on_event:         optional secondary callback receiving every raw
                          GraphEvent — useful for chaining a tracer with
                          a separate handler (logging, metrics, ...).

    Yields:
        A callable suitable for ``engine.run_stream(cfg, cb)``.
    """
    _require_otel()
    from opentelemetry import trace
    from opentelemetry.trace import Status, StatusCode

    pending: dict[str, list] = {}
    root_span = tracer.start_span(root_name)
    root_token = trace.use_span(root_span, end_on_exit=False).__enter__()

    def _close_all():
        for stack in pending.values():
            while stack:
                span = stack.pop()
                try: span.end()
                except Exception: pass
        pending.clear()

    def cb(ev: Any) -> None:
        if on_event is not None:
            try: on_event(ev)
            except Exception: pass
        t = ev.type
        node = ev.node_name
        try:
            if t == GraphEvent.Type.NODE_START:
                span = tracer.start_span(node_span_prefix + node)
                span.set_attribute(f"{attribute_prefix}.node", node)
                if hasattr(ev.data, "items"):
                    for k, v in ev.data.items():
                        span.set_attribute(f"{attribute_prefix}.{k}", str(v))
                pending.setdefault(node, []).append(span)
            elif t == GraphEvent.Type.NODE_END:
                stack = pending.get(node, [])
                if stack:
                    span = stack.pop()
                    if hasattr(ev.data, "items"):
                        for k, v in ev.data.items():
                            span.set_attribute(f"{attribute_prefix}.{k}", str(v))
                    span.set_status(Status(StatusCode.OK))
                    span.end()
            elif t == GraphEvent.Type.ERROR:
                stack = pending.get(node, [])
                if stack:
                    span = stack.pop()
                    msg = str(ev.data)
                    span.set_attribute(f"{attribute_prefix}.error", msg)
                    span.set_status(Status(StatusCode.ERROR, msg))
                    span.end()
            elif t == GraphEvent.Type.INTERRUPT:
                stack = pending.get(node, [])
                if stack:
                    span = stack.pop()
                    span.set_attribute(f"{attribute_prefix}.interrupted", True)
                    span.end()
        except Exception:
            # Tracing must never break the graph run. Swallow any OTel
            # SDK exception — they're observability, not control flow.
            pass

    try:
        yield cb
    finally:
        _close_all()
        try: root_span.end()
        except Exception: pass


__all__ = ["otel_tracer"]
