"""01 — Your first graph: one node, two channels, no model.

A node is a function from state to channel writes. This one reads the `seed` channel, doubles
it and writes the result to the `doubled` channel. The graph is `__start__ -> double -> __end__`
and the definition is a plain dict. No API key is needed.

Run:
    pip install neograph-engine
    python 01_minimal.py

smoke-expect: doubled = 42
smoke-expect: trace:   ['double']
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng


# `@ng.node("double")` registers this function as the node type "double".
@ng.node("double")
def double(state):
    return [ng.ChannelWrite("doubled", state.get("seed") * 2)]


definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "minimal",
    # A channel is a named slot of state; "overwrite" means a write replaces the value.
    "channels": {
        "seed": {"reducer": "overwrite"},
        "doubled": {"reducer": "overwrite"},
    },
    "nodes": {"double": {"type": "double"}},
    "edges": [
        {"from": ng.START_NODE, "to": "double"},
        {"from": "double", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"seed": 21}))

print("doubled =", result.output["channels"]["doubled"]["value"])
print("trace:  ", result.execution_trace)
