"""NeoGraph + openai-sdk hybrid WITH tool calling — agentic provider.

Pattern A from the cookbook README: do the tool-calling loop inside
the Python Provider's `complete()`. The user's openai SDK fires the
LLM call, sees `tool_calls`, dispatches Python functions in-process,
appends results, calls again, and returns only the final assistant
message to NeoGraph. The graph sees one provider call per turn, no
`tool_dispatch` node needed.

Run:
    pip install neograph-engine>=0.2.3 openai
    echo 'OPENAI_API_KEY=sk-...' > .env
    python hybrid_with_tools.py
"""

import json as stdjson
import os
import sys
from pathlib import Path

import neograph_engine as ng
from openai import OpenAI


# Tiny helpers for the demo's three tools
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


class AgenticOpenAIProvider(ng.Provider):
    """Provider that handles the OpenAI tool-calling loop in-process.

    On each `complete()`, runs the agentic loop until OpenAI emits a
    final assistant message (no tool_calls) or the cap is hit. NeoGraph
    sees exactly one provider call per graph step.
    """
    def __init__(self, client, tools=PYTHON_TOOLS, model="gpt-5.4-mini",
                 max_iterations=8):
        super().__init__()
        self.client    = client
        self.tools     = tools
        self.model     = model
        self.cap       = max_iterations
        self.tool_calls_made = 0

    def _sdk_tools(self):
        return [{"type": "function",
                 "function": {"name": name,
                              "description": meta["description"],
                              "parameters":  meta["parameters"]}}
                for name, meta in self.tools.items()]

    def complete(self, params):
        messages = [{"role": m.role, "content": m.content}
                    for m in params.messages]
        sdk_tools = self._sdk_tools()

        for step in range(self.cap):
            r = self.client.chat.completions.create(
                model=params.model or self.model,
                messages=messages,
                tools=sdk_tools,
                temperature=params.temperature,
            )
            choice = r.choices[0]

            if not choice.message.tool_calls:
                out = ng.ChatCompletion()
                out.message.role    = "assistant"
                out.message.content = choice.message.content or ""
                if r.usage:
                    out.usage.prompt_tokens     = r.usage.prompt_tokens
                    out.usage.completion_tokens = r.usage.completion_tokens
                    out.usage.total_tokens      = r.usage.total_tokens
                return out

            # Append the assistant message with tool_calls, then
            # dispatch each tool in Python and append its result.
            messages.append({
                "role": "assistant",
                "content": choice.message.content,
                "tool_calls": [{"id": tc.id, "type": "function",
                                "function": {"name": tc.function.name,
                                             "arguments": tc.function.arguments}}
                               for tc in choice.message.tool_calls],
            })
            for tc in choice.message.tool_calls:
                self.tool_calls_made += 1
                name = tc.function.name
                args = stdjson.loads(tc.function.arguments)
                fn = self.tools[name]["fn"]
                print(f"  [tool] {name}({args}) ", end="", file=sys.stderr)
                try:
                    result = fn(**args)
                except Exception as e:
                    result = f"error: {e}"
                print(f"→ {result}", file=sys.stderr)
                messages.append({"role": "tool",
                                 "tool_call_id": tc.id,
                                 "content": str(result)})

        # Cap hit. Surrender with whatever the last text was.
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = "(agent loop cap hit)"
        return out

    def get_name(self):
        return "agentic-openai"


def _load_env():
    for p in (Path(".env"), Path(__file__).parent / ".env",
              Path(__file__).resolve().parents[3] / ".env"):
        if p.exists():
            for line in p.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))
            return


def main() -> int:
    _load_env()
    if not os.environ.get("OPENAI_API_KEY"):
        print("OPENAI_API_KEY not set", file=sys.stderr)
        return 2

    client = OpenAI()
    provider = AgenticOpenAIProvider(client)
    ctx = ng.NodeContext(
        provider=provider,
        instructions="Use the tools when arithmetic or string ops are needed.",
    )

    # Single-step graph; the agentic loop is hidden inside complete().
    graph_def = {
        "name": "byo-openai-agentic",
        "channels": {"messages": {"reducer": "append"}},
        "nodes":    {"agent": {"type": "llm_call"}},
        "edges":    [{"from": ng.START_NODE, "to": "agent"},
                     {"from": "agent",       "to": ng.END_NODE}],
    }
    engine = ng.GraphEngine.compile(graph_def, ctx)

    user_q = ("Reverse the string 'NeoGraph', then count the words in "
              "'the quick brown fox', then compute 17*23+5. "
              "Use the tools — don't compute manually.")
    print(f"[user] {user_q}\n")

    result = engine.run(ng.RunConfig(
        thread_id="agentic-demo",
        input={"messages": [{"role": "user", "content": user_q}]},
    ))

    msgs = result.output["channels"]["messages"]["value"]
    final = msgs[-1]
    print()
    print(f"[assistant] {final.get('content', '')}")
    print()
    print(f"[stats] {provider.tool_calls_made} tool calls dispatched in-Python")
    return 0


if __name__ == "__main__":
    sys.exit(main())
