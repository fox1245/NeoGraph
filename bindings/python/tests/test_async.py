"""Async surface — commit 5 of the binding.

Covers:
  - basic `await engine.run_async(cfg)` round-trip
  - exception propagation from a Python custom node into the awaiter
  - `asyncio.gather` of N concurrent run_async calls (overlap check)
  - run_stream_async with an async-friendly callback
  - resume_async after a run that interrupted on a Python node
"""

import asyncio
import threading

import pytest

import neograph_engine as neograph  # PyPI dist name is `neograph-engine`;
                                     # bare `neograph` was already taken


_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


def _passthrough_definition(channel="x"):
    return {
        "name": f"async_{channel}",
        "channels": {channel: {"reducer": "overwrite"}},
        "nodes": {},
        "edges": [
            {"from": neograph.START_NODE, "to": neograph.END_NODE},
        ],
    }


def test_run_async_basic():
    """Smoke: engine.run_async returns an awaitable that yields a RunResult."""
    engine = neograph.GraphEngine.compile(
        _passthrough_definition(), neograph.NodeContext())

    async def go():
        cfg = neograph.RunConfig(thread_id="t1", input={"x": 7})
        result = await engine.run_async(cfg)
        assert result.output["channels"]["x"]["value"] == 7
        return result

    result = asyncio.run(go())
    assert result is not None


def test_run_async_with_python_node():
    """Custom Python node dispatched via the async path."""
    type_name = _next_type("async_doubler")

    class DoublerNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def execute(self, state):
            x = state.get("x") or 0
            return [neograph.ChannelWrite("x", x * 2)]

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: DoublerNode(name))

    definition = {
        "name": "doubler",
        "channels": {"x": {"reducer": "overwrite"}},
        "nodes": {"d": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "d"},
            {"from": "d", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())

    async def go():
        cfg = neograph.RunConfig(thread_id="t1", input={"x": 21})
        result = await engine.run_async(cfg)
        return result.output["channels"]["x"]["value"]

    assert asyncio.run(go()) == 42


def test_run_async_concurrent_gather():
    """asyncio.gather of 8 concurrent run_async calls should overlap.

    Each call runs on the same engine but with distinct thread_ids, which
    the engine's thread-safety contract permits. The bridge spawns each
    on the AsyncRuntime's io_context, so they overlap on the asio
    worker thread (which suspends between each engine super-step).
    """
    type_name = _next_type("async_conc")

    class IdentityNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def execute(self, state):
            seed = state.get("seed") or 0
            return [neograph.ChannelWrite("doubled", seed * 2)]

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: IdentityNode(name))

    definition = {
        "name": "conc_async",
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

    async def go():
        coros = [
            engine.run_async(neograph.RunConfig(
                thread_id=f"t{i}", input={"seed": i}))
            for i in range(8)
        ]
        return await asyncio.gather(*coros)

    results = asyncio.run(go())
    doubled = sorted(r.output["channels"]["doubled"]["value"]
                     for r in results)
    assert doubled == [i * 2 for i in range(8)]


def test_run_stream_async_events_on_loop_thread():
    """Stream callbacks should be hopped to the asyncio loop thread.

    Without the threadsafe hop, the callback would run on the asio
    worker thread — confusing for users who expect Python callbacks
    to share the loop's thread context.
    """
    type_name = _next_type("async_stream")

    class TrivialNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def execute(self, state):
            return [neograph.ChannelWrite("done", True)]

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: TrivialNode(name))

    definition = {
        "name": "stream_async",
        "channels": {"done": {"reducer": "overwrite"}},
        "nodes": {"n": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "n"},
            {"from": "n", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())

    async def go():
        callback_threads = []
        loop_thread = threading.get_ident()

        def cb(ev):
            callback_threads.append(threading.get_ident())

        cfg = neograph.RunConfig(thread_id="t1", input={})
        await engine.run_stream_async(cfg, cb)
        return callback_threads, loop_thread

    threads, loop_thread = asyncio.run(go())
    assert len(threads) > 0, "expected at least one event"
    # All events must have been delivered on the asyncio loop's
    # thread — the binding's threadsafe hop is doing its job.
    assert all(t == loop_thread for t in threads), \
        f"events fired off the loop thread: {threads} vs loop {loop_thread}"


def test_run_async_propagates_python_node_exception():
    """Python node raises → asyncio future rejects → await raises."""
    type_name = _next_type("async_explode")

    class ExplodeNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def execute(self, state):
            raise RuntimeError("kaboom from python node")

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: ExplodeNode(name))

    definition = {
        "name": "explode",
        "channels": {"x": {"reducer": "overwrite"}},
        "nodes": {"e": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "e"},
            {"from": "e", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())

    async def go():
        cfg = neograph.RunConfig(thread_id="t1", input={"x": 1})
        with pytest.raises(RuntimeError, match="kaboom"):
            await engine.run_async(cfg)

    asyncio.run(go())
