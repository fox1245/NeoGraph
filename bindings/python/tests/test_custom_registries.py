"""Tests for the Python ReducerRegistry / ConditionRegistry hooks.

Both registries are C++ singletons; the Python side stashes callables
on a module dict (`_python_reducers` / `_python_conditions`) and the
C++ closure looks them up by name at call time. The dict-on-module
trick avoids the Py_DECREF-after-Py_Finalize hazard you'd hit if
the std::function captured a py::function directly.

These tests exercise the registration hooks plus the GIL behaviour
under fan-out (multiple worker threads invoking the same Python
reducer concurrently — must not corrupt or deadlock).
"""
from __future__ import annotations

import threading

import pytest

import neograph_engine as ng


# --------------------------------------------------------------------- #
# Custom reducer
# --------------------------------------------------------------------- #


def test_register_python_reducer_runs_in_pipeline():
    """A Python `sum` reducer drives a real channel through the C++ engine."""
    ng.ReducerRegistry.register_reducer(
        "test_sum", lambda c, i: (c or 0) + i
    )

    @ng.node("incr")
    def incr(state):
        return [ng.ChannelWrite("counter", 1)]

    definition = {
        "name": "sum_reducer_test",
        "channels": {"counter": {"reducer": "test_sum"}},
        "nodes": {"incr": {"type": "incr"}},
        "edges": [
            {"from": ng.START_NODE, "to": "incr"},
            {"from": "incr", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())

    # Run the graph 4 times; each run starts fresh with input counter=0
    # and writes one increment. We check ONE run accumulates correctly.
    result = engine.run(ng.RunConfig(thread_id="t1", input={"counter": 10}))
    assert result.output["channels"]["counter"]["value"] == 11


def test_register_python_reducer_replaces_on_duplicate_name():
    """Re-registering an existing name takes effect immediately."""
    ng.ReducerRegistry.register_reducer(
        "test_replace", lambda c, i: "first"
    )
    ng.ReducerRegistry.register_reducer(
        "test_replace", lambda c, i: "second"
    )

    @ng.node("write")
    def write(state):
        return [ng.ChannelWrite("ch", "X")]

    definition = {
        "name": "replace",
        "channels": {"ch": {"reducer": "test_replace"}},
        "nodes": {"write": {"type": "write"}},
        "edges": [
            {"from": ng.START_NODE, "to": "write"},
            {"from": "write", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    result = engine.run(ng.RunConfig(thread_id="t1", input={"ch": ""}))
    assert result.output["channels"]["ch"]["value"] == "second"


def test_register_python_reducer_with_complex_types():
    """JSON round-trip: dict / list / nested values pass through cleanly."""
    def merge_dicts(c, i):
        out = dict(c) if c else {}
        out.update(i)
        return out

    ng.ReducerRegistry.register_reducer("test_merge", merge_dicts)

    @ng.node("write")
    def write(state):
        return [ng.ChannelWrite("settings", {"b": 2, "c": [1, 2, 3]})]

    definition = {
        "name": "merge",
        "channels": {"settings": {"reducer": "test_merge"}},
        "nodes": {"write": {"type": "write"}},
        "edges": [
            {"from": ng.START_NODE, "to": "write"},
            {"from": "write", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    result = engine.run(ng.RunConfig(thread_id="t1",
                                     input={"settings": {"a": 1}}))
    assert result.output["channels"]["settings"]["value"] == {
        "a": 1, "b": 2, "c": [1, 2, 3]
    }


# --------------------------------------------------------------------- #
# Custom condition
# --------------------------------------------------------------------- #


def test_register_python_condition_routes_correctly():
    """Custom condition reads state via state.get and returns a route key."""
    def long_or_short(state):
        msgs = state.get("messages") or []
        return "long" if len(msgs) > 1 else "short"

    ng.ConditionRegistry.register_condition("test_len", long_or_short)

    @ng.node("emit_one")
    def emit_one(state):
        return [ng.ChannelWrite("messages",
            [{"role": "user", "content": "hi"}])]

    @ng.node("on_short")
    def on_short(state):
        return [ng.ChannelWrite("path", "short_path")]

    @ng.node("on_long")
    def on_long(state):
        return [ng.ChannelWrite("path", "long_path")]

    definition = {
        "name": "len_route",
        "channels": {"messages": {"reducer": "append"},
                     "path":     {"reducer": "overwrite"}},
        "nodes": {"emit": {"type": "emit_one"},
                  "s":    {"type": "on_short"},
                  "l":    {"type": "on_long"}},
        "edges": [
            {"from": ng.START_NODE, "to": "emit"},
            {"from": "s", "to": ng.END_NODE},
            {"from": "l", "to": ng.END_NODE},
        ],
        "conditional_edges": [
            {"from": "emit", "condition": "test_len",
             "routes": {"short": "s", "long": "l"}}
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    result = engine.run(ng.RunConfig(thread_id="t1", input={"messages": []}))
    # 1 message after emit → "short" route
    assert "s" in result.execution_trace, result.execution_trace
    assert "l" not in result.execution_trace
    assert result.output["channels"]["path"]["value"] == "short_path"


def test_register_python_condition_sees_state_get_messages():
    """state.get_messages() (typed access) works inside a custom condition."""
    seen_count = []
    def count_messages(state):
        msgs = state.get_messages()
        seen_count.append(len(msgs))
        return "x"   # any string — we just want the call to fire

    ng.ConditionRegistry.register_condition("test_count_msgs", count_messages)

    @ng.node("emit_two")
    def emit_two(state):
        return [ng.ChannelWrite("messages", [
            {"role": "user", "content": "a"},
            {"role": "assistant", "content": "b"},
        ])]

    @ng.node("end_node")
    def end_node(state):
        return []

    definition = {
        "name": "msgs_test",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"emit": {"type": "emit_two"},
                  "x":    {"type": "end_node"}},
        "edges": [
            {"from": ng.START_NODE, "to": "emit"},
            {"from": "x", "to": ng.END_NODE},
        ],
        "conditional_edges": [
            {"from": "emit", "condition": "test_count_msgs",
             "routes": {"x": "x"}},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    engine.run(ng.RunConfig(thread_id="t1", input={"messages": []}))
    assert seen_count and seen_count[0] == 2, seen_count


# --------------------------------------------------------------------- #
# Concurrent invocation
# --------------------------------------------------------------------- #


def test_python_reducer_thread_safe_under_fanout():
    """Multiple Send-spawned siblings invoke the same Python reducer
    concurrently. The GIL acquisition inside the C++ closure must
    serialise correctly — no deadlock, no corruption.
    """
    invocations = []
    invocations_lock = threading.Lock()

    def append_with_log(c, i):
        with invocations_lock:
            invocations.append(i)
        out = list(c) if c else []
        out.append(i)
        return out

    ng.ReducerRegistry.register_reducer("test_concurrent_append", append_with_log)

    class FanoutPlanner(ng.GraphNode):
        def get_name(self): return "planner"
        def execute_full(self, state):
            return ng.NodeResult(
                writes=[],
                sends=[ng.Send("worker", {"i": i}) for i in range(8)],
            )

    class Worker(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def execute(self, state):
            return [ng.ChannelWrite("results", [state.get("i")])]

    ng.NodeFactory.register_type("fanout_planner",
        lambda name, config, ctx: FanoutPlanner())
    ng.NodeFactory.register_type("worker",
        lambda name, config, ctx: Worker(name))

    definition = {
        "name": "concurrent_test",
        "channels": {
            "results": {"reducer": "test_concurrent_append"},
            "i":       {"reducer": "overwrite"},  # Send payload target
        },
        "nodes": {"planner": {"type": "fanout_planner"},
                  "worker":  {"type": "worker"}},
        "edges": [
            {"from": ng.START_NODE, "to": "planner"},
            {"from": "worker",      "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    engine.set_worker_count(8)
    result = engine.run(ng.RunConfig(thread_id="t1", input={"results": []},
                                     max_steps=5))

    # The reducer is `(current, incoming) -> current + [incoming]`, so
    # `final` is a list of incoming values in arrival order. Each worker's
    # write was `[i]` (single-element list), so each reducer call appends
    # `[i]`. Flatten and sort to compare to expected set.
    final = result.output["channels"]["results"]["value"]
    flattened = sorted(v[0] for v in final if v)
    assert flattened == list(range(8)), (final, invocations)
    # Also confirm no GIL deadlock / corruption — every Send fired the reducer
    assert len(invocations) >= 8


# --------------------------------------------------------------------- #
# Sanity: built-ins still work alongside custom registrations
# --------------------------------------------------------------------- #


def test_builtin_reducers_unaffected_by_custom_registration():
    ng.ReducerRegistry.register_reducer("test_unrelated", lambda c, i: i)

    @ng.node("emit")
    def emit(state):
        return [ng.ChannelWrite("xs", [42])]

    definition = {
        "name": "unaffected",
        "channels": {"xs": {"reducer": "append"}},  # built-in
        "nodes": {"emit": {"type": "emit"}},
        "edges": [
            {"from": ng.START_NODE, "to": "emit"},
            {"from": "emit", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    result = engine.run(ng.RunConfig(thread_id="t1", input={"xs": []}))
    assert result.output["channels"]["xs"]["value"] == [42]
