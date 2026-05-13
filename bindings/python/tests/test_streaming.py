"""Tests for neograph_engine.streaming helpers."""

from __future__ import annotations

import neograph_engine as ng
from neograph_engine.streaming import message_stream


class _TokenEmitter(ng.GraphNode):
    """Test node that emits a fixed list of LLM tokens through the
    streaming callback. We can't easily fake a Provider here, so we
    drive the streaming path by calling the callback directly inside
    execute_full_stream, which is the framework's preferred dispatch
    point for streaming nodes."""

    def __init__(self, name, tokens):
        super().__init__()
        self._name = name
        self._tokens = tokens

    def get_name(self):
        return self._name

    def run(self, input):
        state = input.state
        cb = input.stream_cb
        for t in self._tokens:
            if cb:
                ev = ng.GraphEvent()
                ev.type = ng.GraphEvent.Type.LLM_TOKEN
                ev.node_name = self._name
                ev.data = t
                cb(ev)
        return ng.NodeResult(writes=[ng.ChannelWrite("done", [1])])


def _build_engine(tokens):
    ng.NodeFactory.register_type(
        "_emitter",
        lambda name, c, ctx: _TokenEmitter(name, tokens))
    defn = {
        "name": "stream_test",
        "channels": {"done": {"reducer": "append"}},
        "nodes":    {"emit":  {"type": "_emitter"}},
        "edges": [
            {"from": ng.START_NODE, "to": "emit"},
            {"from": "emit",        "to": ng.END_NODE},
        ],
    }
    e = ng.GraphEngine.compile(defn, ng.NodeContext())
    e.set_checkpoint_store(ng.InMemoryCheckpointStore())
    return e


def test_message_stream_emits_per_token_chunks():
    tokens = ["Hel", "lo, ", "world", "!"]
    engine = _build_engine(tokens)

    received: list[dict] = []
    cb = message_stream(received.append)

    cfg = ng.RunConfig(
        thread_id="t1",
        input={},
        max_steps=5,
        stream_mode=ng.StreamMode.TOKENS,
    )
    engine.run_stream(cfg, cb)

    assert len(received) == 4
    # Each chunk carries the delta token and full content_so_far.
    assert [m["content"] for m in received] == tokens
    assert [m["content_so_far"] for m in received] == [
        "Hel", "Hello, ", "Hello, world", "Hello, world!",
    ]
    assert all(m["role"] == "assistant" for m in received)
    assert all(m["node"] == "emit" for m in received)
    assert all(m["metadata"] == {"langgraph_node": "emit"} for m in received)


def test_message_stream_no_accumulate_drops_running_total():
    tokens = ["a", "b"]
    engine = _build_engine(tokens)

    received: list[dict] = []
    cb = message_stream(received.append, accumulate=False)

    cfg = ng.RunConfig(
        thread_id="t2",
        input={},
        max_steps=5,
        stream_mode=ng.StreamMode.TOKENS,
    )
    engine.run_stream(cfg, cb)

    assert len(received) == 2
    assert "content_so_far" not in received[0]
    assert "content_so_far" not in received[1]


def test_message_stream_passes_through_other_events():
    tokens = ["x"]
    engine = _build_engine(tokens)

    msgs: list[dict] = []
    events: list = []
    cb = message_stream(msgs.append, on_event=events.append)

    cfg = ng.RunConfig(
        thread_id="t3",
        input={},
        max_steps=5,
        stream_mode=ng.StreamMode.ALL,
    )
    engine.run_stream(cfg, cb)

    # message_stream filters to LLM_TOKEN; events list sees everything.
    assert len(msgs) == 1
    types = {ev.type for ev in events}
    assert ng.GraphEvent.Type.NODE_START in types
    assert ng.GraphEvent.Type.NODE_END   in types
    assert ng.GraphEvent.Type.LLM_TOKEN  in types
