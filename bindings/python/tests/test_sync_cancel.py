"""Synchronous Python run cancellation through RunConfig (#119)."""

import concurrent.futures
import threading

import neograph_engine as ng
import pytest


_uid = 0


def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


def test_cancel_token_is_constructible_and_assignable_to_run_config():
    token = ng.CancelToken()
    config = ng.RunConfig(thread_id="cancel-surface")

    assert not token.is_cancelled()
    assert config.cancel_token is None
    config.cancel_token = token
    assert config.cancel_token is token

    token.cancel()
    token.cancel()
    assert token.is_cancelled()


def test_pre_cancelled_sync_run_stops_before_node_execution():
    called = False
    type_name = _next_type("pre_cancelled")

    class MustNotRun(ng.GraphNode):
        def __init__(self):
            super().__init__()

        def run(self, input):
            nonlocal called
            called = True
            return []

        def get_name(self):
            return "must_not_run"

    ng.NodeFactory.register_type(type_name, lambda *_: MustNotRun())
    definition = {
        "name": "pre_cancelled",
        "channels": {},
        "nodes": {"must_not_run": {"type": type_name}},
        "edges": [
            {"from": ng.START_NODE, "to": "must_not_run"},
            {"from": "must_not_run", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    token = ng.CancelToken()
    token.cancel()
    config = ng.RunConfig(thread_id="pre-cancelled")
    config.cancel_token = token

    with pytest.raises(RuntimeError, match="run cancelled"):
        engine.run(config)
    assert not called


def test_sync_run_can_be_cancelled_from_another_python_thread():
    entered = threading.Event()
    release = threading.Event()
    second_node_called = False
    block_type = _next_type("cancel_block")
    second_type = _next_type("cancel_second")

    class BlockingNode(ng.GraphNode):
        def __init__(self):
            super().__init__()

        def run(self, input):
            entered.set()
            assert release.wait(timeout=5)
            return []

        def get_name(self):
            return "blocking"

    class MustNotRun(ng.GraphNode):
        def __init__(self):
            super().__init__()

        def run(self, input):
            nonlocal second_node_called
            second_node_called = True
            return []

        def get_name(self):
            return "must_not_run"

    ng.NodeFactory.register_type(block_type, lambda *_: BlockingNode())
    ng.NodeFactory.register_type(second_type, lambda *_: MustNotRun())
    definition = {
        "name": "cross_thread_cancel",
        "channels": {},
        "nodes": {
            "blocking": {"type": block_type},
            "must_not_run": {"type": second_type},
        },
        "edges": [
            {"from": ng.START_NODE, "to": "blocking"},
            {"from": "blocking", "to": "must_not_run"},
            {"from": "must_not_run", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    token = ng.CancelToken()
    config = ng.RunConfig(thread_id="cross-thread-cancel")
    config.cancel_token = token

    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        future = pool.submit(engine.run, config)
        assert entered.wait(timeout=5)
        token.cancel()
        release.set()
        with pytest.raises(RuntimeError, match="run cancelled"):
            future.result(timeout=5)

    assert not second_node_called
