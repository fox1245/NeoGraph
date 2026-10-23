"""Every run, stream and resume entry point takes the same per-run values.

``RunMetadata`` (deadline, trace and run identity, owner scope) used to reach
``run`` and ``resume_from`` only. ``run_stream``, ``run_async``,
``run_stream_async``, ``resume`` and ``resume_async`` ignored it, so the
asyncio and streaming paths a web service uses could not carry a deadline or
an identity. The resume entry points also took no stream callback, so a turn
that continued after a human approval could not stream its tokens, and
``resume_async`` did not stop when its Future was cancelled.

These tests drive each entry point the way a caller does and look at what a
node, a stream callback and a checkpoint Hook observe.
"""

import asyncio
import sys
import threading
import time

import pytest

import neograph_engine as ng
from neograph_engine.streaming import emit_token


RUN_ENTRIES = ["run", "run_stream", "run_async", "run_stream_async"]
RESUME_ENTRIES = ["resume", "resume_from", "resume_async", "resume_from_async"]
ALL_ENTRIES = RUN_ENTRIES + RESUME_ENTRIES
ASYNC_ENTRIES = ["run_async", "run_stream_async", "resume_async", "resume_from_async"]
TIMEOUT_MS = 600_000

_lock = threading.Lock()
_seen = {}


def _record(thread_id, **fields):
    with _lock:
        _seen.setdefault(thread_id, []).append(fields)


def _seen_by(thread_id):
    with _lock:
        return list(_seen.get(thread_id, []))


class _Probe(ng.GraphNode):
    """Records what the engine tells a node about its run and streams two tokens.

    With ``ask`` the node pauses the run until a resume answers it, so the
    resume entry points have something to continue.
    """

    def __init__(self, name, ask):
        super().__init__()
        self._name = name
        self._ask = ask

    def get_name(self):
        return self._name

    def run(self, input):
        ctx = input.ctx
        if self._ask and ctx.resume_value is None:
            raise ng.NodeInterrupt({"asks": self._name}, reason="approve")
        _record(ctx.thread_id, run_id=ctx.run_id, trace_id=ctx.trace_id,
                has_deadline=ctx.has_deadline, remaining_ms=ctx.deadline_remaining_ms,
                resume_value=ctx.resume_value, streamed=input.stream_cb is not None)
        if input.stream_cb:
            for chunk in ("hel", "lo"):
                emit_token(input.stream_cb, self._name, chunk)
        return [ng.ChannelWrite("answer", "done")]


class _Hold(ng.GraphNode):
    """After the answer, waits until its run is cancelled, for at most three seconds."""

    def get_name(self):
        return "work"

    def run(self, input):
        ctx = input.ctx
        if ctx.resume_value is None:
            raise ng.NodeInterrupt({"asks": "work"}, reason="approve")
        token = ctx.cancel_token
        _record(ctx.thread_id, phase="holding")
        limit = time.monotonic() + 3.0
        while time.monotonic() < limit and not (token is not None and token.is_cancelled()):
            time.sleep(0.005)
        _record(ctx.thread_id, phase="released", has_token=token is not None,
                saw_cancel=token is not None and token.is_cancelled())
        return [ng.ChannelWrite("answer", "released")]


ng.NodeFactory.register_type("entrypoints.probe", lambda name, _c, _x: _Probe(name, False))
ng.NodeFactory.register_type("entrypoints.ask", lambda name, _c, _x: _Probe(name, True))
ng.NodeFactory.register_type("entrypoints.hold", lambda name, _c, _x: _Hold())


