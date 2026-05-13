"""Python Tool trampoline — commit 6 of the binding.

Covers:
  - Subclass neograph_engine.Tool and dispatch via the
    built-in ``tool_dispatch`` node type.
  - NodeContext(tools=[...]) survives compile (engine takes
    ownership; Python references can be dropped without dangling).
  - Argument round-trip: dict in (parsed from JSON) → execute()
    sees a dict, returns string.
  - Multiple tools + dispatch picks by tool_call name.
"""

import json

import neograph_engine as neograph  # PyPI dist `neograph-engine`


_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


class _MockToolEmittingNode(neograph.GraphNode):
    """Custom node that pretends to be an LLM and emits a tool_call.

    Real users would use neograph_engine's `llm_call` node (which
    talks to a Provider). For testing without an API key, this node
    fakes the LLM by writing an assistant message containing a
    canned tool_call straight into the messages channel.
    """

    def __init__(self, name, tool_name, arguments_json):
        super().__init__()
        self._name = name
        self._tool_name = tool_name
        self._arguments_json = arguments_json

    def get_name(self):
        return self._name

    def run(self, input):
        state = input.state
        msg = neograph.ChatMessage(
            role="assistant",
            content="",
            tool_calls=[neograph.ToolCall(
                id="call_test_1",
                name=self._tool_name,
                arguments=self._arguments_json,
            )],
        )
        # Engine's ToolCall parser (types.h:from_json) expects flat
        # {id, name, arguments} — NOT the OpenAI-style nested
        # {id, type, function: {name, arguments}}. The provider
        # adapters handle that conversion on the way in; here we're
        # writing the engine's internal shape directly.
        return [neograph.ChannelWrite("messages", [{
            "role": "assistant",
            "content": "",
            "tool_calls": [{
                "id": "call_test_1",
                "name": self._tool_name,
                "arguments": self._arguments_json,
            }],
        }])]


class _CalculatorTool(neograph.Tool):
    """Doubles its `x` argument. Records calls for assertion."""
    def __init__(self):
        super().__init__()
        self.calls = []

    def get_name(self):
        return "calc"

    def get_definition(self):
        return neograph.ChatTool(
            name="calc",
            description="Multiply x by 2",
            parameters={
                "type": "object",
                "properties": {"x": {"type": "number"}},
                "required": ["x"],
            },
        )

    def execute(self, arguments):
        self.calls.append(arguments)
        return str(arguments.get("x", 0) * 2)


def test_python_tool_subclass_basic():
    """Tool subclass roundtrips through get_name / get_definition / execute."""
    t = _CalculatorTool()
    assert t.get_name() == "calc"
    defn = t.get_definition()
    assert defn.name == "calc"
    assert "x" in defn.parameters["properties"]
    assert t.execute({"x": 21}) == "42"


def test_node_context_holds_python_tools():
    """NodeContext._pytools is the Python-side tool list backing
    the C++ trampoline ownership transfer."""
    t1 = _CalculatorTool()
    t2 = _CalculatorTool()
    ctx = neograph.NodeContext(tools=[t1, t2])
    assert len(ctx._pytools) == 2
    assert ctx._pytools[0] is t1


def test_tool_dispatch_executes_python_tool():
    """End-to-end: emitter node writes a tool_call message, then
    the built-in `tool_dispatch` node finds the Python tool by name
    and runs it."""
    emit_type = _next_type("emit")

    calc = _CalculatorTool()

    neograph.NodeFactory.register_type(
        emit_type,
        lambda name, config, ctx: _MockToolEmittingNode(
            name, "calc", json.dumps({"x": 7})),
    )

    definition = {
        "name": "tool_dispatch_e2e",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {
            "emit":     {"type": emit_type},
            "dispatch": {"type": "tool_dispatch"},
        },
        "edges": [
            {"from": neograph.START_NODE, "to": "emit"},
            {"from": "emit",              "to": "dispatch"},
            {"from": "dispatch",          "to": neograph.END_NODE},
        ],
    }

    ctx = neograph.NodeContext(tools=[calc])
    engine = neograph.GraphEngine.compile(definition, ctx)

    # Drop the local reference — the engine should now hold ownership.
    del calc, ctx

    result = engine.run(neograph.RunConfig(thread_id="t1", input={"messages": []}))

    # Find the tool result message in the channel output.
    msgs = result.output["channels"]["messages"]["value"]
    tool_msgs = [m for m in msgs if m.get("role") == "tool"]
    assert len(tool_msgs) == 1, f"expected one tool message, got {msgs}"
    assert tool_msgs[0]["content"] == "14"  # 7 * 2


