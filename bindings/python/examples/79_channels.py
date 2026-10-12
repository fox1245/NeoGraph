"""79 — Channels, reducers and versions.

A channel is a named slot of graph state. Its reducer decides what a write does to it:

    overwrite   the new value replaces the old one
    append      the new value (a list) is added to the end of the old list

A channel can start from an `initial` value. Every write also stamps the channel with a
version, taken from ONE counter shared by all channels: it tells you the order of writes, not
how many a channel received. A channel nobody wrote keeps version 0.
Three nodes run in turn, each writing the two channels; the example prints what is left in
each channel and its version. See example 80 for your own reducer.

Run:
    pip install neograph-engine
    python 79_channels.py

smoke-expect: overwrite last   = 'third'  (version 5)
smoke-expect: append    all    = ['first', 'second', 'third']  (version 6)
smoke-expect: initial   budget = 100  (version 0, never written)
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng


class WriterNode(ng.GraphNode):
    """Writes its own name to `last` (overwrite) and appends it to `all` (append)."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("last", self._name),
                ng.ChannelWrite("all", [self._name])]      # append needs a list


ng.NodeFactory.register_type("writer", lambda name, config, ctx: WriterNode(name))

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "channels",
    "channels": {
        "last": {"reducer": "overwrite"},
        "all": {"reducer": "append"},
        # `initial` is what a reader sees before the first write.
        "budget": {"reducer": "overwrite", "initial": 100},
    },
    "nodes": {name: {"type": "writer"} for name in ("first", "second", "third")},
    "edges": [
        {"from": ng.START_NODE, "to": "first"},
        {"from": "first", "to": "second"},
        {"from": "second", "to": "third"},
        {"from": "third", "to": ng.END_NODE},
    ],
}
result = ng.GraphEngine.compile(definition, ng.NodeContext()).run(ng.RunConfig())

# output["channels"][name] holds {"value": ..., "version": ...}.
for label, channel, note in [("overwrite", "last", ""), ("append   ", "all", ""),
                             ("initial  ", "budget", ", never written")]:
    entry = result.output["channels"][channel]
    print(f"{label} {channel:<6} = {entry['value']!r}  (version {entry['version']}{note})")
