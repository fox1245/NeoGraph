"""Python implementations of the C++ Store contracts (#117)."""

import asyncio
import concurrent.futures
import gc
import threading
import weakref

import neograph_engine as ng
import pytest


class DictStore(ng.Store):
    def __init__(self):
        ng.Store.__init__(self)
        self.items = {}

    def put(self, ns, key, value):
        item = ng.StoreItem()
        item.ns = ns
        item.key = key
        item.value = value
        item.created_at = item.updated_at = 1
        self.items[(tuple(ns), key)] = item

    def get(self, ns, key):
        return self.items.get((tuple(ns), key))

    def search(self, ns_prefix, limit=100):
        prefix = tuple(ns_prefix)
        return [item for (ns, _), item in self.items.items()
                if ns[:len(prefix)] == prefix][:limit]

    def delete_item(self, ns, key):
        self.items.pop((tuple(ns), key), None)

    def list_namespaces(self, prefix=None):
        prefix = tuple(prefix or [])
        return sorted({ns for ns, _ in self.items if ns[:len(prefix)] == prefix})


def _copy_checkpoint(source):
    cp = ng.Checkpoint()
    for name in (
        "id", "thread_id", "channel_values", "channel_versions", "parent_id",
        "current_node", "next_nodes", "interrupt_phase", "barrier_state",
        "metadata", "step", "timestamp", "schema_version",
    ):
        setattr(cp, name, getattr(source, name))
    return cp


class DictCheckpointStore(ng.CheckpointStore):
    """Unmanaged CPU checkpoint storage; no managed-bank grant/journal support."""

    def __init__(self):
        ng.CheckpointStore.__init__(self)
        self._lock = threading.Lock()
        self.by_thread = {}
        self.by_id = {}
        self.callback_threads = set()
        self._managed_denials = set()

    def save(self, checkpoint):
        cp = _copy_checkpoint(checkpoint)
        with self._lock:
            self.callback_threads.add(threading.get_ident())
            if "provider_managed_budget" in cp.channel_values:
                self._managed_denials.add(cp.thread_id)
                raise RuntimeError("dictionary backend cannot persist managed-bank custody")
            self.by_thread.setdefault(cp.thread_id, []).append(cp)
            self.by_id[cp.id] = cp

    def requires_managed_budget(self, thread_id):
        with self._lock:
            return thread_id in self._managed_denials

    def load_latest(self, thread_id):
        with self._lock:
            values = self.by_thread.get(thread_id, [])
            return _copy_checkpoint(values[-1]) if values else None

    def load_by_id(self, checkpoint_id):
        with self._lock:
            cp = self.by_id.get(checkpoint_id)
            return _copy_checkpoint(cp) if cp else None

    def list(self, thread_id, limit=100):
        with self._lock:
            return [_copy_checkpoint(cp)
                    for cp in reversed(self.by_thread.get(thread_id, []))][:limit]

    def delete_thread(self, thread_id):
        with self._lock:
            for cp in self.by_thread.pop(thread_id, []):
                self.by_id.pop(cp.id, None)


def _empty_definition():
    return {
        "name": "python_checkpoint_backend",
        "channels": {"value": {"reducer": "overwrite"}},
        "nodes": {},
        "edges": [{"from": ng.START_NODE, "to": ng.END_NODE}],
    }


def _unmanaged_run_config(thread_id):
    """These CPU-only backend tests do not request managed model-token currency."""
    config = ng.RunConfig(thread_id=thread_id)
    config.model_token_budget = 0
    return config


class _NoopNode(ng.GraphNode):
    def __init__(self):
        ng.GraphNode.__init__(self)

    def run(self, input):
        return []

    def get_name(self):
        return "checkpoint_noop"


ng.NodeFactory.register_type("py_checkpoint_noop", lambda *_: _NoopNode())


def _noop_definition():
    definition = _empty_definition()
    definition["nodes"] = {"checkpoint_noop": {"type": "py_checkpoint_noop"}}
    definition["edges"] = [
        {"from": ng.START_NODE, "to": "checkpoint_noop"},
        {"from": "checkpoint_noop", "to": ng.END_NODE},
    ]
    return definition


