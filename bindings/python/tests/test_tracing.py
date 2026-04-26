"""Tests for neograph_engine.tracing.otel_tracer.

Uses the OpenTelemetry SDK's InMemorySpanExporter so we can assert on
the actual span tree the bridge produces — no mocking of OTel internals.
opentelemetry-* is a dev/test dep here; production users opt in by
installing it themselves before importing neograph_engine.tracing.
"""

from __future__ import annotations

import pytest

# All tests skip cleanly if OTel isn't installed — keeps the suite green
# in slim environments and lets us assert the lazy-import error too.
pytest.importorskip("opentelemetry")
pytest.importorskip("opentelemetry.sdk.trace")

from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import (
    SimpleSpanProcessor,
)
from opentelemetry.sdk.trace.export.in_memory_span_exporter import (
    InMemorySpanExporter,
)

import neograph_engine as ng
from neograph_engine.tracing import otel_tracer


@pytest.fixture
def exporter_and_tracer():
    """Fresh InMemorySpanExporter + tracer per test — TracerProviders
    can't be globally swapped after the first set, so we reuse one
    provider but a fresh exporter slot per fixture."""
    exporter = InMemorySpanExporter()
    provider = TracerProvider()
    provider.add_span_processor(SimpleSpanProcessor(exporter))
    tracer = provider.get_tracer("test")
    yield exporter, tracer
    exporter.clear()


class _OkNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute(self, state):
        return [ng.ChannelWrite("done", [1])]


def _build():
    ng.NodeFactory.register_type(
        "_otel_ok_node",
        lambda name, c, ctx: _OkNode(name))
    defn = {
        "name": "otel_test",
        "channels": {"done": {"reducer": "append"}},
        "nodes":    {"work": {"type": "_otel_ok_node"}},
        "edges": [
            {"from": ng.START_NODE, "to": "work"},
            {"from": "work",        "to": ng.END_NODE},
        ],
    }
    e = ng.GraphEngine.compile(defn, ng.NodeContext())
    e.set_checkpoint_store(ng.InMemoryCheckpointStore())
    return e


def test_emits_root_and_node_spans(exporter_and_tracer):
    exporter, tracer = exporter_and_tracer
    engine = _build()
    cfg = ng.RunConfig(thread_id="t1", input={}, max_steps=5,
                       stream_mode=ng.StreamMode.EVENTS)
    with otel_tracer(tracer) as cb:
        engine.run_stream(cfg, cb)
    spans = exporter.get_finished_spans()

    names = {s.name for s in spans}
    assert "graph.run" in names
    assert "node.work" in names

    work = next(s for s in spans if s.name == "node.work")
    assert work.attributes.get("neograph.node") == "work"


def test_passes_event_data_as_span_attributes(exporter_and_tracer):
    exporter, tracer = exporter_and_tracer
    engine = _build()
    cfg = ng.RunConfig(thread_id="t1", input={}, max_steps=5,
                       stream_mode=ng.StreamMode.EVENTS)
    with otel_tracer(tracer) as cb:
        engine.run_stream(cfg, cb)
    spans = exporter.get_finished_spans()

    work = next(s for s in spans if s.name == "node.work")
    # NODE_END payload includes "command_goto" or "sends" if set; for
    # this graph neither is present — just confirm the attribute helper
    # handled an empty / scalar payload without crashing.
    assert work.status.status_code.name == "OK"


def test_lazy_import_error_message(monkeypatch):
    """If opentelemetry isn't installed, otel_tracer should raise a
    clear ImportError (not a generic AttributeError or NameError)."""
    import sys
    saved_otel = sys.modules.get("opentelemetry")
    sys.modules["opentelemetry"] = None  # type: ignore[assignment]
    try:
        # Re-import tracing so _require_otel re-evaluates with broken
        # import; we use a direct call to the helper since the cm needs
        # a tracer arg before _require_otel runs.
        import importlib
        import neograph_engine.tracing as tracing_mod
        importlib.reload(tracing_mod)
        with pytest.raises(ImportError, match="opentelemetry-api"):
            with tracing_mod.otel_tracer(None):
                pass
    finally:
        if saved_otel is not None:
            sys.modules["opentelemetry"] = saved_otel
        else:
            sys.modules.pop("opentelemetry", None)
        import importlib
        import neograph_engine.tracing as tracing_mod
        importlib.reload(tracing_mod)
