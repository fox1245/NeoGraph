"""18 — Per-node result caching (`set_node_cache_enabled`).

Mark a node as cacheable; the engine hashes the input state, looks up
`(node_name, hash)`, and replays the cached `NodeResult` on hit — no
LLM call, no token cost.

Useful for: dev iteration, eval harnesses, identical-input batch jobs.
Not useful for: streaming runs (cached hits cannot replay tokens, so
the cache is bypassed automatically there).

Run:
    pip install neograph-engine python-dotenv
    echo 'OPENAI_API_KEY=sk-...' > .env
    python 18_node_cache.py
"""

import time

import neograph_engine as ng
from _common import schema_provider


PROVIDER = schema_provider(schema="openai", default_model="gpt-5.4-mini")


class ExpensiveNode(ng.GraphNode):
    """Calls the LLM. The cache eliminates duplicate calls when the
    incoming state hashes to a previously-seen value."""

    def __init__(self, name):
        super().__init__()
        self._name = name
        self.calls = 0

    def get_name(self):
        return self._name

    def execute(self, state):
        self.calls += 1
        topic = state.get("topic") or ""
        c = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(
                role="user",
                content=f"Give one short fun fact about {topic}.")],
            temperature=0.0))  # deterministic so cache hits are visible
        return [ng.ChannelWrite("answer", c.message.content.strip())]


node = ExpensiveNode("ask")
ng.NodeFactory.register_type(
    "expensive_ask",
    lambda name, config, ctx, _n=node: _n)

definition = {
    "name": "cache_demo",
    "channels": {
        "topic":  {"reducer": "overwrite"},
        "answer": {"reducer": "overwrite"},
    },
    "nodes": {"ask": {"type": "expensive_ask"}},
    "edges": [
        {"from": ng.START_NODE, "to": "ask"},
        {"from": "ask",         "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
engine.set_node_cache_enabled("ask", True)


def run_once(label, topic):
    t0 = time.perf_counter()
    out = engine.run(ng.RunConfig(
        thread_id=f"{label}-{int(t0)}",
        input={"topic": topic},
        max_steps=5))
    dt = time.perf_counter() - t0
    answer = out.output["channels"]["answer"]["value"]
    print(f"  [{label}]  {dt:5.2f}s  calls={node.calls}  ::  {answer}")


print("Cache enabled for 'ask'. Three runs with the same topic:\n")
run_once("run 1", "octopuses")
run_once("run 2", "octopuses")  # cache hit — no LLM call
run_once("run 3", "octopuses")  # cache hit
print("\nA different topic forces a fresh LLM call:")
run_once("run 4", "honeybees")
run_once("run 5", "honeybees")  # cache hit on the new key

print(f"\nfinal stats: {engine.node_cache_stats()}")
print(f"LLM calls actually made: {node.calls}")
