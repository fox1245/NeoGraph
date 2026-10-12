"""75 — Stream modes: one run, seen four ways.

`run_stream()` calls you back while the graph runs. `RunConfig(stream_mode=...)` is a bit mask
that decides which events arrive. This example runs the same two-node graph once per mode and
counts what each mode delivers:

    VALUES   a full state snapshot after every super-step
    UPDATES  one event per channel write
    EVENTS   NODE_START and NODE_END for every node
    DEBUG    the engine's routing decisions (which nodes run next); one per hop between nodes

Modes combine with `|`. TOKENS (LLM tokens, needs a model) and ALL are not shown here.

Run:
    pip install neograph-engine
    python 75_stream_modes.py

smoke-expect: VALUES   node_events=0 snapshots=2 updates=0 routing=0
smoke-expect: UPDATES  node_events=0 snapshots=0 updates=3 routing=0
smoke-expect: EVENTS   node_events=4 snapshots=0 updates=0 routing=0
smoke-expect: DEBUG    node_events=0 snapshots=0 updates=0 routing=1
smoke-expect: EVENTS|UPDATES node_events=4 snapshots=0 updates=3 routing=0
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng


@ng.node("plan")
def plan(state):
    return [ng.ChannelWrite("steps", ["plan"])]                     # 1 write


@ng.node("work")
def work(state):
    return [ng.ChannelWrite("steps", ["work"]), ng.ChannelWrite("total", 42)]   # 2 writes


definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "stream_modes",
    "channels": {"steps": {"reducer": "append"}, "total": {"reducer": "overwrite"}},
    "nodes": {"plan": {"type": "plan"}, "work": {"type": "work"}},
    "edges": [
        {"from": ng.START_NODE, "to": "plan"},
        {"from": "plan", "to": "work"},
        {"from": "work", "to": ng.END_NODE},
    ],
}
engine = ng.GraphEngine.compile(definition, ng.NodeContext())

Type = ng.GraphEvent.Type


def observe(mode):
    """Run once in `mode` and sort the events it delivers by kind."""
    tally = {"node_events": 0, "snapshots": 0, "updates": 0, "routing": 0}

    def on_event(event):
        if event.type in (Type.NODE_START, Type.NODE_END):
            tally["routing" if event.node_name == "__routing__" else "node_events"] += 1
        elif event.type == Type.CHANNEL_WRITE:
            tally["snapshots" if event.node_name == "__state__" else "updates"] += 1

    engine.run_stream(ng.RunConfig(stream_mode=mode), on_event)
    return tally


for label, mode in [
    ("VALUES  ", ng.StreamMode.VALUES),
    ("UPDATES ", ng.StreamMode.UPDATES),
    ("EVENTS  ", ng.StreamMode.EVENTS),
    ("DEBUG   ", ng.StreamMode.DEBUG),
    ("EVENTS|UPDATES", ng.StreamMode.EVENTS | ng.StreamMode.UPDATES),
]:
    print(label, " ".join(f"{name}={count}" for name, count in observe(mode).items()))
