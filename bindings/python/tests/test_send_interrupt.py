"""A Python node that a Send dispatches can pause the graph (#385).

A map node returns one ng.Send per item, and the Send target raises
ng.NodeInterrupt for the item that needs an answer. The run used to raise
RuntimeError and leave no checkpoint. It now pauses like any other node: an
Interrupted result, a NodeInterrupt checkpoint that re-enters the step, and a
resume that runs the Send that asked with the answer while the map node and
the Sends that finished replay without running again. Every entry point of the
binding behaves the same way.
"""

import asyncio
import threading

import pytest

import neograph_engine as ng


_lock = threading.Lock()
_runs = {}


def _count(key):
    with _lock:
        _runs[key] = _runs.get(key, 0) + 1


def _ran(key):
    with _lock:
        return _runs.get(key, 0)


class _Map(ng.GraphNode):
    def __init__(self, name, probe):
        super().__init__()
        self._name, self._probe = name, probe

    def get_name(self):
        return self._name

    def run(self, input):
        _count(f"{self._probe}/map")
        items = input.state.get("items") or []
        return [ng.ChannelWrite("trace", ["map"])] + [ng.Send("worker", {"item": i}) for i in items]


class _Worker(ng.GraphNode):
    def __init__(self, name, probe):
        super().__init__()
        self._name, self._probe = name, probe

    def get_name(self):
        return self._name

    def run(self, input):
        item = input.state.get("item")
        _count(f"{self._probe}/worker:{item}")
        if input.state.get("ask") == item:
            answer = input.ctx.resume_value
            if answer is None:
                raise ng.NodeInterrupt({"item": item}, reason=f"approve {item}")
            return [ng.ChannelWrite("results", [f"{item}={answer}"])]
        return [ng.ChannelWrite("results", [item])]


_probes = iter(range(1_000_000))


def _engine(store=None):
    probe = f"send-interrupt-{next(_probes)}"
    ng.NodeFactory.register_type(f"{probe}.map", lambda name, _c, _x: _Map(name, probe))
    ng.NodeFactory.register_type(f"{probe}.worker", lambda name, _c, _x: _Worker(name, probe))
    definition = {
        "name": "py_send_interrupt",
        "channels": {
            "items": {"reducer": "overwrite", "initial": []},
            "ask": {"reducer": "overwrite", "initial": ""},
            "item": {"reducer": "overwrite", "initial": ""},
            "results": {"reducer": "append", "initial": []},
            "trace": {"reducer": "append", "initial": []},
        },
        "nodes": {"map": {"type": f"{probe}.map"}, "worker": {"type": f"{probe}.worker"}},
        "edges": [
            {"from": "__start__", "to": "map"},
            {"from": "map", "to": "__end__"},
            {"from": "worker", "to": "__end__"},
        ],
    }
    store = store or ng.InMemoryCheckpointStore()
    return ng.GraphEngine.compile(definition, ng.NodeContext(), store), store, probe


def _config(thread_id, items, ask):
    return ng.RunConfig(thread_id=thread_id, input={"items": items, "ask": ask})


def _results(result):
    return result.output["channels"]["results"]["value"]


def test_a_send_target_that_interrupts_pauses_instead_of_raising():
    engine, store, probe = _engine()

    paused = engine.run(_config("py-send-pause", ["a", "b", "c"], "b"))

    assert paused.interrupted, "a NodeInterrupt in a Send target must pause the run"
    assert paused.interrupt_node == "worker"
    assert paused.interrupt_value["reason"] == "approve b"
    assert paused.interrupt_value["value"] == {"item": "b"}
    assert paused.checkpoint_id
    saved = store.load_by_id(paused.checkpoint_id)
    assert saved.interrupt_phase == ng.CheckpointPhase.NodeInterrupt
    assert list(saved.next_nodes) == ["map"]
    assert saved.channel_values["channels"]["results"]["value"] == []

    done = engine.resume("py-send-pause", "yes")

    assert not done.interrupted
    assert _results(done) == ["a", "b=yes", "c"]
    assert _ran(f"{probe}/map") == 1, "the map node replays; it does not run again"
    assert _ran(f"{probe}/worker:a") == 1, "a Send that finished replays"
    assert _ran(f"{probe}/worker:c") == 1, "a Send that finished replays"
    assert _ran(f"{probe}/worker:b") == 2


def _pause_run(engine, config):
    return engine.run(config)


def _pause_run_stream(engine, config):
    return engine.run_stream(config, lambda _event: None)


def _pause_run_async(engine, config):
    async def go():
        return await engine.run_async(config)
    return asyncio.run(go())


def _pause_run_stream_async(engine, config):
    async def go():
        return await engine.run_stream_async(config, lambda _event: None)
    return asyncio.run(go())


def _resume(engine, config, _paused):
    return engine.resume(config.thread_id, "yes")


def _resume_async(engine, config, _paused):
    async def go():
        return await engine.resume_async(config.thread_id, "yes")
    return asyncio.run(go())


def _resume_from(engine, config, paused):
    return engine.resume_from(ng.RunConfig(thread_id=config.thread_id), paused.checkpoint_id, "yes")


@pytest.mark.parametrize("pause, resume", [
    (_pause_run, _resume),
    (_pause_run_stream, _resume_async),
    (_pause_run_async, _resume_from),
    (_pause_run_stream_async, _resume),
    (_pause_run, _resume_from),
], ids=["run+resume", "run_stream+resume_async", "run_async+resume_from",
        "run_stream_async+resume", "run+resume_from"])
def test_every_entry_point_pauses_and_resumes_the_same_way(pause, resume):
    engine, _store, probe = _engine()
    config = _config(f"py-send-{pause.__name__}-{resume.__name__}", ["a", "b"], "a")

    paused = pause(engine, config)

    assert paused.interrupted
    assert paused.interrupt_node == "worker"
    assert paused.checkpoint_id

    done = resume(engine, config, paused)

    assert not done.interrupted
    assert _results(done) == ["a=yes", "b"]
    assert _ran(f"{probe}/map") == 1
    assert _ran(f"{probe}/worker:b") == 1
    assert _ran(f"{probe}/worker:a") == 2


def test_a_resumed_send_can_ask_again():
    engine, store, probe = _engine()
    first = engine.run(_config("py-send-again", ["a", "b"], "a"))
    assert first.interrupted

    second = engine.resume_from(ng.RunConfig(thread_id="py-send-again"), first.checkpoint_id, None)
    assert second.interrupted, "a Send resumed without an answer asks again"
    assert second.checkpoint_id != first.checkpoint_id
    assert store.load_by_id(second.checkpoint_id).parent_id == first.checkpoint_id

    done = engine.resume("py-send-again", "ok")
    assert _results(done) == ["a=ok", "b"]
    assert _ran(f"{probe}/worker:b") == 1
    assert _ran(f"{probe}/worker:a") == 3
