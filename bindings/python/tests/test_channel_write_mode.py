"""ChannelWrite.Mode from Python (issue #91).

The C++ side gained a way to overwrite an accumulating channel instead of only
ever growing it. Python had no way to ask for that: ChannelWrite's constructor
took (channel, value) and nothing else, so the feature existed but half the
users could not reach it — and Python users hit the same wall, since `messages`
is append-reduced there too.
"""

import neograph_engine as ng


def _definition():
    return {
        "name": "py_channel_write_mode",
        "channels": {"log": {"reducer": "append"}},
        "nodes": {"seed": {"type": "cwm_seed"}, "trim": {"type": "cwm_trim"}},
        "edges": [
            {"from": "__start__", "to": "seed"},
            {"from": "seed", "to": "trim"},
            {"from": "trim", "to": "__end__"},
        ],
    }


class _SeedNode(ng.GraphNode):
    """Accumulates two entries through the append reducer."""

    def get_name(self):
        return "seed"

    def run(self, _input):
        return [ng.ChannelWrite("log", ["a"]), ng.ChannelWrite("log", ["b"])]


def test_overwrite_replaces_an_accumulated_channel():
    class TrimNode(ng.GraphNode):
        def get_name(self):
            return "trim"

        def run(self, _input):
            return [
                ng.ChannelWrite("log", ["trimmed"], ng.ChannelWrite.Mode.OVERWRITE)
            ]

    ng.NodeFactory.register_type("cwm_seed", lambda n, c, x: _SeedNode())
    ng.NodeFactory.register_type("cwm_trim", lambda n, c, x: TrimNode())

    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext())
    cfg = ng.RunConfig()
    cfg.thread_id = "py-cwm-overwrite"

    result = engine.run(cfg)
    assert result.output["channels"]["log"]["value"] == ["trimmed"]


def test_default_mode_still_reduces():
    """Existing Python code passes no mode and must keep appending."""

    class AppendNode(ng.GraphNode):
        def get_name(self):
            return "trim"

        def run(self, _input):
            return [ng.ChannelWrite("log", ["c"])]

    ng.NodeFactory.register_type("cwm_seed", lambda n, c, x: _SeedNode())
    ng.NodeFactory.register_type("cwm_trim", lambda n, c, x: AppendNode())

    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext())
    cfg = ng.RunConfig()
    cfg.thread_id = "py-cwm-default"

    result = engine.run(cfg)
    assert result.output["channels"]["log"]["value"] == ["a", "b", "c"]


def test_mode_enum_is_exposed():
    w = ng.ChannelWrite("log", [1], ng.ChannelWrite.Mode.OVERWRITE)
    assert w.mode == ng.ChannelWrite.Mode.OVERWRITE
    assert "OVERWRITE" in repr(w)

    plain = ng.ChannelWrite("log", [1])
    assert plain.mode == ng.ChannelWrite.Mode.REDUCE
