"""15 — Load a graph definition from a JSON file and run it.

Companion to `14_graph_to_json.py`. The JSON file produced by 14
is consumed verbatim here — same wiring, no code change. Custom
node-type factories still need to be registered before compile;
they live in this file's import-time code, not in the JSON.

Run:
    pip install neograph-engine
    python 14_graph_to_json.py    # produces my_graph.json
    python 15_graph_from_json.py  # loads it and runs

Pattern:

    definition = json.loads(Path("my_graph.json").read_text())
    engine     = ng.GraphEngine.compile(definition, ng.NodeContext())
    result     = engine.run(ng.RunConfig(...))
"""

import json
import sys
from pathlib import Path

import neograph_engine as ng


# --- Register the same node-type factory the JSON references ---
#
# This bit isn't in the JSON because Python classes can't be encoded
# to JSON. The convention is: your application's import-time code
# registers every custom type it knows about, then `compile()`
# resolves the JSON's "type" strings against that registry.

class DoublerNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        seed = input.state.get("seed") or 0
        return [ng.ChannelWrite("doubled", seed * 2)]


ng.NodeFactory.register_type(
    "doubler",
    lambda name, config, ctx: DoublerNode(name),
)


# --- Load + compile + run ---

graph_path = Path(__file__).with_name("my_graph.json")
if not graph_path.is_file():
    print(f"{graph_path.name} not found. Run 14_graph_to_json.py first.")
    sys.exit(1)

definition = json.loads(graph_path.read_text())
print(f"loaded {graph_path.name}: name={definition['name']!r}, "
      f"nodes={list(definition['nodes'])}")

engine = ng.GraphEngine.compile(definition, ng.NodeContext())

# Run it twice to show that this is the same engine as a dict-built one.
for seed in (5, 100):
    result = engine.run(ng.RunConfig(thread_id=f"t-{seed}", input={"seed": seed}))
    print(f"  seed={seed:>3} → doubled={result.output['channels']['doubled']['value']}")
