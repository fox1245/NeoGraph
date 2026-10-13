"""A node that only Send starts, and the stderr notices of GraphEngine.compile (#415).

No edge leads to a Send target, so the reachability lint (E7) reports it: the
topology holds no Send or Command target, only the nodes that return them at
run time. `compile(..., stderr_notices=False)` is the same switch as C++
`EngineConfig::stderr_notices`. A static edge to the target is not a way to
quiet the lint: the target then runs once more, with the sender's state.
"""

import neograph_engine as ng

LINT = "[neograph] lint [E7]"


class Fan(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.Send("worker", {"item": i}) for i in range(3)]


class Worker(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("seen", [input.state.get("item")])]


ng.NodeFactory.register_type("send_lint.fan", lambda name, _config, _ctx: Fan(name))
ng.NodeFactory.register_type("send_lint.worker", lambda name, _config, _ctx: Worker(name))


def _definition(static_edge=False):
    edges = [{"from": ng.START_NODE, "to": "fan"}, {"from": "worker", "to": ng.END_NODE}]
    if static_edge:
        edges.append({"from": "fan", "to": "worker"})
    return {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
        "name": "send_target_lint",
        "channels": {"item": {"reducer": "overwrite"}, "seen": {"reducer": "append", "initial": []}},
        "nodes": {"fan": {"type": "send_lint.fan"}, "worker": {"type": "send_lint.worker"}},
        "edges": edges,
    }


def _registry():
    registry = ng.GraphRegistry()
    registry.register_type("send_lint.fan", lambda name, _config, _ctx: Fan(name))
    registry.register_type("send_lint.worker", lambda name, _config, _ctx: Worker(name))
    return registry


def test_a_send_only_node_is_reported_by_default(capfd):
    capfd.readouterr()
    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext())
    err = capfd.readouterr().err
    assert err.count(LINT) == 1, err
    assert '"worker"' in err
    assert engine.run(ng.RunConfig()).execution_trace == ["fan"] + ["worker[send]"] * 3


def test_stderr_notices_false_prints_no_lint(capfd):
    capfd.readouterr()
    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext(), stderr_notices=False)
    assert capfd.readouterr().err == ""

    result = engine.run(ng.RunConfig())
    assert result.execution_trace == ["fan"] + ["worker[send]"] * 3
    assert sorted(result.output["channels"]["seen"]["value"]) == [0, 1, 2]
    assert capfd.readouterr().err == ""


def test_stderr_notices_true_is_the_default(capfd):
    capfd.readouterr()
    ng.GraphEngine.compile(_definition(), ng.NodeContext(), stderr_notices=True)
    assert capfd.readouterr().err.count(LINT) == 1


def test_the_switch_reaches_every_way_to_compile(capfd):
    capfd.readouterr()
    ng.GraphEngine.compile(_definition(), ng.NodeContext(), registry=_registry(), stderr_notices=False)
    ng.GraphEngine.compile(_definition(), ng.NodeContext(), failure_text_redactor=lambda _failure: None,
                           stderr_notices=False)
    assert capfd.readouterr().err == ""

    ng.GraphEngine.compile(_definition(), ng.NodeContext(), registry=_registry())
    assert capfd.readouterr().err.count(LINT) == 1


def test_a_graph_with_no_lint_prints_nothing_either_way(capfd):
    definition = _definition(static_edge=True)
    capfd.readouterr()
    ng.GraphEngine.compile(definition, ng.NodeContext())
    ng.GraphEngine.compile(definition, ng.NodeContext(), stderr_notices=False)
    assert capfd.readouterr().err == ""


def test_a_static_edge_to_a_send_target_adds_a_run_with_the_senders_state():
    result = ng.GraphEngine.compile(
        _definition(static_edge=True), ng.NodeContext()).run(ng.RunConfig())

    assert result.execution_trace == ["fan"] + ["worker[send]"] * 3 + ["worker"]
    # The three Sends carry their payloads; the static run sees the shared
    # state, where no payload ever landed.
    assert result.output["channels"]["seen"]["value"] == [0, 1, 2, None]
