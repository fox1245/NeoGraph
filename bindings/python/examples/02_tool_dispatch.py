"""02 — Python Tool subclass + built-in tool_dispatch node.

A "fake LLM" emitter node writes a tool_call message to the
messages channel. The built-in `tool_dispatch` node finds the
matching Python Tool, runs it, and writes back the result message.

This is the LangChain-migration shape — Python defines tools,
the engine handles dispatch. No API key needed for this example
because we skip the LLM and hand-craft the tool_call.

Run:
    pip install neograph-engine
    python 02_tool_dispatch.py
"""

import json

import neograph_engine as ng


class CalculatorTool(ng.Tool):
    """A trivial Python tool the LLM (or our fake emitter) can call."""

    def get_name(self):
        return "calc"

    def get_definition(self):
        # The LLM uses this schema to decide *when* and *how* to call.
        return ng.ChatTool(
            name="calc",
            description="Multiply x by 2",
            parameters={
                "type": "object",
                "properties": {"x": {"type": "number"}},
                "required": ["x"],
            },
        )

    def execute(self, arguments):
        # Returned to the engine as the tool result message body.
        return str(arguments.get("x", 0) * 2)


class FakeLLMNode(ng.GraphNode):
    """Stands in for an LLM call. Writes a tool_call into messages.

    Real users would use the built-in `llm_call` node (which talks
    to an OpenAIProvider / SchemaProvider). For an offline demo
    we hand-craft the same wire shape the engine's tool_dispatch
    expects to see.
    """

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": "",
            "tool_calls": [{
                "id": "call_1",
                "name": "calc",
                "arguments": json.dumps({"x": 21}),
            }],
        }])]


ng.NodeFactory.register_type(
    "fake_llm",
    lambda name, config, ctx: FakeLLMNode(name),
)

definition = {
    "name": "tool_dispatch_demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {
        "llm":      {"type": "fake_llm"},
        "dispatch": {"type": "tool_dispatch"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "llm"},
        {"from": "llm",         "to": "dispatch"},
        {"from": "dispatch",    "to": ng.END_NODE},
    ],
}

# Pass the tools list to NodeContext — the engine takes ownership
# at compile time so Python references can drop afterwards.
ctx = ng.NodeContext(tools=[CalculatorTool()])
engine = ng.GraphEngine.compile(definition, ctx)

result = engine.run(ng.RunConfig(thread_id="t1", input={"messages": []}))

# Pull out the tool result message that ToolDispatchNode appended.
msgs = result.output["channels"]["messages"]["value"]
tool_msgs = [m for m in msgs if m.get("role") == "tool"]
print("tool result:", tool_msgs[0]["content"])  # "42"
print("full trace:", result.execution_trace)
