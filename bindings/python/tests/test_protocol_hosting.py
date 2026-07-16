"""Protocol SDK bridge: session continuity, output mapping, and cancellation."""

import asyncio
from types import SimpleNamespace

import pytest

import neograph_engine as ng


class RecordingEngine:
    def __init__(self, output=None):
        self.configs = []
        self.output = output or {
            "channels": {
                "messages": {
                    "value": [{"role": "assistant", "content": "hello"}]
                }
            }
        }

    async def run_async(self, cfg):
        self.configs.append(cfg)
        return SimpleNamespace(output=self.output)


def test_protocol_host_maps_session_and_chat_state():
    engine = RecordingEngine()
    adapter = ng.ProtocolHostAdapter(engine)

    answer = asyncio.run(
        adapter.run("question", thread_id="context-7", request_id="task-2")
    )

    assert answer == "hello"
    assert len(engine.configs) == 1
    cfg = engine.configs[0]
    assert cfg.thread_id == "context-7"
    assert cfg.resume_if_exists is True
    assert cfg.input == {
        "messages": [{"role": "user", "content": "question"}]
    }
    assert adapter.active_count() == 0


def test_protocol_host_accepts_custom_graph_shape():
    engine = RecordingEngine(
        output={"channels": {"answer": {"value": {"text": "custom"}}}}
    )
    adapter = ng.ProtocolHostAdapter(
        engine,
        input_builder=lambda text: {"prompt": text},
        output_reader=lambda result: result.output["channels"]["answer"][
            "value"
        ]["text"],
    )

    answer = asyncio.run(adapter.run("question", thread_id="session-1"))

    assert answer == "custom"
    assert engine.configs[0].input == {"prompt": "question"}


def test_protocol_host_rejects_missing_output_channel():
    adapter = ng.ProtocolHostAdapter(RecordingEngine(output={"channels": {}}))

    with pytest.raises(ValueError, match="output_reader"):
        asyncio.run(adapter.run("question", thread_id="session-1"))


def test_protocol_cancel_reaches_underlying_async_run():
    class BlockingEngine:
        def __init__(self):
            self.started = asyncio.Event()
            self.cancelled = False

        async def run_async(self, cfg):
            self.started.set()
            try:
                await asyncio.Future()
            except asyncio.CancelledError:
                self.cancelled = True
                raise

    async def exercise():
        engine = BlockingEngine()
        adapter = ng.ProtocolHostAdapter(engine)
        call = asyncio.create_task(
            adapter.run("wait", thread_id="session-1", request_id="request-1")
        )
        await engine.started.wait()

        assert adapter.active_count("request-1") == 1
        assert adapter.cancel("request-1") == 1
        with pytest.raises(asyncio.CancelledError):
            await call
        assert engine.cancelled is True
        assert adapter.active_count() == 0

    asyncio.run(exercise())


def test_protocol_host_requires_thread_id():
    adapter = ng.ProtocolHostAdapter(RecordingEngine())

    with pytest.raises(ValueError, match="thread_id"):
        asyncio.run(adapter.run("question", thread_id=""))


def test_protocol_host_serializes_runs_for_same_checkpoint_thread():
    class ConcurrencyEngine:
        def __init__(self):
            self.inflight = 0
            self.max_inflight = 0

        async def run_async(self, cfg):
            self.inflight += 1
            self.max_inflight = max(self.max_inflight, self.inflight)
            await asyncio.sleep(0.01)
            self.inflight -= 1
            text = cfg.input["messages"][-1]["content"]
            return SimpleNamespace(
                output={
                    "channels": {
                        "messages": {
                            "value": [
                                {"role": "assistant", "content": text}
                            ]
                        }
                    }
                }
            )

    async def exercise():
        engine = ConcurrencyEngine()
        adapter = ng.ProtocolHostAdapter(engine)
        answers = await asyncio.gather(
            adapter.run("one", thread_id="same", request_id="r1"),
            adapter.run("two", thread_id="same", request_id="r2"),
        )
        same_thread_max = engine.max_inflight
        engine.max_inflight = 0
        await asyncio.gather(
            adapter.run("three", thread_id="left", request_id="r3"),
            adapter.run("four", thread_id="right", request_id="r4"),
        )
        return same_thread_max, engine.max_inflight, answers

    same_thread_max, different_thread_max, answers = asyncio.run(exercise())
    assert same_thread_max == 1
    assert different_thread_max == 2
    assert answers == ["one", "two"]


def test_protocol_host_resumes_real_checkpointed_conversation():
    class EchoNode(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self.name = name

        def get_name(self):
            return self.name

        def run(self, input):
            messages = input.state.get("messages") or []
            text = messages[-1]["content"]
            return [
                ng.ChannelWrite(
                    "messages",
                    [{"role": "assistant", "content": f"reply:{text}"}],
                )
            ]

    type_name = "protocol_hosting_test_echo"
    ng.NodeFactory.register_type(
        type_name, lambda name, config, ctx: EchoNode(name)
    )
    definition = {
        "name": "protocol_hosting_test",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"echo": {"type": type_name}},
        "edges": [
            {"from": ng.START_NODE, "to": "echo"},
            {"from": "echo", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
    adapter = ng.ProtocolHostAdapter(engine)

    async def exercise():
        first = await adapter.run("one", thread_id="session-1")
        second = await adapter.run("two", thread_id="session-1")
        return first, second

    assert asyncio.run(exercise()) == ("reply:one", "reply:two")
    messages = engine.get_state("session-1")["channels"]["messages"]["value"]
    assert [message["content"] for message in messages] == [
        "one",
        "reply:one",
        "two",
        "reply:two",
    ]
    assert adapter.cancel("not-running") == 0
