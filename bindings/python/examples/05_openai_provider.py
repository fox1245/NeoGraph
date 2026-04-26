"""05 — Real LLM call via OpenAIProvider (requires OPENAI_API_KEY).

Wire an OpenAI-compatible provider into NodeContext, build a graph
that uses the built-in `llm_call` node, and run a one-shot
completion. Works with OpenAI, Groq, Together, vLLM, Ollama —
any endpoint speaking the /v1/chat/completions shape.

Run:
    pip install neograph-engine
    export OPENAI_API_KEY=sk-...
    python 05_openai_provider.py

To target a different OpenAI-compatible endpoint:
    export OPENAI_API_BASE=https://api.groq.com  # for Groq
    export OPENAI_MODEL=llama-3.3-70b-versatile
"""

import os
import sys

import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider


api_key = os.environ.get("OPENAI_API_KEY")
if not api_key:
    print("OPENAI_API_KEY not set — skipping the live LLM call.")
    print("Set it and re-run, or point OPENAI_API_BASE at any "
          "OpenAI-compatible endpoint (Groq, Together, vLLM, ...).")
    sys.exit(0)

provider = OpenAIProvider(
    api_key=api_key,
    base_url=os.environ.get("OPENAI_API_BASE", "https://api.openai.com"),
    default_model=os.environ.get("OPENAI_MODEL", "gpt-4o-mini"),
)

# Built-in `llm_call` node: reads the messages channel, calls the
# provider, writes the assistant message back. No subclassing needed
# for the common case.
definition = {
    "name": "openai_oneshot",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}},
    "edges": [
        {"from": ng.START_NODE, "to": "llm"},
        {"from": "llm",         "to": ng.END_NODE},
    ],
}

ctx = ng.NodeContext(provider=provider)
engine = ng.GraphEngine.compile(definition, ctx)

input_state = {
    "messages": [
        {"role": "user", "content": "Say 'hello world' and nothing else."},
    ],
}

result = engine.run(ng.RunConfig(thread_id="t1", input=input_state))

# Pull the LLM's response out of the messages channel.
msgs = result.output["channels"]["messages"]["value"]
assistant = [m for m in msgs if m.get("role") == "assistant"]
if assistant:
    print("assistant:", assistant[0]["content"])
else:
    print("no assistant message — full messages:", msgs)
