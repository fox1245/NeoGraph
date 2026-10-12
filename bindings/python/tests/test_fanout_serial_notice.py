"""The "fan-out ... executing serially" notice still reaches Python users.

A Python node body is synchronous: the engine runs it to the end on its thread,
so the branches of a fan-out whose nodes are Python nodes run one after another
unless the engine has a worker pool. That is the case the notice was written
for. It stays quiet when the engine has a pool, and when nothing fans out. (The
C++ tests cover coroutine branches, which overlap and are not reported.)
"""

import neograph_engine as ng

NOTICE = "executing serially"


class Leaf(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("results", [self._name])]


ng.NodeFactory.register_type("notice.leaf", lambda name, _config, _ctx: Leaf(name))


def _graph(width):
    nodes = {f"n{i}": {"type": "notice.leaf"} for i in range(width)}
    edges = []
    for name in nodes:
        edges += [{"from": "__start__", "to": name}, {"from": name, "to": "__end__"}]
    return {"name": "serial_notice", "channels": {"results": {"reducer": "append", "initial": []}},
            "nodes": nodes, "edges": edges}


def _stderr_of_a_run(capfd, monkeypatch, width, workers):
    monkeypatch.delenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", raising=False)
    engine = ng.GraphEngine.compile(_graph(width), ng.NodeContext())
    if workers != 1:
        engine.set_worker_count(workers)
    capfd.readouterr()
    engine.run(ng.RunConfig(thread_id="notice"))
    return capfd.readouterr().err


def test_python_branches_without_a_pool_are_reported_as_serial(capfd, monkeypatch):
    err = _stderr_of_a_run(capfd, monkeypatch, width=3, workers=1)
    assert err.count(NOTICE) == 1, err


def test_a_worker_pool_or_a_single_branch_has_nothing_to_report(capfd, monkeypatch):
    assert NOTICE not in _stderr_of_a_run(capfd, monkeypatch, width=3, workers=3)
    assert NOTICE not in _stderr_of_a_run(capfd, monkeypatch, width=1, workers=1)