def test_store_base_methods_dispatch_to_python_and_convert_json():
    store = DictStore()
    ng.Store.put(store, ["users", "u1"], "prefs",
                 {"theme": "dark", "flags": [1, True]})

    assert ng.Store.get(store, ["users", "u1"], "prefs").value == {
        "theme": "dark", "flags": [1, True]
    }
    assert [item.key for item in ng.Store.search(store, ["users"])] == ["prefs"]
    assert ng.Store.list_namespaces(store) == [["users", "u1"]]
    ng.Store.delete_item(store, ["users", "u1"], "prefs")
    assert ng.Store.get(store, ["users", "u1"], "prefs") is None


def test_checkpoint_base_methods_persist_values():
    store = DictCheckpointStore()
    cp = ng.Checkpoint()
    cp.id = "cp-1"
    cp.thread_id = "thread-1"
    cp.channel_values = {"answer": 42}
    cp.channel_versions = {"answer": 1}
    cp.metadata = {"source": "python"}
    cp.interrupt_phase = ng.CheckpointPhase.Updated
    ng.CheckpointStore.save(store, cp)

    loaded = ng.CheckpointStore.load_latest(store, "thread-1")
    assert loaded.id == "cp-1"
    assert loaded.channel_values == {"answer": 42}
    assert loaded.metadata == {"source": "python"}
    assert (ng.CheckpointStore.load_by_id(store, "cp-1").interrupt_phase
            == ng.CheckpointPhase.Updated)
    assert [item.id for item in ng.CheckpointStore.list(store, "thread-1")] == ["cp-1"]

def test_python_checkpoint_backend_drives_save_get_state_and_resume():
    class InterruptOnce(ng.GraphNode):
        def __init__(self):
            ng.GraphNode.__init__(self)
            self.calls = 0

        def run(self, input):
            self.calls += 1
            if input.ctx.resume_value is None:
                raise ng.NodeInterrupt({"question": "continue?"}, reason="approval")
            return [ng.ChannelWrite("value", input.ctx.resume_value["value"])]

        def get_name(self):
            return "interrupt_once"

    node = InterruptOnce()
    ng.NodeFactory.register_type("py_checkpoint_interrupt", lambda *_: node)
    definition = _empty_definition()
    definition["nodes"] = {"interrupt_once": {"type": "py_checkpoint_interrupt"}}
    definition["edges"] = [
        {"from": ng.START_NODE, "to": "interrupt_once"},
        {"from": "interrupt_once", "to": ng.END_NODE},
    ]
    store = DictCheckpointStore()
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(), store)

    assert engine.run(_unmanaged_run_config("resume-me")).interrupted
    assert engine.get_state("resume-me") is not None
    result = engine.resume("resume-me", {"value": 7})

    assert not result.interrupted
    assert result.output["channels"]["value"]["value"] == 7


def test_backend_exception_propagates_out_of_engine_run():
    failure = OSError("checkpoint save rejected")

    class Broken(DictCheckpointStore):
        def save(self, checkpoint):
            raise failure

    engine = ng.GraphEngine.compile(_noop_definition(), ng.NodeContext(), Broken())
    with pytest.raises(OSError) as caught:
        engine.run(_unmanaged_run_config("broken"))
    assert caught.value is failure
    assert caught.value.__traceback__ is not None


def test_unmanaged_backend_cannot_grant_a_bounded_bank():
    store = DictCheckpointStore()
    engine = ng.GraphEngine.compile(_noop_definition(), ng.NodeContext(), store)
    config = _unmanaged_run_config("bounded-bank-denied")
    config.model_token_budget = 1000
    metadata = ng.RunMetadata(owner_scope="checkpoint-owner")
    with pytest.raises(RuntimeError):
        engine.run(config, metadata)
    assert store.by_thread == {}


def test_temporary_backends_survive_compile_and_setter_reassignment():
    first = DictCheckpointStore()
    first_ref = weakref.ref(first)
    engine = ng.GraphEngine.compile(_noop_definition(), ng.NodeContext(), first)
    del first
    gc.collect()
    assert first_ref() is not None

    second = DictCheckpointStore()
    second_ref = weakref.ref(second)
    engine.set_checkpoint_store(second)
    del second
    gc.collect()
    assert first_ref() is None
    assert second_ref() is not None
    engine.run(_unmanaged_run_config("kept-alive"))

    old_memory = DictStore()
    old_memory_ref = weakref.ref(old_memory)
    engine.set_store(old_memory)
    del old_memory
    gc.collect()
    assert old_memory_ref() is not None

    new_memory = DictStore()
    memory_ref = weakref.ref(new_memory)
    engine.set_store(new_memory)
    del new_memory
    gc.collect()
    assert old_memory_ref() is None
    assert memory_ref() is not None
    engine.get_store().put(["lifetime"], "ok", True)


