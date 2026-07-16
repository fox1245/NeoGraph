"""Async surface — commit 5 of the binding.

Covers:
  - basic `await engine.run_async(cfg)` round-trip
  - exception propagation from a Python custom node into the awaiter
  - `asyncio.gather` of N concurrent run_async calls (overlap check)
  - run_stream_async with an async-friendly callback
  - resume_async after a run that interrupted on a Python node
  - cancel-during-run: future cancel must not raise InvalidStateError
    when the worker completes after cancellation (cost-leak guard)
"""

import asyncio
import threading
import time

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


def _traceback_function_names(exc):
    names = []
    tb = exc.__traceback__
    while tb is not None:
        names.append(tb.tb_frame.f_code.co_name)
        tb = tb.tb_next
    return names


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
        def run(self, input):
            state = input.state
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
        def run(self, input):
            state = input.state
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
        def run(self, input):
            state = input.state
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


def test_run_async_cancel_does_not_double_resolve_future():
    """Cancelling an in-flight run_async future must not raise
    InvalidStateError when the C++ worker finishes after the cancel.

    The textbook trigger: a FastAPI SSE handler driven by a frontend
    AbortController that fires every keystroke (300 ms debounce). The
    asyncio task gets cancelled, but the engine's worker thread keeps
    running; when the worker's completion lambda tries to
    `loop.call_soon_threadsafe(fut.set_result, …)`, the now-cancelled
    future raises InvalidStateError on the loop thread, where no C++
    try/except can catch it. asyncio's default handler logs it, and
    under a typing UI the log fills with the same trace many times a
    minute.

    The bridge wraps set_result/set_exception in `_safe_set_future_*`
    helpers that `if not fut.done():` guard before calling. This test
    captures asyncio loop exceptions and asserts none of them are
    InvalidStateError after a cancel-during-run.
    """
    type_name = _next_type("async_sleepy")

    class SleepyNode(neograph.GraphNode):
        # time.sleep (not asyncio.sleep) — execute() is synchronous,
        # runs on the engine's worker thread, releases GIL while
        # sleeping. The duration must outlive the test's cancel-point
        # so the worker is still running when we cancel the future.
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def run(self, input):
            state = input.state
            time.sleep(0.4)
            return [neograph.ChannelWrite("done", True)]

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: SleepyNode(name))

    definition = {
        "name": "sleepy",
        "channels": {"done": {"reducer": "overwrite"}},
        "nodes": {"s": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "s"},
            {"from": "s", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())

    async def go():
        loop = asyncio.get_running_loop()
        captured: list[dict] = []
        loop.set_exception_handler(lambda lp, ctx: captured.append(ctx))

        cfg = neograph.RunConfig(thread_id="cancel-1", input={})
        fut = engine.run_async(cfg)

        # Let the worker start the SleepyNode's time.sleep.
        await asyncio.sleep(0.05)

        # Cancel before the worker finishes. The worker keeps running
        # — that's the bug shape we're guarding against.
        fut.cancel()
        with pytest.raises(asyncio.CancelledError):
            await fut

        # Wait long enough for the worker to finish its sleep and
        # invoke the completion lambda. Pre-fix, this is when
        # InvalidStateError would surface on the loop.
        await asyncio.sleep(0.6)

        return captured

    captured = asyncio.run(go())
    invalid_state = [
        c for c in captured
        if "InvalidStateError" in repr(c.get("exception"))
        or "invalid state" in str(c.get("message", "")).lower()
    ]
    assert not invalid_state, (
        f"future double-resolve raised InvalidStateError after "
        f"cancel — _safe_set_future_* guard not in effect: "
        f"{invalid_state}")


def test_run_stream_async_cancel_does_not_double_resolve_future():
    """Same cancel-during-run guard for the streaming surface.

    run_stream_async has its own completion lambda (separate from
    run_async); the safe-resolve helpers must cover it too.
    """
    type_name = _next_type("async_stream_sleepy")

    class StreamingSleepyNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def run(self, input):
            state = input.state
            time.sleep(0.4)
            return [neograph.ChannelWrite("done", True)]

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: StreamingSleepyNode(name))

    definition = {
        "name": "stream_sleepy",
        "channels": {"done": {"reducer": "overwrite"}},
        "nodes": {"s": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "s"},
            {"from": "s", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())

    async def go():
        loop = asyncio.get_running_loop()
        captured: list[dict] = []
        loop.set_exception_handler(lambda lp, ctx: captured.append(ctx))

        def cb(ev):
            pass

        cfg = neograph.RunConfig(thread_id="cancel-stream-1", input={})
        fut = engine.run_stream_async(cfg, cb)

        await asyncio.sleep(0.05)
        fut.cancel()
        with pytest.raises(asyncio.CancelledError):
            await fut
        await asyncio.sleep(0.6)
        return captured

    captured = asyncio.run(go())
    invalid_state = [
        c for c in captured
        if "InvalidStateError" in repr(c.get("exception"))
        or "invalid state" in str(c.get("message", "")).lower()
    ]
    assert not invalid_state, (
        f"future double-resolve raised InvalidStateError after "
        f"cancel on streaming path: {invalid_state}")


@pytest.mark.parametrize("async_method", ["run_async", "run_stream_async"])
def test_async_run_api_preserves_python_node_exception(async_method):
    """Each async run API exposes the original Python exception."""
    type_name = _next_type("async_explode")

    class CustomNodeError(Exception):
        def __init__(self, message, marker):
            super().__init__(message, marker)
            self.marker = marker

    error = CustomNodeError("kaboom from python node", 41)

    def raise_custom_error():
        raise error

    class ExplodeNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def run(self, input):
            state = input.state
            raise_custom_error()

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
    cfg = neograph.RunConfig(thread_id="sync-error", input={"x": 1})

    with pytest.raises(CustomNodeError) as sync_info:
        engine.run(cfg)

    sync_exc = sync_info.value
    sync_frames = _traceback_function_names(sync_exc)
    assert sync_exc is error
    assert type(sync_exc) is CustomNodeError
    assert sync_exc.args == ("kaboom from python node", 41)
    assert sync_exc.marker == 41
    assert sync_frames[-2:] == ["run", "raise_custom_error"]

    # Raising the same instance again otherwise retains the earlier traceback.
    error.__traceback__ = None

    async def go():
        cfg = neograph.RunConfig(
            thread_id=f"{async_method}-error", input={"x": 1})
        with pytest.raises(CustomNodeError) as async_info:
            if async_method == "run_stream_async":
                await engine.run_stream_async(cfg, lambda _event: None)
            else:
                await engine.run_async(cfg)
        return async_info.value

    async_exc = asyncio.run(go())
    assert async_exc is error
    assert type(async_exc) is type(sync_exc)
    assert async_exc.args == sync_exc.args
    assert async_exc.marker == sync_exc.marker
    assert _traceback_function_names(async_exc)[-2:] == sync_frames[-2:]


def test_resume_async_preserves_python_node_exception():
    """An exception raised after resuming keeps its object and traceback."""
    type_name = _next_type("async_resume_explode")

    class CustomResumeError(Exception):
        def __init__(self, message, marker):
            super().__init__(message, marker)
            self.marker = marker

    error = CustomResumeError("kaboom after resume", 42)

    def raise_resume_error():
        raise error

    class InterruptThenExplodeNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def run(self, input):
            if input.ctx.resume_value is None:
                raise neograph.NodeInterrupt("continue into failure")
            raise_resume_error()

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: InterruptThenExplodeNode(name))

    definition = {
        "name": "resume_explode",
        "channels": {},
        "nodes": {"gate": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "gate"},
            {"from": "gate", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(
        definition, neograph.NodeContext(),
        neograph.InMemoryCheckpointStore())
    cfg = neograph.RunConfig(thread_id="resume-error", input={})
    assert engine.run(cfg).interrupted

    async def go():
        with pytest.raises(CustomResumeError) as resume_info:
            await engine.resume_async("resume-error", {"approved": True})
        return resume_info.value

    resume_exc = asyncio.run(go())
    assert resume_exc is error
    assert type(resume_exc) is CustomResumeError
    assert resume_exc.args == ("kaboom after resume", 42)
    assert resume_exc.marker == 42
    assert _traceback_function_names(resume_exc)[-2:] == [
        "run", "raise_resume_error"]


def test_run_async_invalid_node_return_matches_sync_type_error():
    """py::type_error from node coercion stays TypeError in async runs."""
    type_name = _next_type("async_invalid_return")

    class InvalidReturnNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name
        def get_name(self): return self._name
        def run(self, input):
            return object()

    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: InvalidReturnNode(name))

    definition = {
        "name": "invalid_return",
        "channels": {},
        "nodes": {"bad": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "bad"},
            {"from": "bad", "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())

    with pytest.raises(TypeError) as sync_info:
        engine.run(neograph.RunConfig(thread_id="sync-type-error", input={}))

    async def go():
        with pytest.raises(TypeError) as async_info:
            await engine.run_async(neograph.RunConfig(
                thread_id="async-type-error", input={}))
        return async_info.value

    async_exc = asyncio.run(go())
    assert type(async_exc) is type(sync_info.value)
    assert async_exc.args == sync_info.value.args
