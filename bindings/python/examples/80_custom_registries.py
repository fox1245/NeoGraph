"""80 — Your own reducer and your own condition.

The built-in reducers (`overwrite`, `append`) and conditions (`has_tool_calls`, `route_channel`)
cover most graphs. When they do not, register a Python callable under a name and use that name
in the graph definition:

  * a reducer `(current, incoming) -> new value` decides what a write does to a channel. Here
    `highest` keeps the largest score, so three judges that write at the same time give the
    same result in any order;
  * a condition `(state) -> label` picks the next node. Here `band` sends a score of 70 or more
    to `publish` and anything lower to `review`.

Both run under the GIL and are looked up by name when the graph is compiled; an unknown name
fails the compile. Re-registering a name replaces the earlier callable.

Run:
    pip install neograph-engine
    python 80_custom_registries.py

smoke-expect: scores [90, 60, 75] -> highest 90 -> publish
smoke-expect: scores [40, 55, 30] -> highest 55 -> review
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng

# 1. A reducer: `current` is None until the first write.
ng.ReducerRegistry.register_reducer(
    "highest", lambda current, incoming: incoming if current is None else max(current, incoming))

# 2. A condition: it reads state and returns one of the labels in the edge's `routes`.
ng.ConditionRegistry.register_condition(
    "band", lambda state: "publish" if state.get("score") >= 70 else "review")


class JudgeNode(ng.GraphNode):
    """Writes the score stored in the input channel named after the judge."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("score", input.state.get(f"vote_{self._name}"))]


class DeskNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return []


ng.NodeFactory.register_type("judge", lambda name, config, ctx: JudgeNode(name))
ng.NodeFactory.register_type("desk", lambda name, config, ctx: DeskNode(name))

JUDGES = ("a", "b", "c")
definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "custom_registries",
    "channels": {
        "score": {"reducer": "highest"},                 # the custom reducer, by name
        **{f"vote_{j}": {"reducer": "overwrite"} for j in JUDGES},
    },
    "nodes": {
        **{j: {"type": "judge"} for j in JUDGES},
        "tally": {"type": "desk", "barrier": {"wait_for": list(JUDGES)}},
        "publish": {"type": "desk"},
        "review": {"type": "desk"},
    },
    "edges": [
        *({"from": ng.START_NODE, "to": j} for j in JUDGES),
        *({"from": j, "to": "tally"} for j in JUDGES),
        # The custom condition, by name; the labels it returns are the keys of `routes`.
        {"from": "tally", "condition": "band",
         "routes": {"publish": "publish", "review": "review"}},
        {"from": "publish", "to": ng.END_NODE},
        {"from": "review", "to": ng.END_NODE},
    ],
}
engine = ng.GraphEngine.compile(definition, ng.NodeContext())
engine.set_worker_count(3)

for votes in ([90, 60, 75], [40, 55, 30]):
    result = engine.run(ng.RunConfig(input={f"vote_{j}": v for j, v in zip(JUDGES, votes)}))
    print(f"scores {votes} -> highest {result.output['channels']['score']['value']} "
          f"-> {result.execution_trace[-1]}")
