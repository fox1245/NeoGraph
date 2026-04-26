"""01 — Minimal Python custom node, no LLM.

Smallest possible NeoGraph graph: one custom Python node that reads
a channel, multiplies, writes the result back. No API key needed.

Run:
    pip install neograph-engine
    python 01_minimal.py
"""

import neograph_engine as ng


class DoublerNode(ng.GraphNode):
    """Reads `seed`, writes `doubled = seed * 2`."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        seed = state.get("seed") or 0
        return [ng.ChannelWrite("doubled", seed * 2)]


# Register the node type so the JSON definition can reference it.
ng.NodeFactory.register_type(
    "doubler",
    lambda name, config, ctx: DoublerNode(name),
)

# Compile a 1-node graph: __start__ → doubler → __end__
definition = {
    "name": "minimal",
    "channels": {
        "seed":    {"reducer": "overwrite"},
        "doubled": {"reducer": "overwrite"},
    },
    "nodes": {"d": {"type": "doubler"}},
    "edges": [
        {"from": ng.START_NODE, "to": "d"},
        {"from": "d",           "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())

cfg = ng.RunConfig(thread_id="t1", input={"seed": 21})
result = engine.run(cfg)

print("doubled =", result.output["channels"]["doubled"]["value"])
print("trace:  ", result.execution_trace)
