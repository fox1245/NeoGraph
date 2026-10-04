"""03 — Send-based fan-out (map-reduce pattern).

A fan-out node returns a list of `Send` objects, one per work item.
The engine dispatches the same target node N times in parallel,
each with a distinct input. Channel writes from all branches are
merged via the channel's reducer.

Branches run on the engine's fan-out worker pool (see `set_worker_count`).
Python callbacks still obey the CPython GIL: I/O may overlap, but this small
pure-Python square calculation does not demonstrate CPU-core parallelism.

Run:
    pip install neograph-engine
    python 03_send_fanout.py
"""

import neograph_engine as ng


class FanOutNode(ng.GraphNode):
    """Emit 8 Send objects; each sends `worker` a different number."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        # run() returns a list mixing ChannelWrite/Send/Command.
        # Bare list of Send: each entry triggers one parallel branch.
        return [ng.Send("worker", {"item": i}) for i in range(8)]


class WorkerNode(ng.GraphNode):
    """Square the item and append to results."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        n = input.state.get("item")
        # Append-reducer on `results` accumulates each branch's write.
        return [ng.ChannelWrite("results", [n * n])]


ng.NodeFactory.register_type(
    "fanout",
    lambda name, config, ctx: FanOutNode(name),
)
ng.NodeFactory.register_type(
    "worker",
    lambda name, config, ctx: WorkerNode(name),
)

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "fanout_demo",
    "channels": {
        "item":    {"reducer": "overwrite"},
        "results": {"reducer": "append"},
    },
    "nodes": {
        "fan":    {"type": "fanout"},
        "worker": {"type": "worker"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "fan"},
        {"from": "worker",      "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
# Use four fan-out workers. Python callbacks still share the CPython GIL;
# parallel CPU-bound Python work requires a process or a GIL-releasing extension.
engine.set_worker_count(4)

result = engine.run(ng.RunConfig(thread_id="t1", input={}))

squares = sorted(result.output["channels"]["results"]["value"])
print("squares:", squares)  # [0, 1, 4, 9, 16, 25, 36, 49]
