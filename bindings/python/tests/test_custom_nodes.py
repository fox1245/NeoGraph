"""Custom Python GraphNode subclassing — commit 2 of the binding.

Covers:
  - Subclassing neograph.GraphNode with override of execute()
  - NodeFactory.register_type wiring a Python factory
  - Round-tripping ChannelWrite emissions through the engine
  - execute_full() returning Command (routing override)
  - execute_full() returning Send list (dynamic fan-out)
  - @neograph.node decorator sugar for write-only nodes
"""

import threading

import neograph_engine as neograph  # PyPI dist name is `neograph-engine`;
                                     # bare `neograph` was already taken


# Each test registers a unique type name so the global NodeFactory
# state doesn't collide across tests when run with pytest -x.
_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


def test_simple_counter_node_runs_in_sequence():
    """A trivial subclass that increments a channel."""
    type_name = _next_type("counter_seq")
    calls = []

    class CounterNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            state = input.state
            calls.append(self._name)
            current = state.get("count") or 0
            return [neograph.ChannelWrite("count", current + 1)]

    neograph.NodeFactory.register_type(
        type_name,
        lambda name, config, ctx: CounterNode(name),
    )

    definition = {
        "name": "counter_seq_graph",
        "channels": {"count": {"reducer": "overwrite"}},
        "nodes": {
            "step1": {"type": type_name},
            "step2": {"type": type_name},
        },
        "edges": [
            {"from": neograph.START_NODE, "to": "step1"},
            {"from": "step1",             "to": "step2"},
            {"from": "step2",             "to": neograph.END_NODE},
        ],
    }

    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    result = engine.run(neograph.RunConfig(thread_id="t1", input={"count": 0}))

    # Engine called both nodes, in order, exactly once each.
    assert calls == ["step1", "step2"]
    # Final value is 2 after two increments.
    final_count = result.output["channels"]["count"]["value"]
    assert final_count == 2


