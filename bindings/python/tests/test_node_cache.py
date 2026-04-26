"""Python-side tests for the per-node result cache."""

from __future__ import annotations

import neograph_engine as ng


class _CountingNode(ng.GraphNode):
    """Bumps an instance counter every time .execute() runs.
    Cache hits skip execute() entirely → counter stops growing."""

    def __init__(self, name):
        super().__init__()
        self._name = name
        self.calls = 0

    def get_name(self):
        return self._name

    def execute(self, state):
        self.calls += 1
        return [ng.ChannelWrite("out", "ran")]


def _build():
    node = _CountingNode("work")
    ng.NodeFactory.register_type(
        "_counting_for_cache",
        lambda name, c, ctx, _n=node: _n)
    defn = {
        "name": "cache_pytest",
        "channels": {"out": {"reducer": "overwrite"}},
        "nodes":    {"work": {"type": "_counting_for_cache"}},
        "edges": [
            {"from": ng.START_NODE, "to": "work"},
            {"from": "work",        "to": ng.END_NODE},
        ],
    }
    e = ng.GraphEngine.compile(defn, ng.NodeContext())
    e.set_checkpoint_store(ng.InMemoryCheckpointStore())
    return e, node


def test_cache_off_by_default():
    e, node = _build()
    cfg1 = ng.RunConfig(thread_id="a", input={}, max_steps=5)
    cfg2 = ng.RunConfig(thread_id="b", input={}, max_steps=5)
    e.run(cfg1)
    e.run(cfg2)
    assert node.calls == 2
    assert e.node_cache_stats() == {"size": 0, "hits": 0, "misses": 0}


def test_enable_caches_identical_runs():
    e, node = _build()
    e.set_node_cache_enabled("work", True)

    for tid in ("a", "b", "c"):
        e.run(ng.RunConfig(thread_id=tid, input={}, max_steps=5))

    assert node.calls == 1
    stats = e.node_cache_stats()
    assert stats["hits"]   == 2
    assert stats["misses"] == 1
    assert stats["size"]   == 1


def test_clear_drops_entries():
    e, node = _build()
    e.set_node_cache_enabled("work", True)

    e.run(ng.RunConfig(thread_id="a", input={}, max_steps=5))
    assert node.calls == 1

    e.clear_node_cache()
    assert e.node_cache_stats()["size"] == 0

    e.run(ng.RunConfig(thread_id="b", input={}, max_steps=5))
    # Cache cleared → re-runs.
    assert node.calls == 2


def test_disable_after_enable_stops_caching():
    e, node = _build()
    e.set_node_cache_enabled("work", True)
    e.run(ng.RunConfig(thread_id="a", input={}, max_steps=5))
    assert node.calls == 1

    e.set_node_cache_enabled("work", False)
    e.run(ng.RunConfig(thread_id="b", input={}, max_steps=5))
    e.run(ng.RunConfig(thread_id="c", input={}, max_steps=5))
    # Both subsequent runs hit the node — disabled lookups skip the cache.
    assert node.calls == 3
