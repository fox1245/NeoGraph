"""Execute OpenAI SDK tool calls inside one NeoGraph custom graph node.

Requires the current typed-provider neograph-engine release and ``openai``.
The SDK call loop runs Python tools and writes only its final assistant text
into the messages channel. It does not manufacture typed provider evidence.

Run ``python hybrid_with_tools.py``. Hosted calls require OPENROUTER_API_KEY
and NG_ALLOW_HOSTED_CALLS=1. For a no-cost smoke, set
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 and OPENROUTER_MODEL=fixture-model.
Loopback calls use a dummy credential. The sibling hybrid.py supplies endpoint
configuration and the hosted-call guard.

The peer accepts buffered POST /v1/chat/completions with three function tools
and provider={"zdr": true}. First return assistant tool_calls for reverse_string
({"s":"NeoGraph"}), word_count ({"text":"the quick brown fox"}), and calc
({"expr":"17*23+5"}), with distinct ids and finish_reason="tool_calls".
The second request must contain that assistant tool-call message followed by
three role=tool messages with the matching ids and contents hparGoeN, 4, 396.
Return a final assistant choice without tool_calls and finish_reason="stop".
Expected graph state: messages contains the user and final assistant only,
tool_calls=3, and sdk_usage contains each response's usage dictionary or None.
Eight consecutive tool-call responses raise an error without a fake answer.
"""

import json
import os
import sys

import neograph_engine as ng
from openai import OpenAI

from hybrid import MODEL, _endpoint_and_key, _load_env_if_present


def calc(expr: str) -> str:
    return str(eval(expr, {"__builtins__": {}}, {}))


def reverse_string(s: str) -> str:
    return s[::-1]


def word_count(text: str) -> str:
    return str(len(text.split()))


PYTHON_TOOLS = {
    "calc": {
        "fn": calc,
        "description": "Evaluate a Python arithmetic expression.",
        "parameters": {
            "type": "object",
            "properties": {"expr": {"type": "string"}},
            "required": ["expr"],
        },
    },
    "reverse_string": {
        "fn": reverse_string,
        "description": "Reverse a string.",
        "parameters": {
            "type": "object",
            "properties": {"s": {"type": "string"}},
            "required": ["s"],
        },
    },
    "word_count": {
        "fn": word_count,
        "description": "Count words in a string.",
        "parameters": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
}


class AgenticOpenAINode(ng.GraphNode):
    """Keep SDK history and tool execution local until a final answer arrives."""

    def __init__(self, name, client, model=MODEL, max_iterations=8):
        super().__init__()
        self._name = name
        self.client = client
        self.model = model
        self.cap = max_iterations
        self.tool_calls_made = 0
        self.calls = 0
        self.sdk_tools = [
            {"type": "function", "function": {
                "name": name,
                "description": meta["description"],
                "parameters": meta["parameters"],
            }}
            for name, meta in PYTHON_TOOLS.items()
        ]

    def get_name(self):
        return self._name

    def run(self, input):
        messages = [{"role": "system", "content": "Use the tools when arithmetic or string ops are needed."}]
        messages.extend(input.state.get("messages") or [])
        usage = []
        tool_count = 0

        for step in range(self.cap):
            self.calls += 1
            response = self.client.chat.completions.create(
                model=self.model,
                messages=messages,
                tools=self.sdk_tools,
                temperature=0.7,
                extra_body={"provider": {"zdr": True}},
            )
            message = response.choices[0].message
            usage.append(response.usage.model_dump() if response.usage else None)
            if not message.tool_calls:
                return [
                    ng.ChannelWrite("messages", [{"role": "assistant", "content": message.content or ""}]),
                    ng.ChannelWrite("tool_calls", tool_count),
                    ng.ChannelWrite("sdk_usage", usage),
                ]

            messages.append({
                "role": "assistant",
                "content": message.content,
                "tool_calls": [{"id": call.id, "type": "function", "function": {
                    "name": call.function.name,
                    "arguments": call.function.arguments,
                }} for call in message.tool_calls],
            })
            for call in message.tool_calls:
                self.tool_calls_made += 1
                tool_count += 1
                name = call.function.name
                try:
                    arguments = json.loads(call.function.arguments)
                    result = PYTHON_TOOLS[name]["fn"](**arguments)
                except Exception as error:
                    result = f"error: {error}"
                print(f"[tool] {name} -> {result}", file=sys.stderr)
                messages.append({"role": "tool", "tool_call_id": call.id, "content": str(result)})

        raise RuntimeError(f"OpenAI SDK tool loop exhausted its {self.cap}-call limit")


def main() -> int:
    _load_env_if_present()
    try:
        base_url, api_key = _endpoint_and_key()
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2

    with OpenAI(api_key=api_key, base_url=base_url, timeout=120) as client:
        node = AgenticOpenAINode("agent", client, os.environ.get("OPENROUTER_MODEL", MODEL))
        registry = ng.GraphRegistry()
        registry.register_type("agentic_openai_sdk", lambda name, config, ctx: node)
        graph_def = {
            "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
            "name": "byo-openai-agentic",
            "channels": {
                "messages": {"reducer": "append"},
                "tool_calls": {"reducer": "overwrite"},
                "sdk_usage": {"reducer": "overwrite"},
            },
            "nodes": {"agent": {"type": "agentic_openai_sdk"}},
            "edges": [{"from": ng.START_NODE, "to": "agent"},
                      {"from": "agent", "to": ng.END_NODE}],
        }
        engine = ng.GraphEngine.compile(graph_def, ng.NodeContext(), registry=registry)
        user_q = ("Reverse the string 'NeoGraph', then count the words in "
                  "'the quick brown fox', then compute 17*23+5. "
                  "Use the tools - don't compute manually.")
        print(f"[user] {user_q}")
        result = engine.run(ng.RunConfig(
            thread_id="agentic-demo",
            input={"messages": [{"role": "user", "content": user_q}]},
        ))
        final = result.output["channels"]["messages"]["value"][-1]
        print(f"[assistant] {final.get('content', '')}")
        print(f"[stats] {node.tool_calls_made} tool calls dispatched in Python across {node.calls} SDK calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
