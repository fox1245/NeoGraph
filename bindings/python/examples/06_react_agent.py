"""06 — ReAct agent (LLM ↔ tools loop).

Real OpenAI call + a Python `Tool` subclass. The LLM decides whether
to call the tool, the engine dispatches, the result is fed back, the
LLM reads the result and produces the final answer.

The graph is a 2-node loop with a conditional edge: `llm → dispatch
→ llm → ... → __end__` once no more tool_calls appear in the latest
assistant message.

Run:
    pip install neograph-engine python-dotenv
    cp .env.example .env  # fill in OPENAI_API_KEY
    python 06_react_agent.py
"""

import json

from _common import ng, openai_provider


class CalculatorTool(ng.Tool):
    """Evaluate a math expression and return the result.

    Tiny sandboxed eval — only digits, operators, parentheses.
    Use `decimal.Decimal` or a real expr parser for production.
    """

    def get_name(self):
        return "calc"

    def get_definition(self):
        return ng.ChatTool(
            name="calc",
            description=(
                "Evaluate a math expression. Supports + - * / and "
                "parentheses. Example: '(2+3)*4'."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "expression": {
                        "type": "string",
                        "description": "The expression to evaluate.",
                    },
                },
                "required": ["expression"],
            },
        )

    def execute(self, arguments):
        expr = arguments.get("expression", "").strip()
        # Sandbox: reject anything outside the math charset.
        allowed = set("0123456789+-*/(). ")
        if not all(c in allowed for c in expr):
            return f"error: illegal character in expression: {expr!r}"
        try:
            return str(eval(expr, {"__builtins__": {}}, {}))
        except Exception as exc:
            return f"error: {exc}"


def has_tool_calls(messages):
    """Return True if the latest assistant message has tool_calls."""
    for msg in reversed(messages):
        if msg.get("role") == "assistant":
            return bool(msg.get("tool_calls"))
    return False


provider = openai_provider()

# Built-in `llm_call` and `tool_dispatch` nodes plus the
# `has_tool_calls` conditional condition (registered in the C++ side
# as a built-in). The conditional edge from `llm` routes back into
# `dispatch` when the LLM emitted a tool call, otherwise it ends.
definition = {
    "name": "react_agent",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {
        "llm":      {"type": "llm_call"},
        "dispatch": {"type": "tool_dispatch"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "llm"},
        {"from": "dispatch",    "to": "llm"},
    ],
    "conditional_edges": [
        {
            "from":      "llm",
            "condition": "has_tool_calls",
            "routes": {
                "true":  "dispatch",
                "false": ng.END_NODE,
            },
        },
    ],
}

ctx = ng.NodeContext(
    provider=provider,
    tools=[CalculatorTool()],
    instructions=(
        "You are a calculator-using assistant. When the user asks for "
        "an arithmetic answer, call the `calc` tool. Once you have a "
        "numeric result, reply with just the number — no extra "
        "commentary."
    ),
)

engine = ng.GraphEngine.compile(definition, ctx)

result = engine.run(ng.RunConfig(
    thread_id="t1",
    input={"messages": [
        {"role": "user", "content": "What is (123 + 456) * 7?"},
    ]},
    max_steps=10,
))

# Final answer is the last assistant message without tool_calls.
msgs = result.output["channels"]["messages"]["value"]
print("\n=== conversation ===")
for m in msgs:
    role = m.get("role", "?")
    content = m.get("content", "")
    tcs = m.get("tool_calls", [])
    if tcs:
        for tc in tcs:
            print(f"  [{role}] tool_call {tc['name']}({tc['arguments']})")
    elif content:
        print(f"  [{role}] {content}")
print("\nexecution trace:", result.execution_trace)
