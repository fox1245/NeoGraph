"""v0.3.1: TODO_v0.3.md item #3 — emit_token helper.

Streaming-aware Python nodes used to need a 4-line GraphEvent
construction ritual to push tokens. The new
``neograph_engine.streaming.emit_token(cb, node, data)`` helper
collapses that to a single call and is exported from the streaming
module's __all__.
"""

import pytest

import neograph_engine as neograph
from neograph_engine.streaming import emit_token


def test_emit_token_pushes_llm_token_event():
    """The helper builds an LLM_TOKEN event and passes it to the callback."""
    captured = []
    emit_token(captured.append, "n1", "hello")
    assert len(captured) == 1
    ev = captured[0]
    assert ev.type      == neograph.GraphEvent.Type.LLM_TOKEN
    assert ev.node_name == "n1"
    assert ev.data      == "hello"


def test_emit_token_accepts_structured_data():
    """``data`` round-trips through JSON for non-string payloads."""
    captured = []
    emit_token(captured.append, "n1", {"choice": 0, "delta": "x"})
    assert captured[0].data == {"choice": 0, "delta": "x"}


def test_emit_token_with_none_cb_is_noop():
    """Engine passes None when streaming is disabled — must not raise."""
    emit_token(None, "n1", "anything")  # no exception


def test_emit_token_runs_inside_execute_full_stream():
    """End-to-end: a node uses the helper to forward tokens; the engine's
    run_stream callback observes them in the events list."""

    class StreamerNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def run(self, input):
            state = input.state
            cb = input.stream_cb
            for chunk in ("hel", "lo"):
                if cb:
                    emit_token(cb, self._n, chunk)
            return neograph.NodeResult(writes=[
                neograph.ChannelWrite("messages", [{"role": "assistant", "content": "hello"}])
            ])

    neograph.NodeFactory.register_type(
        "streamer_emit_token",
        lambda name, config, ctx: StreamerNode(name),
    )

    definition = {
        "name": "streamer_graph",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"s": {"type": "streamer_emit_token"}},
        "edges": [
            {"from": neograph.START_NODE, "to": "s"},
            {"from": "s",                 "to": neograph.END_NODE},
        ],
    }

    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    cfg = neograph.RunConfig(thread_id="t", input={})

    tokens = []

    def on_event(ev):
        if ev.type == neograph.GraphEvent.Type.LLM_TOKEN:
            tokens.append(ev.data)

    engine.run_stream(cfg, on_event)
    assert tokens == ["hel", "lo"]


def test_emit_token_in_streaming_module_all():
    """emit_token must be in the public API surface."""
    from neograph_engine import streaming
    assert "emit_token" in streaming.__all__
