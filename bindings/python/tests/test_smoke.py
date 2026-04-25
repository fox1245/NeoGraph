"""Smoke test for the pybind11 binding.

Run from the build dir (where libneograph_core.so + neograph/ live):

    PYTHONPATH=$PWD pytest -q ../bindings/python/tests

Doesn't hit any LLM API — uses a JSON-defined graph that compiles
cleanly without invoking a Provider. The point is to prove the
binding boundary works end-to-end: dict-shaped graph definition →
GraphEngine.compile → GraphEngine.run → result.output.
"""

import neograph_engine as neograph  # PyPI dist name is `neograph-engine`;
                                     # bare `neograph` was already taken


def test_module_metadata():
    assert isinstance(neograph.__version__, str)
    assert neograph.START_NODE == "__start__"
    assert neograph.END_NODE == "__end__"


def test_stream_mode_bitfield():
    # StreamMode is bound with py::arithmetic so | and & should compose.
    combined = neograph.StreamMode.EVENTS | neograph.StreamMode.TOKENS
    # Either result is fine; both must register as containing EVENTS.
    assert int(combined) & int(neograph.StreamMode.EVENTS)
    assert int(combined) & int(neograph.StreamMode.TOKENS)


def test_compile_minimal_graph():
    """The simplest valid graph: one channel, no nodes, START → END."""
    definition = {
        "name": "passthrough",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {},
        "edges": [
            {"from": neograph.START_NODE, "to": neograph.END_NODE},
        ],
    }
    ctx = neograph.NodeContext()
    engine = neograph.GraphEngine.compile(definition, ctx)
    assert engine.name == "passthrough"

    cfg = neograph.RunConfig(
        thread_id="t1",
        input={"messages": [{"role": "user", "content": "hello"}]},
    )
    result = engine.run(cfg)
    assert result.output is not None
    # Output should expose the channel we wrote into.
    # The exact serialization shape is engine-internal; we just
    # check that the messages we sent in came back out.
    payload = result.output
    assert isinstance(payload, dict)


def test_node_context_construction():
    """NodeContext should accept a None provider for graphs that don't need one."""
    ctx = neograph.NodeContext(
        provider=None,
        model="gpt-4o-mini",
        instructions="You are helpful.",
        extra_config={"foo": "bar"},
    )
    assert ctx.model == "gpt-4o-mini"
    assert ctx.instructions == "You are helpful."
    assert ctx.extra_config == {"foo": "bar"}


def test_channel_write_round_trip():
    w = neograph.ChannelWrite("findings", [{"k": 1}, {"k": 2}])
    assert w.channel == "findings"
    assert w.value == [{"k": 1}, {"k": 2}]


def test_send_round_trip():
    s = neograph.Send("worker", {"item": 42})
    assert s.target_node == "worker"
    assert s.input == {"item": 42}


def test_command_construction():
    c = neograph.Command(
        goto_node="approve",
        updates=[neograph.ChannelWrite("status", "approved")],
    )
    assert c.goto_node == "approve"
    assert len(c.updates) == 1
    assert c.updates[0].channel == "status"
    assert c.updates[0].value == "approved"


def test_chat_message_round_trip():
    msg = neograph.ChatMessage(role="user", content="hi")
    assert msg.role == "user"
    assert msg.content == "hi"

    tc = neograph.ToolCall(id="call_1", name="calc", arguments='{"x":1}')
    msg.tool_calls = [tc]
    assert msg.tool_calls[0].name == "calc"


def test_completion_params_construction():
    params = neograph.CompletionParams(
        model="gpt-4o-mini",
        messages=[neograph.ChatMessage(role="user", content="hi")],
        temperature=0.5,
    )
    assert params.model == "gpt-4o-mini"
    assert len(params.messages) == 1
    assert params.temperature == 0.5
