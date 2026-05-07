"""Tests for the OpenInference observability layer.

Verify that ``openinference_tracer`` + ``OpenInferenceProvider`` emit
spans with the right OpenInference attribute keys so Phoenix / Arize /
Langfuse render NeoGraph traces as LLM chains.

Strategy: use an InMemorySpanExporter to collect spans, then assert
on attribute presence + values. No external Phoenix instance needed.
"""
from __future__ import annotations

import json
import pytest

import neograph_engine as ng

pytest.importorskip("opentelemetry")

from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import SimpleSpanProcessor
from opentelemetry.sdk.trace.export.in_memory_span_exporter import (
    InMemorySpanExporter,
)

from neograph_engine.openinference import (
    OpenInferenceProvider,
    openinference_tracer,
)


@pytest.fixture
def tracer_and_exporter():
    """Fresh TracerProvider + InMemorySpanExporter per test (avoids
    cross-test span leakage)."""
    exporter = InMemorySpanExporter()
    provider = TracerProvider()
    provider.add_span_processor(SimpleSpanProcessor(exporter))
    tracer = provider.get_tracer("test-openinference")
    yield tracer, exporter
    exporter.clear()


# ── A tiny in-process Provider so tests don't need a network ────────

class _FakeProvider(ng.Provider):
    """Echoes the last user message with a fixed prefix. Captures usage
    so we can verify token-count attribute mapping."""

    def __init__(self, reply: str = "ok", *, prompt_tokens: int = 7,
                 completion_tokens: int = 3):
        super().__init__()
        self._reply = reply
        self._pt = prompt_tokens
        self._ct = completion_tokens

    def get_name(self):
        return "fake"

    def complete(self, params):
        result = ng.ChatCompletion()
        result.message = ng.ChatMessage(role="assistant", content=self._reply)
        usage = ng.ChatCompletion.Usage()
        usage.prompt_tokens = self._pt
        usage.completion_tokens = self._ct
        usage.total_tokens = self._pt + self._ct
        result.usage = usage
        return result


# ── A NeoGraph node that calls the provider once ────────────────────

class _LLMNode(ng.GraphNode):
    def __init__(self, name, ctx):
        super().__init__()
        self._name = name
        self._ctx = ctx

    def get_name(self):
        return self._name

    def run(self, input):
        msgs = input.state.get("messages") or []
        params = ng.CompletionParams()
        params.model = "fake-model-1"
        params.temperature = 0.5
        params.max_tokens = 100
        out = [ng.ChatMessage(role="system", content="be helpful")]
        for m in msgs:
            role = m.get("role", "")
            content = m.get("content", "")
            if role and content:
                out.append(ng.ChatMessage(role=role, content=content))
        params.messages = out
        result = self._ctx.provider.complete(params)
        text = result.message.content if result.message else ""
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": text,
        }])]


def _spans_by_kind(exporter):
    """Return spans grouped by openinference.span.kind attribute."""
    by_kind: dict[str, list] = {}
    for s in exporter.get_finished_spans():
        kind = s.attributes.get("openinference.span.kind", "<none>")
        by_kind.setdefault(kind, []).append(s)
    return by_kind


def test_provider_wrapper_emits_llm_span(tracer_and_exporter):
    """OpenInferenceProvider opens an LLM-kind span on complete()."""
    tracer, exporter = tracer_and_exporter
    inner = _FakeProvider(reply="hello world", prompt_tokens=7,
                          completion_tokens=3)
    wrapped = OpenInferenceProvider(inner, tracer)

    params = ng.CompletionParams()
    params.model = "fake-model-1"
    params.temperature = 0.5
    params.messages = [
        ng.ChatMessage(role="system", content="sys"),
        ng.ChatMessage(role="user", content="hi"),
    ]
    result = wrapped.complete(params)
    assert result.message.content == "hello world"

    spans = exporter.get_finished_spans()
    assert len(spans) == 1
    span = spans[0]
    assert span.name == "llm.complete"
    a = span.attributes
    assert a.get("openinference.span.kind") == "LLM"
    assert a.get("llm.model_name") == "fake-model-1"
    assert a.get("llm.input_messages.0.message.role") == "system"
    assert a.get("llm.input_messages.0.message.content") == "sys"
    assert a.get("llm.input_messages.1.message.role") == "user"
    assert a.get("llm.input_messages.1.message.content") == "hi"
    assert a.get("llm.output_messages.0.message.role") == "assistant"
    assert a.get("llm.output_messages.0.message.content") == "hello world"
    assert a.get("llm.token_count.prompt") == 7
    assert a.get("llm.token_count.completion") == 3
    assert a.get("llm.token_count.total") == 10
    # input.value is JSON, parseable
    parsed = json.loads(a.get("input.value"))
    assert parsed[0]["role"] == "system" and parsed[1]["role"] == "user"


