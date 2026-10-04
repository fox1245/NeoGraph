"""OpenInference spans follow real graph and typed TLS provider dispatches."""

import asyncio
import json
import gc

import pytest

pytest.importorskip("opentelemetry.sdk")
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import SimpleSpanProcessor
from opentelemetry.sdk.trace.export.in_memory_span_exporter import InMemorySpanExporter

import neograph_engine as ng
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer


@pytest.fixture
def tracer_and_exporter():
    exporter = InMemorySpanExporter()
    provider = TracerProvider()
    provider.add_span_processor(SimpleSpanProcessor(exporter))
    try:
        yield provider.get_tracer("neograph-openinference-test"), exporter
    finally:
        provider.shutdown()


def _engine(fail=False):
    class Calculate(ng.GraphNode):
        def get_name(self):
            return "calculate"

        def run(self, input):
            if fail:
                raise ValueError("calculation rejected")
            return [ng.ChannelWrite("answer", input.state.get("value") * 2)]

    node_type = "oi_failure" if fail else "oi_calculate"
    ng.NodeFactory.register_type(node_type, lambda _n, _c, _ctx: Calculate())
    return ng.GraphEngine.compile({
        "name": "oi-graph", "schema_version": 1,
        "channels": {"value": {"reducer": "overwrite"},
                     "answer": {"reducer": "overwrite"}},
        "nodes": {"calculate": {"type": node_type}},
        "edges": [{"from": ng.START_NODE, "to": "calculate"},
                  {"from": "calculate", "to": ng.END_NODE}],
    }, ng.NodeContext())


@pytest.mark.parametrize("asynchronous", [False, True])
def test_graph_spans_end_with_parent_and_output(tracer_and_exporter, asynchronous):
    tracer, exporter = tracer_and_exporter
    engine = _engine()
    cfg = ng.RunConfig(thread_id=f"oi-{asynchronous}", input={"value": 21},
                       stream_mode=ng.StreamMode.ALL)
    with openinference_tracer(tracer) as callback:
        if asynchronous:
            async def execute():
                return await engine.run_stream_async(cfg, callback)
            result = asyncio.run(execute())
        else:
            result = engine.run_stream(cfg, callback)
    assert result.output["channels"]["answer"]["value"] == 42
    spans = {span.name: span for span in exporter.get_finished_spans()}
    root = spans["graph.run"]
    node = spans["node.calculate"]
    assert node.parent.span_id == root.context.span_id
    assert node.attributes["openinference.span.kind"] == "CHAIN"
    assert node.status.status_code.name == "OK"
    assert json.loads(node.attributes["input.value"])["node"] == "calculate"
    assert node.end_time <= root.end_time


def test_error_closes_pending_node_span(tracer_and_exporter):
    tracer, exporter = tracer_and_exporter
    with openinference_tracer(tracer) as callback:
        with pytest.raises((RuntimeError, ValueError)):
            _engine(fail=True).run_stream(
                ng.RunConfig(thread_id="oi-failure", input={"value": 21}), callback)
    spans = {span.name: span for span in exporter.get_finished_spans()}
    assert spans["node.calculate"].status.status_code.name == "ERROR"
    assert spans["node.calculate"].end_time <= spans["graph.run"].end_time


@pytest.mark.parametrize("stream", [False, True])
@pytest.mark.parametrize("usage", [None, {"prompt_tokens": 0, "completion_tokens": 0,
                                       "total_tokens": 0}])
def test_provider_spans_preserve_missing_usage_and_reported_zero(
        provider_peer, tracer_and_exporter, stream, usage):
    tracer, exporter = tracer_and_exporter
    provider_peer.usage = usage
    wrapped = OpenInferenceProvider(provider_peer.provider(), tracer)
    request = ng.make_provider_request(wrapped, "local-model", [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("trace this request")]),
    ], mode=ng.ProviderMode.Stream if stream else ng.ProviderMode.Collect)
    events = []
    request.on_event = events.append
    outcome = wrapped.invoke(request)
    assert outcome.text == "ok"
    span, = exporter.get_finished_spans()
    assert span.attributes["openinference.span.kind"] == "LLM"
    assert span.attributes["llm.model_name"] == "local-model"
    assert span.attributes["llm.input_messages.0.message.content"] == "trace this request"
    assert span.attributes["llm.output_messages.0.message.content"] == "ok"
    keys = ("llm.token_count.prompt", "llm.token_count.completion", "llm.token_count.total")
    if usage is None:
        assert all(key not in span.attributes for key in keys)
    else:
        assert [span.attributes[key] for key in keys] == [0, 0, 0]
    assert span.status.status_code.name == "OK"
    if stream:
        assert [event.value.bytes for event in events if event.kind == "PartDelta"] == ["ok"]
        assert [event.name for event in span.events] == ["llm.token"]