def test_graph_state_methods_visible_to_python():
    """The GraphState passed into execute() exposes get / channel_names / etc."""
    type_name = _next_type("inspect")
    seen = {}

    class InspectNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            state = input.state
            seen["channels"] = sorted(state.channel_names())
            seen["greeting"] = state.get("greeting")
            seen["missing"] = state.get("nonexistent_channel")
            return [neograph.ChannelWrite("greeting", "world")]

    neograph.NodeFactory.register_type(
        type_name,
        lambda name, config, ctx: InspectNode(name),
    )

    definition = {
        "name": "inspect",
        "channels": {"greeting": {"reducer": "overwrite"}},
        "nodes": {"x": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "x"},
            {"from": "x", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    engine.run(neograph.RunConfig(thread_id="t1", input={"greeting": "hello"}))

    assert seen["greeting"] == "hello"
    assert seen["missing"] is None
    assert "greeting" in seen["channels"]


def test_command_routing_override():
    """execute_full() can return a Command to override edge-based routing."""
    type_name_router = _next_type("router")
    type_name_taken = _next_type("taken")
    type_name_skipped = _next_type("skipped")
    visits = []

    class RouterNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            state = input.state
            visits.append(self._name)
            # Override default routing to land on the taken node
            # instead of whatever the JSON edges would dispatch to.
            return neograph.Command(
                goto_node="taken",
                updates=[neograph.ChannelWrite("status", "rerouted")],
            )

    class StubNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            state = input.state
            visits.append(self._name)
            return []

    neograph.NodeFactory.register_type(
        type_name_router,  lambda name, c, ctx: RouterNode(name))
    neograph.NodeFactory.register_type(
        type_name_taken,   lambda name, c, ctx: StubNode(name))
    neograph.NodeFactory.register_type(
        type_name_skipped, lambda name, c, ctx: StubNode(name))

    definition = {
        "name": "router_graph",
        "channels": {"status": {"reducer": "overwrite"}},
        "nodes": {
            "router":  {"type": type_name_router},
            "taken":   {"type": type_name_taken},
            "skipped": {"type": type_name_skipped},
        },
        "edges": [
            {"from": neograph.START_NODE, "to": "router"},
            # Without the Command override, the engine would dispatch
            # the edge → skipped path. The Command tells it "go to
            # taken instead".
            {"from": "router",  "to": "skipped"},
            {"from": "taken",   "to": neograph.END_NODE},
            {"from": "skipped", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    result = engine.run(neograph.RunConfig(thread_id="t1", input={}))

    # The router emitted Command{goto: taken}; engine should have
    # taken that path.
    assert "taken" in visits
    assert "skipped" not in visits
    # The state update from the Command applied.
    final_status = result.output["channels"]["status"]["value"]
    assert final_status == "rerouted"


def test_send_fan_out():
    """execute_full() can return Sends for dynamic fan-out."""
    type_name_fanout = _next_type("fanout")
    type_name_worker = _next_type("worker")

    class FanOutNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            state = input.state
            # Three siblings, each handed a different item.
            return [
                neograph.Send("worker", {"item": "a"}),
                neograph.Send("worker", {"item": "b"}),
                neograph.Send("worker", {"item": "c"}),
            ]

    class WorkerNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            state = input.state
            item = state.get("item")
            # Append to results channel.
            return [neograph.ChannelWrite("results", [item])]

    neograph.NodeFactory.register_type(
        type_name_fanout, lambda name, c, ctx: FanOutNode(name))
    neograph.NodeFactory.register_type(
        type_name_worker, lambda name, c, ctx: WorkerNode(name))

    definition = {
        "name": "fanout_graph",
        "channels": {
            "item":    {"reducer": "overwrite"},
            "results": {"reducer": "append"},
        },
        "nodes": {
            "fan":    {"type": type_name_fanout},
            "worker": {"type": type_name_worker},
        },
        "edges": [
            {"from": neograph.START_NODE, "to": "fan"},
            {"from": "worker", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    result = engine.run(neograph.RunConfig(thread_id="t1", input={}))

    final_results = result.output["channels"]["results"]["value"]
    # All three items processed; order is non-deterministic under
    # parallel fan-out so just check membership.
    assert sorted(final_results) == ["a", "b", "c"]


def test_decorator_sugar():
    """@neograph.node wraps a plain function into a GraphNode."""
    type_name = _next_type("decorated")

    @neograph.node(type_name)
    def hello_node(state):
        return [neograph.ChannelWrite("greeting", "hi from decorator")]

    definition = {
        "name": "decorated_graph",
        "channels": {"greeting": {"reducer": "overwrite"}},
        "nodes": {"x": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "x"},
            {"from": "x", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    result = engine.run(neograph.RunConfig(thread_id="t1", input={}))

    final = result.output["channels"]["greeting"]["value"]
    assert final == "hi from decorator"


def test_send_fan_out_with_worker_pool():
    """Send fan-out + set_worker_count() — trampoline must be GIL-safe
    when multiple C++ worker threads dispatch into Python in parallel.

    Without GIL handling this would deadlock or crash on
    PyEval_AcquireLock contention.
    """
    type_name_fanout = _next_type("fanout_pool")
    type_name_worker = _next_type("worker_pool")

    class FanOutNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self):
            return self._name
        def run(self, input):
            state = input.state
            return [neograph.Send("worker", {"item": i}) for i in range(8)]

    class WorkerNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self):
            return self._name
        def run(self, input):
            state = input.state
            n = state.get("item")
            return [neograph.ChannelWrite("results", [n * n])]

    neograph.NodeFactory.register_type(
        type_name_fanout, lambda name, c, ctx: FanOutNode(name))
    neograph.NodeFactory.register_type(
        type_name_worker, lambda name, c, ctx: WorkerNode(name))

    definition = {
        "name": "fanout_pool",
        "channels": {
            "item":    {"reducer": "overwrite"},
            "results": {"reducer": "append"},
        },
        "nodes": {
            "fan":    {"type": type_name_fanout},
            "worker": {"type": type_name_worker},
        },
        "edges": [
            {"from": neograph.START_NODE, "to": "fan"},
            {"from": "worker", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    engine.set_worker_count(4)  # real CPU parallelism for fan-out
    result = engine.run(neograph.RunConfig(thread_id="t1", input={}))

    final_results = result.output["channels"]["results"]["value"]
    assert sorted(final_results) == [0, 1, 4, 9, 16, 25, 36, 49]


def test_concurrent_runs_with_python_node():
    """Confirm the GIL handling holds up under N concurrent run() calls.

    Each run() releases the GIL inside C++ and re-acquires only when
    dispatching into the Python execute() body. Multiple runs at once
    must therefore not deadlock and not interleave channel state
    across thread_ids.
    """
    type_name = _next_type("conc")

    class WorkNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            state = input.state
            seed = state.get("seed") or 0
            return [neograph.ChannelWrite("doubled", seed * 2)]

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: WorkNode(name))

    definition = {
        "name": "conc_graph",
        "channels": {
            "seed":    {"reducer": "overwrite"},
            "doubled": {"reducer": "overwrite"},
        },
        "nodes": {"w": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "w"},
            {"from": "w", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())

    results = {}
    errors = []

    def run_one(seed):
        try:
            r = engine.run(neograph.RunConfig(
                thread_id=f"t{seed}", input={"seed": seed}))
            results[seed] = r.output["channels"]["doubled"]["value"]
        except Exception as e:
            errors.append((seed, e))

    threads = [threading.Thread(target=run_one, args=(i,))
               for i in range(8)]
    for t in threads: t.start()
    for t in threads: t.join()

    assert errors == [], f"concurrent runs raised: {errors}"
    assert results == {i: i * 2 for i in range(8)}
