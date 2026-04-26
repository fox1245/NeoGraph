"""04 — asyncio-compatible run_async + asyncio.gather.

Drive the engine from an asyncio loop. Multiple in-flight runs
(distinct thread_ids) overlap on the binding's internal asio
worker — useful for fan-out across user sessions or for batching
LLM-bound agents on a single engine instance.

Run:
    pip install neograph-engine
    python 04_async_concurrent.py
"""

import asyncio

import neograph_engine as ng


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

definition = {
    "name": "async_demo",
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


async def go():
    # Eight concurrent runs, each on its own thread_id.
    coros = [
        engine.run_async(ng.RunConfig(thread_id=f"t{i}", input={"seed": i}))
        for i in range(8)
    ]
    results = await asyncio.gather(*coros)
    return [r.output["channels"]["doubled"]["value"] for r in results]


vals = asyncio.run(go())
print("doubled:", vals)  # [0, 2, 4, 6, 8, 10, 12, 14]


# Streaming variant: events from the asio worker are hopped onto
# the asyncio loop thread, so the user's callback runs where
# they expect.
async def streamed():
    events = []

    def cb(ev):
        events.append((ev.type.name, ev.node_name))

    cfg = ng.RunConfig(thread_id="stream-1", input={"seed": 99})
    await engine.run_stream_async(cfg, cb)
    return events


events = asyncio.run(streamed())
print("event count:", len(events))
print("event sample:", events[:4])
