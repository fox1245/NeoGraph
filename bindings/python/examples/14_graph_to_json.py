"""14 — Graph definition is just JSON: save it to a file.

The Python `dict` you pass to `GraphEngine.compile()` is exactly the
same shape as the JSON the C++ engine consumes. So you can:

  1. Build the dict in code (this file).
  2. `json.dumps(definition)` → write it to a `.json` file.
  3. Load the same file later (15_graph_from_json.py) and compile.

This is the on-disk graph format LangGraph users expect — a
declarative graph spec separate from the runtime code that drives
it. Useful for:

  - shipping agent specs as artifacts (config repo, S3, DB)
  - editing graphs in a visual tool, then loading them at runtime
  - diffing graph changes in code review

Custom-node *types* still need to be registered in code (they
reference Python classes), but the graph wiring — channels, nodes
by type, edges, conditional edges, retry policies — is data.

Run:
    pip install neograph-engine
    python 14_graph_to_json.py
    # produces: my_graph.json
"""

import json
from pathlib import Path

import neograph_engine as ng


# Same 'doubler' pattern as 01_minimal.py — the simplest non-trivial
# graph that actually runs a node.
class DoublerNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        seed = state.get("seed") or 0
        return [ng.ChannelWrite("doubled", seed * 2)]


ng.NodeFactory.register_type(
    "doubler",
    lambda name, config, ctx: DoublerNode(name),
)


# Build the definition the same way you would in any other example.
definition = {
    "name": "doubler_graph",
    "channels": {
        "seed":    {"reducer": "overwrite"},
        "doubled": {"reducer": "overwrite"},
    },
    "nodes": {
        "d": {"type": "doubler"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "d"},
        {"from": "d",           "to": ng.END_NODE},
    ],
}

# Step 1 — confirm it compiles + runs as a dict.
engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"seed": 21}))
print("from-dict:    doubled =", result.output["channels"]["doubled"]["value"])

# Step 2 — serialize the definition to a file. Plain `json.dumps`,
# no special API needed.
out_path = Path(__file__).with_name("my_graph.json")
out_path.write_text(json.dumps(definition, indent=2))
print(f"wrote {out_path.name} ({out_path.stat().st_size} bytes)")

# Print the file so the on-disk shape is visible.
print("\n=== my_graph.json ===")
print(out_path.read_text())
print("=== end ===")

# Tip: if you wanted to ALSO ship the registered node-type → factory
# mapping, you'd do that at import time in your application code.
# The JSON itself only encodes wiring, by design.
