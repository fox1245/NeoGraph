"""05 — One model call through the built-in `llm_call` node.

Wire a provider into NodeContext, build a graph that uses the built-in
`llm_call` node, and run a one-shot completion.

Offline by default: a scripted model (see `_scripted.py`) answers "hello world"
through the real SchemaProvider and a local HTTP server, so no key is needed.

Run:
    python 05_openai_provider.py           # scripted model, no key, no network
    export OPENAI_API_KEY=...              # then, to call a hosted model (billed):
    python 05_openai_provider.py --live

--live uses OPENAI_API_BASE / OPENAI_MODEL when set. The endpoint must implement
the admitted OpenAI Chat protocol; matching an endpoint name alone does not
establish protocol compatibility.

smoke-expect: provider: scripted
smoke-expect: assistant: hello world
"""

from _common import example_model, example_provider, ng
from _scripted import say

with example_provider([say("hello world")]) as chosen:
    provider = chosen.provider

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
    if chosen.scripted:
        assert chosen.scripted.call_count == 1, "one llm_call, one model call"
