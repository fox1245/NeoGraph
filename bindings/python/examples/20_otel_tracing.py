"""20 — OpenTelemetry tracing via `otel_tracer`.

Bridge engine events (NODE_START / NODE_END / ERROR / INTERRUPT) into
OpenTelemetry spans: one root span per graph run, one child span per
node invocation, with payload metadata as span attributes. Drop-in
for any OTel exporter — Jaeger, Tempo, Honeycomb, Datadog, or just
the console.

This example uses ConsoleSpanExporter so spans print to stdout in JSON
right after each node finishes. Swap the exporter for OTLPSpanExporter
to ship to a real backend.

Run:
    pip install neograph-engine opentelemetry-api opentelemetry-sdk
    python 20_otel_tracing.py
"""

import neograph_engine as ng
from neograph_engine.tracing import otel_tracer

from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import (
    BatchSpanProcessor,
    ConsoleSpanExporter,
)


# Wire up an OTel tracer that prints finished spans to stdout.
provider = TracerProvider()
provider.add_span_processor(BatchSpanProcessor(ConsoleSpanExporter()))
tracer = provider.get_tracer("neograph-example")


# A few nodes so the span tree has multiple children.
class StepA(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute(self, state):
        return [ng.ChannelWrite("trail", ["A"])]


class StepB(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute(self, state):
        return [ng.ChannelWrite("trail", ["B"])]


class StepC(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute(self, state):
        return [ng.ChannelWrite("trail", ["C"])]


for tn, fac in [("step_a", StepA), ("step_b", StepB), ("step_c", StepC)]:
    ng.NodeFactory.register_type(
        tn,
        lambda name, config, ctx, _f=fac: _f(name))

definition = {
    "name": "otel_demo",
    "channels": {"trail": {"reducer": "append"}},
    "nodes": {
        "a": {"type": "step_a"},
        "b": {"type": "step_b"},
        "c": {"type": "step_c"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "a"},
        {"from": "a",           "to": "b"},
        {"from": "b",           "to": "c"},
        {"from": "c",           "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())

cfg = ng.RunConfig(
    thread_id="t1",
    input={},
    max_steps=10,
    stream_mode=ng.StreamMode.EVENTS,
)

print("Running with OTel tracing — spans will print as they finish.\n")
with otel_tracer(tracer) as cb:
    result = engine.run_stream(cfg, cb)

# BatchSpanProcessor flushes asynchronously; force a flush so spans
# show before the script exits.
provider.force_flush()
provider.shutdown()

print("\nFinal trail:", result.output["channels"]["trail"]["value"])
