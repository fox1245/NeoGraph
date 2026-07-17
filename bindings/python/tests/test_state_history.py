"""Checkpoint history inspection and historical forks (#118)."""

import neograph_engine as ng
import pytest


class _PassthroughNode(ng.GraphNode):
    def __init__(self):
        super().__init__()

    def run(self, input):
        return []

    def get_name(self):
        return "history_passthrough"


ng.NodeFactory.register_type(
    "py_state_history_passthrough", lambda *_: _PassthroughNode())


def _definition():
    return {
        "name": "python_state_history",
        "channels": {"value": {"reducer": "overwrite"}},
        "nodes": {
            "history_passthrough": {"type": "py_state_history_passthrough"},
        },
        "edges": [
            {"from": ng.START_NODE, "to": "history_passthrough"},
            {"from": "history_passthrough", "to": ng.END_NODE},
        ],
    }


def _value(state):
    return state["channels"]["value"]["value"]


def test_history_discovers_checkpoint_for_divergent_fork():
    store = ng.InMemoryCheckpointStore()
    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext(), store)
    engine.run(ng.RunConfig(thread_id="source", input={"value": 1}))
    engine.update_state("source", {"value": 2}, as_node="admin")

    history = engine.get_state_history("source")
    assert len(history) >= 2
    newest, previous = history[:2]
    assert newest.parent_id == previous.id
    assert newest.step == previous.step
    assert newest.timestamp > previous.timestamp
    assert newest.interrupt_phase == ng.CheckpointPhase.Updated
    assert _value(newest.channel_values) == 2
    assert [cp.id for cp in history] == [cp.id for cp in store.list("source")]
    assert [cp.id for cp in engine.get_state_history("source", limit=1)] == [
        newest.id,
    ]

    fork_id = engine.fork("source", "branch", checkpoint_id=previous.id)
    assert _value(engine.get_state("branch")) == 1
    fork_checkpoint = engine.get_state_history("branch")[0]
    assert fork_checkpoint.id == fork_id
    assert fork_checkpoint.parent_id == previous.id
    assert fork_checkpoint.metadata["forked_from"] == {
        "thread_id": "source",
        "checkpoint_id": previous.id,
    }

    engine.update_state("branch", {"value": 9})
    assert _value(engine.get_state("branch")) == 9
    assert _value(engine.get_state("source")) == 2

    engine.run(ng.RunConfig(thread_id="other", input={"value": 77}))
    other_id = engine.get_state_history("other")[0].id
    with pytest.raises(RuntimeError, match="does not belong"):
        engine.fork("source", "wrong-source", checkpoint_id=other_id)
    assert engine.get_state("wrong-source") is None


def test_history_without_store_or_matching_thread_is_empty():
    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext())
    assert engine.get_state_history("missing") == []

    engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
    assert engine.get_state_history("missing") == []

    with pytest.raises(ValueError, match="non-negative"):
        engine.get_state_history("missing", limit=-1)