def test_abandoned_preparation_does_not_open_span_and_dispatched_preparation_retains_tracer(
        provider_peer):
    exporter = InMemorySpanExporter()
    backend = TracerProvider()
    backend.add_span_processor(SimpleSpanProcessor(exporter))
    try:
        tracer = backend.get_tracer("prepared-lifetime")
        inner = provider_peer.provider()
        wrapped = OpenInferenceProvider(inner, tracer)
        request = ng.make_provider_request(wrapped, "local-model", [
            ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("retained preparation")]),
        ])
        abandoned = wrapped.prepare(request)
        del abandoned
        gc.collect()
        assert exporter.get_finished_spans() == ()
        assert provider_peer.requests == []
        prepared = wrapped.prepare(request)
        del wrapped, request, tracer
        gc.collect()
        # The native prepared handle, not wrapper/tracer locals, owns hooks.
        outcome = inner.dispatch(prepared)
        assert outcome.text == "ok"
        span, = exporter.get_finished_spans()
        assert span.attributes["llm.output_messages.0.message.content"] == "ok"
        assert span.status.status_code.name == "OK"
    finally:
        backend.shutdown()


def test_provider_failure_is_traced_without_becoming_success(provider_peer, tracer_and_exporter):
    tracer, exporter = tracer_and_exporter
    provider_peer.status = 429
    wrapped = OpenInferenceProvider(provider_peer.provider(), tracer)
    request = ng.make_provider_request(wrapped, "local-model", [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("failure request")]),
    ])
    outcome = wrapped.invoke(request)
    assert outcome.completion is None
    assert outcome.failure.error.http_status == 429
    span, = exporter.get_finished_spans()
    assert span.status.status_code.name == "ERROR"


def test_native_provider_traces_tls_dispatch_from_node_context(provider_peer, tracer_and_exporter):
    tracer, exporter = tracer_and_exporter
    from opentelemetry import context as otel_context

    with tracer.start_as_current_span("application-root"):
        parent_context = otel_context.get_current()

        class ParentContextTracer:
            def start_span(self, name):
                return tracer.start_span(name, context=parent_context)

        wrapped = OpenInferenceProvider(provider_peer.provider(), ParentContextTracer())
        engine = ng.GraphEngine.compile({
            "name": "oi-provider-graph", "schema_version": 1,
            "channels": {"messages": {"reducer": "append"}},
            "nodes": {"chat": {"type": "llm_call"}},
            "edges": [{"from": ng.START_NODE, "to": "chat"},
                      {"from": "chat", "to": ng.END_NODE}],
        }, ng.NodeContext(provider=wrapped, model="local-model"))
        del wrapped
        gc.collect()
        with openinference_tracer(tracer) as callback:
            result = engine.run_stream(ng.RunConfig(
                thread_id="oi-provider-graph",
                input={"messages": [{"role": "user", "content": "graph provider request"}]},
            ), callback)
    assert result.output["channels"]["messages"]["value"][-1]["content"] == "ok"
    assert result.provider_outcomes[0].text == "ok"
    assert result.native_messages[-1].native is not None
    spans = {span.name: span for span in exporter.get_finished_spans()}
    llm = spans["llm.complete"]
    assert llm.attributes["openinference.span.kind"] == "LLM"
    assert llm.attributes["llm.token_count.total"] == 15
    assert llm.parent.span_id == spans["application-root"].context.span_id
    assert llm.status.status_code.name == "OK"


def test_tracing_preserves_raw_evidence_without_exporting_it(provider_peer, tracer_and_exporter):
    tracer, exporter = tracer_and_exporter
    private_note = "fixture-private-wire-note"
    provider_peer.response_extra = {"private_wire_note": private_note}
    wrapped = OpenInferenceProvider(provider_peer.provider(), tracer)
    request = ng.make_provider_request(wrapped, "local-model", [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("public input")]),
    ])
    outcome = wrapped.invoke(request)
    raw = next(event for event in outcome.completion.raw_events
               if event.type == "chat.completion")
    assert raw.payload["private_wire_note"] == private_note
    assert outcome.messages[0].native is not None
    span, = exporter.get_finished_spans()
    assert private_note not in json.dumps(dict(span.attributes))


def test_failed_tracer_does_not_replace_real_provider_outcome(provider_peer):
    class FailedTracer:
        def start_span(self, _name):
            raise RuntimeError("trace backend refused span")

    wrapped = OpenInferenceProvider(provider_peer.provider(), FailedTracer())
    request = ng.make_provider_request(wrapped, "local-model", [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("provider must still run")]),
    ])
    outcome = wrapped.invoke(request)
    assert outcome.failure is None
    assert outcome.text == "ok"
    assert outcome.usage.total.value == 15


def test_traced_observer_error_keeps_original_cause_and_owned_outcome(
        provider_peer, tracer_and_exporter):
    tracer, exporter = tracer_and_exporter
    refusal = ValueError("observer refused event")
    wrapped = OpenInferenceProvider(provider_peer.provider(), tracer)
    request = ng.make_provider_request(wrapped, "local-model", [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("observer failure")]),
    ], mode=ng.ProviderMode.Stream)

    def reject(_event):
        raise refusal

    request.on_event = reject
    with pytest.raises(ng.ProviderObserverError) as caught:
        wrapped.invoke(request)
    assert caught.value.cause is refusal
    assert caught.value.outcome is not None
    span, = exporter.get_finished_spans()
    assert span.status.status_code.name == "ERROR"
