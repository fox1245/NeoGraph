"""05 — Real Chat API call via typed SchemaProvider.

Wire an OpenAI-compatible provider into NodeContext, build a graph
that uses the built-in `llm_call` node, and run a one-shot
completion. The endpoint must implement the admitted OpenAI Chat protocol;
matching an endpoint name alone does not establish protocol compatibility.

Run:
    pip install neograph-engine python-dotenv
    cp .env.example .env  # fill in OPENAI_API_KEY
    python 05_openai_provider.py

To target a different OpenAI-compatible endpoint or model, set
OPENAI_API_BASE / OPENAI_MODEL in .env. See .env.example.
"""

from _common import example_model, ng, schema_provider


provider = schema_provider()

# Built-in `llm_call` node: reads the messages channel, calls the
# provider, writes the assistant message back. No subclassing needed
# for the common case.
definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "openai_oneshot",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}},
    "edges": [
        {"from": ng.START_NODE, "to": "llm"},
        {"from": "llm",         "to": ng.END_NODE},
    ],
}

ctx = ng.NodeContext(provider=provider, model=example_model())
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
if not assistant:
    raise RuntimeError("llm_call produced no assistant message")
print("assistant:", assistant[-1]["content"])
