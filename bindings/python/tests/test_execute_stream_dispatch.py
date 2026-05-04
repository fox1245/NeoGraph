"""v0.3.2: TODO_v0.3.md item #10 — execute_stream-only nodes dispatch
correctly through engine.run_stream() / run_stream_async().

The v0.3.1 hint added in GraphNode.execute() correctly pointed users
at ``engine.run_stream`` when only a streaming variant was defined,
but for ``execute_stream`` (without ``execute_full_stream``) the
suggested fix didn't actually work — PyGraphNode::execute_full_stream
fell through to execute_full → execute → NotImplementedError. The
fix in this commit makes execute_full_stream consult ``execute_stream``
before that fallback, so a node that only overrides ``execute_stream``
now routes correctly under run_stream.
"""

import neograph_engine as neograph


_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


def _build(node_cls, type_name):
    neograph.NodeFactory.register_type(
        type_name,
        lambda name, c, ctx: node_cls(name),
    )
    definition = {
        "name": "stream_only_dispatch",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"n": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "n"},
            {"from": "n",                 "to": neograph.END_NODE},
        ],
    }
    return neograph.GraphEngine.compile(definition, neograph.NodeContext())


def test_execute_stream_only_runs_under_run_stream():
    """Sole override is execute_stream — run_stream must dispatch to it
    and the user's writes + emitted tokens must both reach the engine."""
    saw_calls = []
    saw_tokens = []

    class StreamOnlyNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def execute_stream(self, state, cb):
            saw_calls.append(self._n)
            # Emit a token so we can verify cb was forwarded.
            ev = neograph.GraphEvent()
            ev.type = neograph.GraphEvent.Type.LLM_TOKEN
            ev.node_name = self._n
            ev.data = "tok"
            cb(ev)
            return [neograph.ChannelWrite(
                "messages", [{"role": "assistant", "content": "ok"}])]

    engine = _build(StreamOnlyNode, _next_type("stream_only_dispatch"))
    cfg = neograph.RunConfig(thread_id="t", input={})

    def on_event(ev):
        if ev.type == neograph.GraphEvent.Type.LLM_TOKEN:
            saw_tokens.append(ev.data)

    result = engine.run_stream(cfg, on_event)

    assert saw_calls == ["n"], f"execute_stream not dispatched, got {saw_calls!r}"
    assert saw_tokens == ["tok"], f"cb forwarding broken, got {saw_tokens!r}"
    msgs = result.output["channels"]["messages"]["value"]
    assert msgs == [{"role": "assistant", "content": "ok"}]


def test_execute_stream_only_still_raises_under_run():
    """run() (non-streaming) on execute_stream-only node must still
    raise — the v0.3.1 hint covers this. The fix only changes the
    run_stream path; run remains a clear error so users don't silently
    drop tokens by picking the wrong entry point."""
    import pytest

    class StreamOnlyNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def execute_stream(self, state, cb):
            return [neograph.ChannelWrite("messages", [])]

    engine = _build(StreamOnlyNode, _next_type("stream_only_run_err"))
    cfg = neograph.RunConfig(thread_id="t", input={})

    with pytest.raises(Exception) as excinfo:
        engine.run(cfg)
    msg = str(excinfo.value)
    assert "run_stream" in msg, f"hint missing: {msg!r}"
    assert "execute_stream" in msg, f"hint missing: {msg!r}"


def test_execute_full_stream_takes_priority_over_execute_stream():
    """If both are defined, execute_full_stream wins (existing semantic).
    The v0.3.2 fix only fills the fallback gap; it doesn't change
    priority."""
    calls = []

    class BothNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def execute_stream(self, state, cb):
            calls.append("stream")
            return []

        def execute_full_stream(self, state, cb):
            calls.append("full_stream")
            return neograph.NodeResult(writes=[
                neograph.ChannelWrite("messages", [])
            ])

    engine = _build(BothNode, _next_type("both_priority"))
    cfg = neograph.RunConfig(thread_id="t", input={})
    engine.run_stream(cfg, lambda ev: None)
    assert calls == ["full_stream"], (
        f"priority broke: {calls!r} — execute_full_stream must win")


def test_execute_stream_with_execute_full_uses_execute_full_for_writes():
    """A node that defines BOTH execute_stream (for tokens) AND
    execute_full (for Command/Send): the engine streaming path picks
    execute_stream — that's the v0.3.2 behaviour, since
    execute_full_stream's first explicit override is execute_stream
    when execute_full_stream is absent. Documented as intentional;
    users wanting Command/Send + tokens override execute_full_stream
    directly."""
    calls = []

    class StreamPlusFull(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def execute_stream(self, state, cb):
            calls.append("stream")
            return [neograph.ChannelWrite("messages", [{"k": "stream"}])]

        def execute_full(self, state):
            calls.append("full")
            return neograph.NodeResult(writes=[
                neograph.ChannelWrite("messages", [{"k": "full"}])
            ])

    engine = _build(StreamPlusFull, _next_type("stream_plus_full"))
    cfg = neograph.RunConfig(thread_id="t", input={})
    result = engine.run_stream(cfg, lambda ev: None)
    assert calls == ["stream"]
    msgs = result.output["channels"]["messages"]["value"]
    assert msgs == [{"k": "stream"}]


def test_execute_stream_returning_none_is_ok():
    """None return from execute_stream is treated as empty writes —
    matches the existing execute_stream behaviour for stream-only
    nodes that exist purely to push events."""
    received = []

    class TokenOnlyNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def execute_stream(self, state, cb):
            ev = neograph.GraphEvent()
            ev.type = neograph.GraphEvent.Type.LLM_TOKEN
            ev.node_name = self._n
            ev.data = "x"
            cb(ev)
            return None

    engine = _build(TokenOnlyNode, _next_type("token_only_none"))
    cfg = neograph.RunConfig(thread_id="t", input={})
    engine.run_stream(cfg, lambda ev:
        received.append(ev.data) if ev.type == neograph.GraphEvent.Type.LLM_TOKEN else None)
    assert received == ["x"]