def test_async_engine_path_inherits_the_sync_backend_bridge():
    store = DictCheckpointStore()
    engine = ng.GraphEngine.compile(_noop_definition(), ng.NodeContext(), store)

    async def run():
        return await engine.run_async(_unmanaged_run_config("async-backend"))

    result = asyncio.run(run())
    assert result.checkpoint_id
    assert store.load_latest("async-backend").id == result.checkpoint_id


def test_async_run_keeps_python_engine_and_backends_alive_until_completion():
    entered = threading.Event()
    release = threading.Event()

    class BlockingCheckpointStore(DictCheckpointStore):
        def save(self, checkpoint):
            entered.set()
            assert release.wait(timeout=5)
            super().save(checkpoint)

    async def run():
        checkpoint_store = BlockingCheckpointStore()
        memory_store = DictStore()
        engine = ng.GraphEngine.compile(
            _noop_definition(), ng.NodeContext(), checkpoint_store)
        engine.set_store(memory_store)
        engine_ref = weakref.ref(engine)
        checkpoint_ref = weakref.ref(checkpoint_store)
        memory_ref = weakref.ref(memory_store)

        future = engine.run_async(_unmanaged_run_config("async-lifetime"))
        assert await asyncio.to_thread(entered.wait, 5)
        del engine, checkpoint_store, memory_store
        gc.collect()

        assert engine_ref() is not None
        assert checkpoint_ref() is not None
        assert memory_ref() is not None

        release.set()
        result = await future
        assert result.checkpoint_id

    asyncio.run(run())


def test_async_resume_keeps_callback_engine_and_backend_storage_alive():
    entered = threading.Event()
    release = threading.Event()

    class BlockingCheckpointStore(DictCheckpointStore):
        block = False

        def save(self, checkpoint):
            if self.block:
                entered.set()
                assert release.wait(timeout=5)
            super().save(checkpoint)

    class InterruptOnce(ng.GraphNode):
        def __init__(self):
            ng.GraphNode.__init__(self)

        def run(self, input):
            if input.ctx.resume_value is None:
                raise ng.NodeInterrupt({"question": "continue?"})
            return [ng.ChannelWrite("value", input.ctx.resume_value["value"])]

        def get_name(self):
            return "async_resume_lifetime"

    node = InterruptOnce()
    ng.NodeFactory.register_type("py_async_resume_lifetime", lambda *_: node)
    definition = _empty_definition()
    definition["nodes"] = {
        "async_resume_lifetime": {"type": "py_async_resume_lifetime"},
    }
    definition["edges"] = [
        {"from": ng.START_NODE, "to": "async_resume_lifetime"},
        {"from": "async_resume_lifetime", "to": ng.END_NODE},
    ]

    async def run():
        store = BlockingCheckpointStore()
        engine = ng.GraphEngine.compile(definition, ng.NodeContext(), store)
        assert engine.run(_unmanaged_run_config("async-resume-lifetime")).interrupted
        store.block = True

        engine_ref = weakref.ref(engine)
        store_ref = weakref.ref(store)
        future = engine.resume_async(
            "async-resume-lifetime", {"value": 11})
        assert await asyncio.to_thread(entered.wait, 5)
        del engine, store
        gc.collect()
        assert engine_ref() is not None
        assert store_ref() is not None

        release.set()
        result = await future
        assert result.output["channels"]["value"]["value"] == 11

    asyncio.run(run())


def test_callbacks_from_parallel_runs_are_isolated_and_gil_safe():
    store = DictCheckpointStore()
    engine = ng.GraphEngine.compile(_noop_definition(), ng.NodeContext(), store)

    def run(index):
        result = engine.run(_unmanaged_run_config(f"parallel-{index}"))
        return result.checkpoint_id

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        checkpoint_ids = list(pool.map(run, range(12)))

    assert all(checkpoint_ids)
    assert len(store.by_thread) == 12
    assert len(store.callback_threads) >= 2
