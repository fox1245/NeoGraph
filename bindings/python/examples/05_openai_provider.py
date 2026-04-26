"""05 — Real LLM call via OpenAIProvider (requires .env / OPENAI_API_KEY).

Wire an OpenAI-compatible provider into NodeContext, build a graph
that uses the built-in `llm_call` node, and run a one-shot
completion. Works with OpenAI, Groq, Together, vLLM, Ollama —
any endpoint speaking the /v1/chat/completions shape.

Run:
    pip install neograph-engine python-dotenv
    cp .env.example .env  # fill in OPENAI_API_KEY
    python 05_openai_provider.py

To target a different OpenAI-compatible endpoint or model, set
OPENAI_API_BASE / OPENAI_MODEL in .env. See .env.example.
"""

from _common import ng, openai_provider


provider = openai_provider()  # exits cleanly if no key

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
