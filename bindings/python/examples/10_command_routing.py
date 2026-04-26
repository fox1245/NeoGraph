"""10 — Command-based routing override + state update.

A node returns `Command(goto_node=..., updates=[...])` to
simultaneously write state and choose the next node, bypassing the
graph's edge-based routing. Useful for guard-rail nodes, supervisors
that direct work to specific specialists, error handlers, etc.

No LLM — illustrates the engine semantics with custom Python nodes.

Run:
    pip install neograph-engine
    python 10_command_routing.py
"""

import neograph_engine as ng


class GuardNode(ng.GraphNode):
    """Inspects incoming `value` and routes to either `accept` or
    `reject` while writing a status field."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute_full(self, state):
        value = state.get("value") or 0
        if value >= 100:
            return ng.Command(
                goto_node="accept",
                updates=[ng.ChannelWrite("status", "auto-approved (>=100)")],
            )
        if value < 0:
            return ng.Command(
                goto_node="reject",
                updates=[ng.ChannelWrite("status", "rejected (negative)")],
            )
        return ng.Command(
            goto_node="manual",
            updates=[ng.ChannelWrite("status", "needs human review")],
        )


class StubNode(ng.GraphNode):
    """Just records that it ran."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        return [ng.ChannelWrite("audit_log", [self._name])]


ng.NodeFactory.register_type(
    "guard",
    lambda name, config, ctx: GuardNode(name),
)
ng.NodeFactory.register_type(
    "stub",
    lambda name, config, ctx: StubNode(name),
)


definition = {
    "name": "command_routing",
    "channels": {
        "value":     {"reducer": "overwrite"},
        "status":    {"reducer": "overwrite"},
        "audit_log": {"reducer": "append"},
    },
    "nodes": {
        "guard":  {"type": "guard"},
        "accept": {"type": "stub"},
        "manual": {"type": "stub"},
        "reject": {"type": "stub"},
    },
    # Default edges — Command overrides these. The default `from
    # guard → accept` would always pick accept; Command lets us
    # short-circuit to manual / reject based on input.
    "edges": [
        {"from": ng.START_NODE, "to": "guard"},
        {"from": "guard",  "to": "accept"},
        {"from": "accept", "to": ng.END_NODE},
        {"from": "manual", "to": ng.END_NODE},
        {"from": "reject", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())


for value in [200, 50, -10]:
    result = engine.run(ng.RunConfig(
        thread_id=f"v-{value}",
        input={"value": value, "audit_log": []},
    ))
    chans = result.output["channels"]
    print(f"\n=== value={value}")
    print(f"    status:  {chans['status']['value']}")
    print(f"    audit:   {chans['audit_log']['value']}")
    print(f"    trace:   {result.execution_trace}")