def test_tracer_emits_chain_spans_per_node(tracer_and_exporter):
    """openinference_tracer opens a CHAIN-kind root + per-node child."""
    tracer, exporter = tracer_and_exporter
    inner = _FakeProvider(reply="reply")
    wrapped = OpenInferenceProvider(inner, tracer)
    ctx = ng.NodeContext(provider=wrapped)

    ng.NodeFactory.register_type(
        "llmnode_for_oi_test",
        lambda n, c, ctx: _LLMNode(n, ctx),
    )
    defn = {
        "name": "oi_chain",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"chat": {"type": "llmnode_for_oi_test"}},
        "edges": [
            {"from": ng.START_NODE, "to": "chat"},
            {"from": "chat", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(defn, ctx)

    with openinference_tracer(tracer) as cb:
        engine.run_stream(
            ng.RunConfig(input={"messages": [
                {"role": "user", "content": "hi"}]}),
            cb,
        )

    by_kind = _spans_by_kind(exporter)
    chain_spans = by_kind.get("CHAIN", [])
    llm_spans = by_kind.get("LLM", [])

    # Expect: 1 root (graph.run) + 1 node (node.chat) + 1 LLM (llm.complete).
    chain_names = sorted(s.name for s in chain_spans)
    assert chain_names == ["graph.run", "node.chat"], chain_names
    assert len(llm_spans) == 1, llm_spans
    assert llm_spans[0].name == "llm.complete"


def test_tracer_records_input_output_blob(tracer_and_exporter):
    """Node spans carry input.value / output.value JSON blobs."""
    tracer, exporter = tracer_and_exporter
    inner = _FakeProvider(reply="reply")
    wrapped = OpenInferenceProvider(inner, tracer)
    ctx = ng.NodeContext(provider=wrapped)
    ng.NodeFactory.register_type(
        "llmnode_for_oi_io_test",
        lambda n, c, ctx: _LLMNode(n, ctx),
    )
    defn = {
        "name": "oi_io",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"chat": {"type": "llmnode_for_oi_io_test"}},
        "edges": [
            {"from": ng.START_NODE, "to": "chat"},
            {"from": "chat", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(defn, ctx)
    with openinference_tracer(tracer) as cb:
        engine.run_stream(
            ng.RunConfig(input={"messages": [
                {"role": "user", "content": "hi"}]}),
            cb,
        )

    node_spans = [s for s in exporter.get_finished_spans()
                  if s.name == "node.chat"]
    assert node_spans
    a = node_spans[0].attributes
    assert a.get("input.mime_type") == "application/json"
    parsed = json.loads(a.get("input.value"))
    assert parsed.get("node") == "chat"


def test_provider_wrapper_propagates_exceptions(tracer_and_exporter):
    """Inner provider error → span set to ERROR + exception re-raised."""
    tracer, exporter = tracer_and_exporter

    class _BoomProvider(ng.Provider):
        def get_name(self): return "boom"
        def complete(self, params):
            raise RuntimeError("boom!")

    wrapped = OpenInferenceProvider(_BoomProvider(), tracer)
    params = ng.CompletionParams()
    params.model = "x"
    params.messages = [ng.ChatMessage(role="user", content="hi")]
    with pytest.raises(RuntimeError, match="boom"):
        wrapped.complete(params)

    spans = exporter.get_finished_spans()
    assert len(spans) == 1
    # Status is ERROR after we set it; OTel SDK exposes status_code via
    # status object on the readable span.
    assert spans[0].status.status_code.name == "ERROR"