def test_engine_takes_ownership_of_tools():
    """After compile, the Python tool reference can drop without
    breaking subsequent runs — engine.own_tools() should have stashed
    a unique_ptr<PyToolOwner> internally."""
    emit_type = _next_type("emit_own")

    invocations = []

    class TrackedTool(neograph.Tool):
        def get_name(self): return "track"
        def get_definition(self):
            return neograph.ChatTool(name="track", description="track",
                parameters={"type": "object", "properties": {}})
        def execute(self, arguments):
            invocations.append(arguments)
            return "ok"

    neograph.NodeFactory.register_type(
        emit_type,
        lambda name, config, ctx: _MockToolEmittingNode(
            name, "track", json.dumps({"a": 1})),
    )

    tool = TrackedTool()
    ctx = neograph.NodeContext(tools=[tool])
    definition = {
        "name": "owned_tool",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {
            "emit":     {"type": emit_type},
            "dispatch": {"type": "tool_dispatch"},
        },
        "edges": [
            {"from": neograph.START_NODE, "to": "emit"},
            {"from": "emit",              "to": "dispatch"},
            {"from": "dispatch",          "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, ctx)

    # Drop both the Python Tool object and the NodeContext. If the
    # engine didn't take ownership, the next run would crash on a
    # use-after-free in the C++ Tool* dispatch table.
    del tool, ctx

    engine.run(neograph.RunConfig(thread_id="t1", input={"messages": []}))
    assert invocations == [{"a": 1}]


def test_multiple_tools_dispatch_by_name():
    """Two tools registered, emitter picks one by name — dispatch
    should call the matching one only."""
    emit_type = _next_type("multi")

    class AddTool(neograph.Tool):
        def get_name(self): return "add"
        def get_definition(self):
            return neograph.ChatTool(name="add", description="add",
                parameters={"type": "object",
                    "properties": {"a": {"type": "number"},
                                   "b": {"type": "number"}}})
        def execute(self, arguments):
            return str(arguments["a"] + arguments["b"])

    class MulTool(neograph.Tool):
        def get_name(self): return "mul"
        def get_definition(self):
            return neograph.ChatTool(name="mul", description="mul",
                parameters={"type": "object", "properties": {}})
        def execute(self, arguments):
            return "should not run"

    add, mul = AddTool(), MulTool()

    neograph.NodeFactory.register_type(
        emit_type,
        lambda name, config, ctx: _MockToolEmittingNode(
            name, "add", json.dumps({"a": 3, "b": 5})),
    )

    definition = {
        "name": "multi_tool",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {
            "emit":     {"type": emit_type},
            "dispatch": {"type": "tool_dispatch"},
        },
        "edges": [
            {"from": neograph.START_NODE, "to": "emit"},
            {"from": "emit",              "to": "dispatch"},
            {"from": "dispatch",          "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(
        definition, neograph.NodeContext(tools=[add, mul]))
    result = engine.run(neograph.RunConfig(thread_id="t1", input={"messages": []}))

    msgs = result.output["channels"]["messages"]["value"]
    tool_msgs = [m for m in msgs if m.get("role") == "tool"]
    assert len(tool_msgs) == 1
    assert tool_msgs[0]["content"] == "8"  # add(3, 5)