def _owner_hook(owner, fired):
    """A checkpoint Hook that runs only for events of one owner scope."""
    data = ng.HookDefinitionData()
    data.phase = ng.HookPhase.CheckpointPublished
    data.target_id = "owner"
    data.delivery = ng.HookDelivery.BlockingMandatory
    data.failure_mode = ng.HookFailureMode.FailClosed
    data.idempotency = ng.HookIdempotency.Idempotent
    data.effect = ng.ToolEffectClass.ReadOnly
    predicate = ng.HookPredicate()
    predicate.kind = ng.HookPredicateKind.Equals
    predicate.field = ng.RuntimeEventField.OwnerScope
    predicate.value = owner
    data.predicate = predicate
    mapper = ng.HookInputMapper()
    mapper.kind = ng.HookInputMapperKind.Template
    mapper.value_template = {"kind": "checkpoint"}
    data.input_mapper = mapper
    return ng.create_hook_runtime(
        [ng.HookDefinition.create(data)], {"owner": lambda *_args: fired.append(owner)})


def _engine(node_type, hook=None):
    definition = {
        "name": "run_entrypoints",
        "channels": {"answer": {"reducer": "overwrite"}},
        "nodes": {"work": {"type": node_type}},
        "edges": [
            {"from": ng.START_NODE, "to": "work"},
            {"from": "work", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(), ng.InMemoryCheckpointStore())
    if hook is not None:
        engine.set_hook_runtime(hook)
    return engine


def _metadata(case):
    return ng.RunMetadata(timeout_ms=TIMEOUT_MS, trace_id=f"trace-{case}",
                          run_id=f"run-{case}", owner_scope=f"tenant-{case}")


def _call(entry, engine, thread_id, checkpoint_id=None, metadata=None, callback=None, **options):
    """Return the awaitable (async entry points) or the result (sync ones).

    ``options`` (for example ``interrupt_id``) go to the resume entry points only.
    """
    config = ng.RunConfig(thread_id=thread_id)
    extra = {} if metadata is None else {"metadata": metadata}
    if entry == "run":
        return engine.run(config, **extra)
    if entry == "run_stream":
        return engine.run_stream(config, callback or (lambda _event: None), **extra)
    if entry == "run_async":
        return engine.run_async(config, **extra)
    if entry == "run_stream_async":
        return engine.run_stream_async(config, callback or (lambda _event: None), **extra)
    if callback is not None:
        extra["callback"] = callback
    extra.update(options)
    if entry == "resume":
        return engine.resume(thread_id, "yes", **extra)
    if entry == "resume_async":
        return engine.resume_async(thread_id, "yes", **extra)
    if entry == "resume_from":
        return engine.resume_from(config, checkpoint_id, "yes", **extra)
    if entry == "resume_from_async":
        return engine.resume_from_async(config, checkpoint_id, "yes", **extra)
    raise AssertionError(entry)


def _drive(entry, engine, thread_id, metadata=None, callback=None, **options):
    """Run one entry point to completion; a resume entry first pauses the run."""
    checkpoint_id = None
    if entry in RESUME_ENTRIES:
        # A Hook runtime refuses an event without an owner scope and a run id, so the run that pauses has both.
        setup = ng.RunMetadata(run_id=f"setup-{thread_id}", owner_scope="setup")
        paused = engine.run(ng.RunConfig(thread_id=thread_id), setup)
        assert paused.interrupted
        checkpoint_id = paused.checkpoint_id
    if entry in ASYNC_ENTRIES:
        async def go():
            return await _call(entry, engine, thread_id, checkpoint_id, metadata, callback, **options)
        return asyncio.run(go())
    return _call(entry, engine, thread_id, checkpoint_id, metadata, callback, **options)


def _node_type(entry):
    return "entrypoints.ask" if entry in RESUME_ENTRIES else "entrypoints.probe"


@pytest.mark.parametrize("entry", ALL_ENTRIES)
def test_metadata_reaches_the_entry_point(entry):
    fired = []
    engine = _engine(_node_type(entry), _owner_hook(f"tenant-{entry}", fired))
    thread_id = f"metadata-{entry}"

    result = _drive(entry, engine, thread_id, _metadata(entry))

    assert not result.interrupted
    (seen,) = _seen_by(thread_id)
    assert seen["run_id"] == f"run-{entry}"
    assert seen["trace_id"] == f"trace-{entry}"
    assert seen["has_deadline"]
    assert 0 < seen["remaining_ms"] <= TIMEOUT_MS
    assert fired, "the owner scope never reached the checkpoint Hook"


@pytest.mark.parametrize("entry", ALL_ENTRIES)
def test_an_entry_point_without_metadata_runs_with_the_defaults(entry):
    engine = _engine(_node_type(entry))
    thread_id = f"defaults-{entry}"

    result = _drive(entry, engine, thread_id)

    assert not result.interrupted
    (seen,) = _seen_by(thread_id)
    assert seen["run_id"] == "" and seen["trace_id"] == ""
    assert not seen["has_deadline"] and seen["remaining_ms"] is None


def test_concurrent_async_runs_keep_their_own_metadata():
    """One RunMetadata object, changed between calls, never leaks into a run in flight."""
    engine = _engine("entrypoints.probe")
    count = 8

    async def go():
        shared = ng.RunMetadata()
        futures = []
        for i in range(count):
            shared.trace_id = f"trace-{i}"
            shared.run_id = f"run-{i}"
            shared.owner_scope = f"tenant-{i}"
            shared.set_timeout_ms(TIMEOUT_MS + i * 100_000)
            config = ng.RunConfig(thread_id=f"concurrent-run-{i}")
            if i % 2:
                futures.append(engine.run_stream_async(config, lambda _event: None, metadata=shared))
            else:
                futures.append(engine.run_async(config, metadata=shared))
        shared.trace_id = "mutated-after-the-calls"
        return await asyncio.gather(*futures)

    results = asyncio.run(go())

    assert not any(result.interrupted for result in results)
    for i in range(count):
        (seen,) = _seen_by(f"concurrent-run-{i}")
        assert seen["run_id"] == f"run-{i}"
        assert seen["trace_id"] == f"trace-{i}"
        assert abs(seen["remaining_ms"] - (TIMEOUT_MS + i * 100_000)) < 20_000


def test_concurrent_async_resumes_keep_their_own_metadata():
    engine = _engine("entrypoints.ask")
    count = 6
    checkpoints = []
    for i in range(count):
        paused = engine.run(ng.RunConfig(thread_id=f"concurrent-resume-{i}"))
        assert paused.interrupted
        checkpoints.append(paused.checkpoint_id)

    async def go():
        shared = ng.RunMetadata()
        futures = []
        for i in range(count):
            shared.trace_id = f"trace-{i}"
            shared.run_id = f"run-{i}"
            shared.set_timeout_ms(TIMEOUT_MS + i * 100_000)
            thread_id = f"concurrent-resume-{i}"
            if i % 2:
                config = ng.RunConfig(thread_id=thread_id)
                futures.append(engine.resume_from_async(config, checkpoints[i], "yes", metadata=shared))
            else:
                futures.append(engine.resume_async(thread_id, "yes", metadata=shared))
        shared.trace_id = "mutated-after-the-calls"
        return await asyncio.gather(*futures)

    results = asyncio.run(go())

    assert not any(result.interrupted for result in results)
    for i in range(count):
        (seen,) = _seen_by(f"concurrent-resume-{i}")
        assert seen["run_id"] == f"run-{i}"
        assert seen["trace_id"] == f"trace-{i}"
        assert abs(seen["remaining_ms"] - (TIMEOUT_MS + i * 100_000)) < 20_000


@pytest.mark.parametrize("entry", RESUME_ENTRIES)
def test_a_resumed_turn_streams_its_events(entry):
    engine = _engine("entrypoints.ask")
    thread_id = f"stream-{entry}"
    events = []
    caller = threading.get_ident()

    def collect(event):
        events.append((event, threading.get_ident()))

    result = _drive(entry, engine, thread_id, callback=collect)

    assert not result.interrupted
    types = ng.GraphEvent.Type
    tokens = [event.data for event, _thread in events if event.type == types.LLM_TOKEN]
    assert tokens == ["hel", "lo"]
    assert any(event.type == types.NODE_START and event.node_name == "work"
               for event, _thread in events)
    assert any(event.type == types.NODE_END and event.node_name == "work"
               for event, _thread in events)
    (seen,) = _seen_by(thread_id)
    assert seen["streamed"]
    if entry in ASYNC_ENTRIES:
        assert {thread for _event, thread in events} == {caller}, \
            "an asyncio entry point delivers events on the loop thread"


@pytest.mark.parametrize("entry", RESUME_ENTRIES)
def test_a_resume_without_a_callback_streams_nothing(entry):
    engine = _engine("entrypoints.ask")
    thread_id = f"silent-{entry}"

    result = _drive(entry, engine, thread_id)

    assert not result.interrupted
    (seen,) = _seen_by(thread_id)
    assert not seen["streamed"]


@pytest.mark.parametrize("entry", ["run_stream", "resume", "resume_from"])
def test_a_failing_callback_is_reported_and_does_not_abort_a_sync_run(entry, monkeypatch):
    engine = _engine(_node_type(entry))
    reported = []
    monkeypatch.setattr(sys, "unraisablehook", reported.append)

    def explode(_event):
        raise RuntimeError("callback exploded")

    result = _drive(entry, engine, f"explode-{entry}", callback=explode)

    assert not result.interrupted
    assert result.output["channels"]["answer"]["value"] == "done"
    assert reported, "the exception of the callback was dropped without a trace"
    assert all(isinstance(item.exc_value, RuntimeError) for item in reported)
    assert all("callback exploded" in str(item.exc_value) for item in reported)
    assert all(item.object is explode for item in reported)


@pytest.mark.parametrize("entry", ["run_stream_async", "resume_async", "resume_from_async"])
def test_a_failing_callback_is_left_to_the_loop_and_does_not_abort_an_asyncio_run(entry):
    engine = _engine(_node_type(entry))
    reported = []

    def explode(_event):
        raise RuntimeError("callback exploded")

    async def go():
        asyncio.get_running_loop().set_exception_handler(lambda _loop, context: reported.append(context))
        checkpoint_id = None
        if entry != "run_stream_async":
            setup = ng.RunMetadata(run_id="setup", owner_scope="setup")
            paused = engine.run(ng.RunConfig(thread_id=f"loop-explode-{entry}"), setup)
            checkpoint_id = paused.checkpoint_id
        result = await _call(entry, engine, f"loop-explode-{entry}", checkpoint_id, callback=explode)
        await asyncio.sleep(0.05)
        return result

    result = asyncio.run(go())

    assert not result.interrupted
    assert result.output["channels"]["answer"]["value"] == "done"
    assert reported and all("callback exploded" in repr(item.get("exception")) for item in reported)


def test_a_callback_that_exits_does_not_end_the_process(monkeypatch):
    """SystemExit in a callback is reported like any other exception; it never reaches Py_Exit."""
    engine = _engine("entrypoints.probe")
    reported = []
    monkeypatch.setattr(sys, "unraisablehook", reported.append)

    result = engine.run_stream(ng.RunConfig(thread_id="callback-exit"), lambda _event: sys.exit(3))

    assert not result.interrupted
    assert reported and all(item.exc_type is SystemExit for item in reported)


@pytest.mark.parametrize("entry", ["resume_async", "resume_from_async"])
def test_cancelling_the_future_of_an_async_resume_cancels_the_run(entry):
    engine = _engine("entrypoints.hold")
    thread_id = f"cancel-{entry}"
    paused = engine.run(ng.RunConfig(thread_id=thread_id))
    assert paused.interrupted

    async def go():
        loop = asyncio.get_running_loop()
        problems = []
        loop.set_exception_handler(lambda _loop, context: problems.append(context))
        future = _call(entry, engine, thread_id, paused.checkpoint_id)
        for _ in range(400):
            if any(item.get("phase") == "holding" for item in _seen_by(thread_id)):
                break
            await asyncio.sleep(0.01)
        else:
            raise AssertionError("the resumed node never started")
        future.cancel()
        for _ in range(400):
            if any(item.get("phase") == "released" for item in _seen_by(thread_id)):
                break
            await asyncio.sleep(0.01)
        with pytest.raises(asyncio.CancelledError):
            await future
        await asyncio.sleep(0.1)
        return problems

    problems = asyncio.run(go())

    released = [item for item in _seen_by(thread_id) if item.get("phase") == "released"]
    assert released, "the resumed node never finished"
    assert released[0]["has_token"], "a resumed run carries no cancel token"
    assert released[0]["saw_cancel"], "cancelling the Future did not cancel the resumed run"
    assert not [item for item in problems if "InvalidStateError" in repr(item.get("exception"))]


def test_resume_from_async_resumes_the_exact_checkpoint():
    engine = _engine("entrypoints.ask")
    thread_id = "exact-checkpoint"
    paused = engine.run(ng.RunConfig(thread_id=thread_id))
    assert paused.interrupted and paused.checkpoint_id

    async def go():
        return await engine.resume_from_async(
            ng.RunConfig(thread_id=thread_id), paused.checkpoint_id, "yes")

    result = asyncio.run(go())

    assert result.status == ng.RunStatus.Completed
    assert result.output["channels"]["answer"]["value"] == "done"
    assert _seen_by(thread_id)[0]["resume_value"] == "yes"


@pytest.mark.parametrize("entry", RESUME_ENTRIES)
def test_an_interrupt_id_that_is_not_pending_raises_on_every_resume_entry(entry):
    engine = _engine("entrypoints.ask")

    with pytest.raises(ValueError):
        _drive(entry, engine, f"unknown-interrupt-{entry}", interrupt_id="nobody")


def test_resume_from_async_requires_a_checkpoint_id():
    engine = _engine("entrypoints.ask")

    async def go():
        return await engine.resume_from_async(ng.RunConfig(thread_id="no-checkpoint"), "", "yes")

    with pytest.raises(ValueError):
        asyncio.run(go())


@pytest.mark.parametrize("entry", ALL_ENTRIES)
def test_metadata_must_be_a_run_metadata(entry):
    engine = _engine(_node_type(entry))
    config = ng.RunConfig(thread_id=f"bad-metadata-{entry}")
    callback = lambda _event: None  # noqa: E731
    calls = {
        "run": lambda: engine.run(config, metadata="soon"),
        "run_stream": lambda: engine.run_stream(config, callback, metadata="soon"),
        "run_async": lambda: engine.run_async(config, metadata="soon"),
        "run_stream_async": lambda: engine.run_stream_async(config, callback, metadata="soon"),
        "resume": lambda: engine.resume("t", "yes", metadata="soon"),
        "resume_from": lambda: engine.resume_from(config, "cp", "yes", metadata="soon"),
        "resume_async": lambda: engine.resume_async("t", "yes", metadata="soon"),
        "resume_from_async": lambda: engine.resume_from_async(config, "cp", "yes", metadata="soon"),
    }
    with pytest.raises(TypeError):
        calls[entry]()


def test_existing_positional_calls_keep_their_meaning():
    engine = _engine("entrypoints.ask")
    thread_id = "positional"
    config = ng.RunConfig(thread_id=thread_id)
    seen = []

    paused = engine.run_stream(config, seen.append)
    assert paused.interrupted

    # resume(thread_id, resume_value, interrupt_id): the third positional stays the interrupt id.
    with pytest.raises(ValueError):
        engine.resume(thread_id, "yes", "nobody")
    # resume_from(config, checkpoint_id, resume_value, metadata, interrupt_id)
    done = engine.resume_from(config, paused.checkpoint_id, "yes", _metadata("positional"), None)
    assert done.status == ng.RunStatus.Completed
    assert _seen_by(thread_id)[0]["run_id"] == "run-positional"
