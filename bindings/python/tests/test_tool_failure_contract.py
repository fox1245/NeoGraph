"""Python tools follow the same failure contract as C++ tools.

Run control signals stop the run and never become a tool message: a
``NodeInterrupt`` raised by a tool pauses the run, a ``HookBoundaryBlocked``
raised by a tool (for example from a nested engine whose mandatory Hook
blocked) fails it with its invocation id and state, and cancelling the run's
token cancels it. Every other exception becomes a tool result with a typed
status that the model reads. Each case runs for one call and for a batch, and
for both ``Tool`` (serial) and ``AsyncTool`` (offloaded) subclasses.
"""

import json

import neograph_engine as ng
import pytest


_uid = 0


def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


class _EmitCalls(ng.GraphNode):
    def __init__(self, name, calls):
        super().__init__()
        self._name = name
        self._calls = calls

    def get_name(self):
        return self._name

    def run(self, _input):
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": "",
            "tool_calls": self._calls,
        }])]


def _tool(base, name, body):
    class _Probe(base):
        def get_name(self):
            return name

        def get_definition(self):
            return ng.ChatTool(name=name, description=name,
                               parameters={"type": "object", "properties": {}})

        def execute(self, arguments):
            return body(arguments)

    return _Probe()


def _run(tools, batch, call="target", token=None):
    emit = _next_type("contract_emit")
    calls = [{"id": "call-target", "name": call, "arguments": "{}"}]
    if batch:
        calls.append({"id": "call-sibling", "name": "sibling", "arguments": "{}"})
    ng.NodeFactory.register_type(emit, lambda name, _config, _ctx: _EmitCalls(name, calls))
    definition = {
        "name": "python_tool_contract",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"model": {"type": emit}, "tools": {"type": "tool_dispatch"}},
        "edges": [
            {"from": ng.START_NODE, "to": "model"},
            {"from": "model", "to": "tools"},
            {"from": "tools", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(tools=tools))
    engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
    config = ng.RunConfig(thread_id=_next_type("contract_thread"), input={"messages": []})
    if token is not None:
        config.cancel_token = token
    return engine.run(config)


def _seen(result, call_id="call-target"):
    for message in result.output["channels"]["messages"]["value"]:
        if message.get("role") == "tool" and message.get("tool_call_id") == call_id:
            return message
    return None


def _sibling():
    return _tool(ng.Tool, "sibling", lambda _args: "sibling ok")


BASES = [pytest.param(ng.Tool, id="serial"), pytest.param(ng.AsyncTool, id="async")]
BATCH = [pytest.param(False, id="single"), pytest.param(True, id="batch")]


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_python_exception_is_a_typed_tool_result(base, batch):
    def fail(_args):
        raise ValueError("bad input from the model")

    result = _run([_tool(base, "target", fail), _sibling()], batch)
    seen = _seen(result)
    assert seen is not None
    # The default execution policy cannot rule out an effect of a tool that
    # had started, so the model is told the outcome needs reconciling.
    assert seen["tool_status"] == "reconciliation_required"
    body = json.loads(seen["content"])
    assert "bad input from the model" in body["error"]
    assert body["status"] == "reconciliation_required"
    if batch:
        assert _seen(result, "call-sibling")["content"] == "sibling ok"


@pytest.mark.parametrize("batch", BATCH)
def test_unknown_tool_is_a_rejected_tool_result(batch):
    result = _run([_sibling()], batch, call="missing")
    seen = _seen(result)
    assert seen["tool_status"] == "rejected"
    assert json.loads(seen["content"])["error"] == "Tool not found: missing"


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_node_interrupt_from_a_tool_pauses_the_run(base, batch):
    def ask(_args):
        raise ng.NodeInterrupt({"cmd": "deploy"}, reason="target needs approval")

    result = _run([_tool(base, "target", ask), _sibling()], batch)
    assert result.interrupted, "the interrupt became a tool message the model could ignore"
    assert result.interrupt_node == "tools"
    assert result.interrupt_value["reason"] == "target needs approval"
    assert result.interrupt_value["value"] == {"cmd": "deploy"}


def _nested_engine_with_blocking_hook():
    """An inner engine whose FailClosed checkpoint Hook fails."""
    class _Write(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, _input):
            return [ng.ChannelWrite("value", "done")]

    node_type = _next_type("contract_inner")
    ng.NodeFactory.register_type(node_type, lambda name, _c, _x: _Write(name))
    inner = ng.GraphEngine.compile({
        "name": "inner",
        "channels": {"value": {"reducer": "overwrite"}},
        "nodes": {"work": {"type": node_type}},
        "edges": [{"from": ng.START_NODE, "to": "work"},
                  {"from": "work", "to": ng.END_NODE}],
    }, ng.NodeContext())
    inner.set_checkpoint_store(ng.InMemoryCheckpointStore())

    data = ng.HookDefinitionData()
    data.phase = ng.HookPhase.CheckpointPublished
    data.target_id = "audit"
    data.delivery = ng.HookDelivery.BlockingMandatory
    data.failure_mode = ng.HookFailureMode.FailClosed
    data.idempotency = ng.HookIdempotency.Idempotent
    data.effect = ng.ToolEffectClass.ReadOnly
    mapper = ng.HookInputMapper()
    mapper.kind = ng.HookInputMapperKind.Template
    mapper.value_template = {"kind": "checkpoint"}
    data.input_mapper = mapper

    def refuse(*_args):
        raise RuntimeError("audit sink unavailable")

    inner.set_hook_runtime(ng.create_hook_runtime(
        [ng.HookDefinition.create(data)], {"audit": refuse}))
    return inner


def test_hook_boundary_blocked_carries_invocation_id_and_state():
    inner = _nested_engine_with_blocking_hook()
    with pytest.raises(ng.HookBoundaryBlocked) as raised:
        inner.run(ng.RunConfig(thread_id=_next_type("inner")),
                  ng.RunMetadata(run_id="inner-run", owner_scope="owner"))
    assert raised.value.invocation_id.startswith("sha256:")
    # A failed attempt with none left is recorded as timed_out.
    assert raised.value.state == "timed_out"


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_hook_block_from_a_tool_stops_the_run(base, batch):
    inner = _nested_engine_with_blocking_hook()

    def sub_agent(_args):
        # A tool that runs its own engine: the inner Hook block escapes it.
        inner.run(ng.RunConfig(thread_id=_next_type("inner")),
                  ng.RunMetadata(run_id="inner-run", owner_scope="owner"))
        return "unreachable"

    with pytest.raises(ng.HookBoundaryBlocked) as raised:
        _run([_tool(base, "target", sub_agent), _sibling()], batch)
    assert raised.value.invocation_id.startswith("sha256:")
    # A failed attempt with none left is recorded as timed_out.
    assert raised.value.state == "timed_out"


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_cancelling_the_run_from_a_tool_cancels_the_run(base, batch):
    token = ng.CancelToken()

    def cancel(_args):
        token.cancel()
        return "finished after the cancel"

    with pytest.raises(RuntimeError, match="run cancelled"):
        _run([_tool(base, "target", cancel), _sibling()], batch, token=token)
