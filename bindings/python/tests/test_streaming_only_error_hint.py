"""v0.3.1: TODO_v0.3.md item #2 — better error message when a node
defines only a streaming variant (execute_stream / execute_full_stream)
and the user calls run() / run_async() (non-streaming) by mistake.

The base `GraphNode.execute()` raises NotImplementedError; the new
message includes a hint pointing to run_stream{,_async}() so the user
doesn't have to grep the source to discover what they got wrong.
"""

import pytest

import neograph_engine as neograph


_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


def _build_engine_with(node_cls, type_name):
    neograph.NodeFactory.register_type(
        type_name,
        lambda name, config, ctx: node_cls(name),
    )
    definition = {
        "name": "stream_only",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"n": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "n"},
            {"from": "n",                 "to": neograph.END_NODE},
        ],
    }
    return neograph.GraphEngine.compile(definition, neograph.NodeContext())


def test_execute_stream_only_node_run_raises_with_hint():
    """Subclass with only execute_stream → run() error message hints at run_stream()."""

    class StreamOnlyNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def execute_stream(self, state, cb):
            return [neograph.ChannelWrite("messages", [{"role": "assistant", "content": "ok"}])]

    engine = _build_engine_with(StreamOnlyNode, _next_type("stream_only_run"))
    cfg = neograph.RunConfig(thread_id="t", input={})

    with pytest.raises(Exception) as excinfo:
        engine.run(cfg)
    msg = str(excinfo.value)
    assert "execute_stream" in msg, f"hint missing: {msg!r}"
    assert "run_stream" in msg, f"hint missing: {msg!r}"


def test_execute_full_stream_only_node_run_raises_with_hint():
    """Same hint surfaces for the execute_full_stream variant."""

    class FullStreamOnlyNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def execute_full_stream(self, state, cb):
            return neograph.NodeResult(writes=[
                neograph.ChannelWrite("messages", [{"role": "assistant", "content": "ok"}])
            ])

    engine = _build_engine_with(FullStreamOnlyNode, _next_type("fstream_only_run"))
    cfg = neograph.RunConfig(thread_id="t", input={})

    with pytest.raises(Exception) as excinfo:
        engine.run(cfg)
    msg = str(excinfo.value)
    assert "execute_full_stream" in msg, f"hint missing: {msg!r}"
    assert "run_stream" in msg, f"hint missing: {msg!r}"


def test_no_hint_when_no_streaming_override():
    """A subclass that overrode nothing must still get the original message
    without the streaming-specific hint — that hint would be misleading."""

    class EmptyNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

    engine = _build_engine_with(EmptyNode, _next_type("empty_run"))
    cfg = neograph.RunConfig(thread_id="t", input={})

    with pytest.raises(Exception) as excinfo:
        engine.run(cfg)
    msg = str(excinfo.value)
    assert "execute()" in msg
    # Critical: NO hint about streaming, because the node didn't define one.
    assert "run_stream()" not in msg
    assert "execute_full_stream" not in msg
    assert "execute_stream" not in msg


def test_streaming_variant_actually_works_via_run_stream():
    """Sanity check: the suggested fix (run_stream) actually dispatches
    to the streaming variant — otherwise the hint would mislead.

    Uses execute_full_stream because that is the entry point the C++
    trampoline (PyGraphNode) dispatches on directly. (execute_stream
    fallback wiring is a separate concern outside item #2.)
    """

    saw_event = []

    class FullStreamOnlyNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def execute_full_stream(self, state, cb):
            saw_event.append("full_stream_called")
            return neograph.NodeResult(writes=[
                neograph.ChannelWrite("messages", [{"role": "assistant", "content": "ok"}])
            ])

    engine = _build_engine_with(FullStreamOnlyNode, _next_type("fstream_run_stream"))
    cfg = neograph.RunConfig(thread_id="t", input={})

    events = []
    engine.run_stream(cfg, lambda ev: events.append(ev.type))
    assert saw_event == ["full_stream_called"]
