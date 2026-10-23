"""Protocol bridge: a graph that pauses for a human reaches the protocol client.

A graph can pause (`interrupt_before`, a `NodeInterrupt`, a tool gate). The A2A
and ACP hosts have to tell their client what is asked and, when the answer
comes, resume the checkpoint the run paused at. The adapter used to read the
output of a paused run as if it were an answer: with the default chat reader
that is the last message of the state, which is the user's own text.

The fakes here stand in for the engine so each argument that reaches it can be
read; the last tests drive a real engine and a real `NodeInterrupt`.
"""

import asyncio
from types import SimpleNamespace

import pytest

import neograph_engine as ng


PAUSE_VALUE = {
    "reason": "Run `rm -rf build`?",
    "type": "NodeInterrupt",
    "value": {"command": "rm -rf build"},
}


def _messages(*texts):
    return {
        "channels": {
            "messages": {
                "value": [{"role": "assistant", "content": t} for t in texts]
            }
        }
    }


def _paused(checkpoint_id="cp-1", interrupt_id="s0:ask", value=None, user="hi"):
    """A RunResult of a run that paused: the state still ends with the user's text."""
    return SimpleNamespace(
        output={
            "channels": {
                "messages": {"value": [{"role": "user", "content": user}]}
            }
        },
        interrupted=True,
        interrupt_node="ask",
        interrupt_value=PAUSE_VALUE if value is None else value,
        checkpoint_id=checkpoint_id,
        interrupt_id=interrupt_id,
    )


def _token(node, text):
    event = ng.GraphEvent()
    event.type = ng.GraphEvent.Type.LLM_TOKEN
    event.node_name = node
    event.data = text
    return event


class PausingEngine:
    """Pauses a fresh run, completes a resumed one, and records every call."""

    def __init__(self, resumed=None, pause=None):
        self.calls = []
        self.pause = pause or _paused
        self.resumed = resumed or [_done("approved")]

    def _emit(self, callback, text):
        if callback is not None:
            for token in text.split("|"):
                callback(_token("answer", token))

    async def run_async(self, cfg):
        self.calls.append(("run_async", cfg))
        return self.pause()

    async def run_stream_async(self, cfg, callback):
        self.calls.append(("run_stream_async", cfg, callback))
        return self.pause()

    async def resume_from_async(
        self, cfg, checkpoint_id, resume_value=None, metadata=None,
        interrupt_id=None, *, callback=None,
    ):
        self.calls.append(
            ("resume_from_async", cfg, checkpoint_id, resume_value,
             interrupt_id, callback)
        )
        result = self.resumed.pop(0)
        self._emit(callback, result.output["channels"]["messages"]["value"][-1]
                   ["content"].replace(" ", "| |"))
        return result

    async def resume_async(
        self, thread_id, resume_value=None, interrupt_id=None, *,
        callback=None, metadata=None,
    ):
        self.calls.append(
            ("resume_async", thread_id, resume_value, interrupt_id, callback)
        )
        result = self.resumed.pop(0)
        self._emit(callback, result.output["channels"]["messages"]["value"][-1]
                   ["content"].replace(" ", "| |"))
        return result


def _done(text):
    return SimpleNamespace(output=_messages(text), interrupted=False)


async def _collect(stream):
    return [event async for event in stream]


# -- reporting a pause -------------------------------------------------------


def test_run_raises_for_a_paused_graph_instead_of_returning_the_users_text():
    adapter = ng.ProtocolHostAdapter(PausingEngine())

    with pytest.raises(ng.ProtocolInterrupted) as caught:
        asyncio.run(adapter.run("deploy", thread_id="ctx-1", request_id="t-1"))

    interrupt = caught.value.interrupt
    assert interrupt.thread_id == "ctx-1"
    assert interrupt.node == "ask"
    assert interrupt.reason == "Run `rm -rf build`?"
    assert interrupt.prompt == "Run `rm -rf build`?"
    assert interrupt.payload == {"command": "rm -rf build"}
    assert interrupt.checkpoint_id == "cp-1"
    assert interrupt.interrupt_id == "s0:ask"
    assert adapter.pending_interrupt("ctx-1") == interrupt
    assert adapter.active_count() == 0


def test_stream_ends_with_an_interrupt_event_instead_of_a_final_one():
    adapter = ng.ProtocolHostAdapter(PausingEngine(), stream_node="answer")

    events = asyncio.run(_collect(
        adapter.stream("deploy", thread_id="ctx-1", interrupts=True)
    ))

    assert [event.kind for event in events] == ["interrupt"]
    assert events[0].data == "Run `rm -rf build`?"
    assert events[0].interrupt.payload == {"command": "rm -rf build"}
    assert events[0].interrupt.checkpoint_id == "cp-1"


def test_stream_without_interrupts_raises_so_a_pause_is_never_a_final_answer():
    adapter = ng.ProtocolHostAdapter(PausingEngine(), stream_node="answer")

    async def consume():
        return [event async for event in adapter.stream("deploy", thread_id="c")]

    with pytest.raises(ng.ProtocolInterrupted):
        asyncio.run(consume())
    assert adapter.pending_interrupt("c") is not None


def test_a_pause_without_a_reason_has_a_neutral_prompt():
    engine = PausingEngine(pause=lambda: _paused(
        value={"message": "Interrupt before node: publish"}, interrupt_id=""
    ))
    adapter = ng.ProtocolHostAdapter(engine)
    events = asyncio.run(_collect(
        adapter.stream("x", thread_id="c", interrupts=True)
    ))
    interrupt = events[0].interrupt
    assert interrupt.reason == "Interrupt before node: publish"
    assert interrupt.payload is None
    assert interrupt.interrupt_id == ""

    bare = PausingEngine(pause=lambda: _paused(value={}))
    events = asyncio.run(_collect(ng.ProtocolHostAdapter(bare).stream(
        "x", thread_id="c", interrupts=True
    )))
    assert events[0].interrupt.reason == ""
    assert events[0].data == ng.protocol.DEFAULT_INTERRUPT_PROMPT


# -- answering a pause -------------------------------------------------------


def test_resume_stream_resumes_the_checkpoint_it_paused_at_and_streams():
    engine = PausingEngine(resumed=[_done("ran it")])
    adapter = ng.ProtocolHostAdapter(engine, stream_node="answer")

    async def exercise():
        first = await _collect(
            adapter.stream("deploy", thread_id="ctx-1", interrupts=True)
        )
        second = await _collect(adapter.resume_stream(
            "approve", thread_id="ctx-1", request_id="t-1"
        ))
        return first, second

    first, second = asyncio.run(exercise())

    assert [e.kind for e in first] == ["interrupt"]
    assert [(e.kind, e.data) for e in second] == [
        ("token", "ran"), ("token", " "), ("token", "it"), ("final", "ran it"),
    ]
    name, cfg, checkpoint_id, value, interrupt_id, callback = engine.calls[-1]
    assert name == "resume_from_async"
    assert cfg.thread_id == "ctx-1"
    assert cfg.stream_mode == ng.StreamMode.TOKENS
    assert checkpoint_id == "cp-1"
    assert value == "approve"
    assert interrupt_id == "s0:ask"
    assert callback is not None
    assert adapter.pending_interrupt("ctx-1") is None
    assert adapter.active_count() == 0


def test_an_interrupt_before_pause_resumes_without_an_interrupt_id():
    engine = PausingEngine(
        pause=lambda: _paused(interrupt_id="", value={"message": "before"}),
    )
    adapter = ng.ProtocolHostAdapter(engine)

    async def exercise():
        await _collect(adapter.stream("x", thread_id="c", interrupts=True))
        return await adapter.resume_payload("go", thread_id="c")

    assert asyncio.run(exercise()) == "approved"
    assert engine.calls[-1][4] is None  # interrupt_id


def test_resume_without_a_checkpoint_id_resumes_the_latest_checkpoint():
    engine = PausingEngine()
    adapter = ng.ProtocolHostAdapter(engine)
    stored = ng.ProtocolInterrupt(thread_id="ctx-9", interrupt_id="s1:ask")

    events = asyncio.run(_collect(adapter.resume_stream(
        "yes", thread_id="ctx-9", interrupt=stored
    )))

    assert events[-1].kind == "final"
    assert engine.calls == [("resume_async", "ctx-9", "yes", "s1:ask",
                             engine.calls[0][4])]
    assert engine.calls[0][4] is not None  # the stream callback


def test_resume_of_a_thread_with_no_known_pause_is_refused_not_answered_with_old_text():
    engine = PausingEngine()
    adapter = ng.ProtocolHostAdapter(engine)

    with pytest.raises(LookupError, match="no interrupt waits on thread 'later'"):
        asyncio.run(adapter.resume_payload("yes", thread_id="later"))
    with pytest.raises(LookupError):
        asyncio.run(_collect(adapter.resume_stream("yes", thread_id="later")))
    assert engine.calls == []
    assert adapter.active_count() == 0


def test_a_fresh_adapter_resumes_the_checkpoint_of_a_stored_pause():
    engine = PausingEngine()
    adapter = ng.ProtocolHostAdapter(engine)
    stored = ng.ProtocolInterrupt(
        thread_id="after-restart", checkpoint_id="cp-7", interrupt_id="s2:ask"
    )

    assert asyncio.run(adapter.resume_payload(
        "yes", thread_id="after-restart", interrupt=stored
    )) == "approved"

    name, cfg, checkpoint_id, value, interrupt_id, _ = engine.calls[-1]
    assert (name, cfg.thread_id, checkpoint_id, value, interrupt_id) == (
        "resume_from_async", "after-restart", "cp-7", "yes", "s2:ask",
    )


def test_resume_value_builder_maps_the_answer_for_the_paused_node():
    seen = []

    def builder(answer, interrupt):
        seen.append((answer, interrupt))
        return {"approved": answer in ("approve", "yes"), "command": interrupt.payload}

    engine = PausingEngine()
    adapter = ng.ProtocolHostAdapter(engine, resume_value=builder)

    async def exercise():
        await _collect(adapter.stream("x", thread_id="c", interrupts=True))
        return await adapter.resume_payload("approve", thread_id="c")

    asyncio.run(exercise())

    assert engine.calls[-1][3] == {
        "approved": True, "command": {"command": "rm -rf build"},
    }
    assert seen[0][0] == "approve"
    assert seen[0][1].node == "ask"


def test_a_resumed_turn_can_pause_again_and_the_record_follows():
    second = lambda: _paused(checkpoint_id="cp-2", interrupt_id="s1:ask")
    engine = PausingEngine(resumed=[second(), _done("done")])
    adapter = ng.ProtocolHostAdapter(engine)

    async def exercise():
        await _collect(adapter.stream("x", thread_id="c", interrupts=True))
        again = await _collect(adapter.resume_stream("one", thread_id="c"))
        assert adapter.pending_interrupt("c").checkpoint_id == "cp-2"
        last = await _collect(adapter.resume_stream("two", thread_id="c"))
        return again, last

    again, last = asyncio.run(exercise())

    assert [e.kind for e in again] == ["interrupt"]
    assert [e.kind for e in last] == ["final"]
    assert [call[2] for call in engine.calls if call[0] == "resume_from_async"] == [
        "cp-1", "cp-2",
    ]
    assert adapter.pending_interrupt("c") is None


def test_resume_payload_raises_when_the_resumed_turn_pauses_again():
    engine = PausingEngine(resumed=[_paused(checkpoint_id="cp-2")])
    adapter = ng.ProtocolHostAdapter(engine)

    async def exercise():
        await _collect(adapter.stream("x", thread_id="c", interrupts=True))
        await adapter.resume_payload("one", thread_id="c")

    with pytest.raises(ng.ProtocolInterrupted) as caught:
        asyncio.run(exercise())
    assert caught.value.interrupt.checkpoint_id == "cp-2"


def test_pauses_of_different_threads_do_not_mix():
    checkpoints = iter(["cp-a", "cp-b"])
    engine = PausingEngine(
        pause=lambda: _paused(checkpoint_id=next(checkpoints)),
        resumed=[_done("for b"), _done("for a")],
    )
    adapter = ng.ProtocolHostAdapter(engine)

    async def exercise():
        await _collect(adapter.stream("x", thread_id="a", interrupts=True))
        await _collect(adapter.stream("y", thread_id="b", interrupts=True))
        b = await adapter.resume_payload("go", thread_id="b")
        a = await adapter.resume_payload("go", thread_id="a")
        return a, b

    assert asyncio.run(exercise()) == ("for a", "for b")
    resumed = [call for call in engine.calls if call[0] == "resume_from_async"]
    assert [(call[1].thread_id, call[2]) for call in resumed] == [
        ("b", "cp-b"), ("a", "cp-a"),
    ]


# -- cancellation and withdrawal ---------------------------------------------


def test_cancel_reaches_a_resumed_run_and_keeps_the_pause_for_a_retry():
    class BlockingResume(PausingEngine):
        def __init__(self):
            super().__init__()
            self.started = asyncio.Event()
            self.cancelled = False

        async def resume_from_async(self, cfg, checkpoint_id, *args, **kwargs):
            self.started.set()
            try:
                await asyncio.Future()
            except asyncio.CancelledError:
                self.cancelled = True
                raise

    async def exercise():
        engine = BlockingResume()
        adapter = ng.ProtocolHostAdapter(engine)
        await _collect(adapter.stream("x", thread_id="c", interrupts=True))
        resumed = asyncio.create_task(
            adapter.resume_payload("go", thread_id="c", request_id="t-1")
        )
        await engine.started.wait()
        assert adapter.active_count("t-1") == 1
        assert adapter.cancel("t-1") == 1
        with pytest.raises(asyncio.CancelledError):
            await resumed
        assert engine.cancelled is True
        assert adapter.active_count() == 0
        # The answer was not consumed, so it can be sent again ...
        assert adapter.pending_interrupt("c") is not None
        # ... until the host withdraws the question.
        dropped = adapter.discard_interrupt("c")
        assert dropped.checkpoint_id == "cp-1"
        assert adapter.pending_interrupt("c") is None
        assert adapter.discard_interrupt("c") is None

    asyncio.run(exercise())


def test_closing_a_resumed_stream_cancels_the_engine_call():
    class BlockingResume(PausingEngine):
        def __init__(self):
            super().__init__()
            self.cancelled = False

        async def resume_from_async(self, cfg, checkpoint_id, *args, callback=None, **kwargs):
            callback(_token("answer", "partial"))
            try:
                await asyncio.Future()
            except asyncio.CancelledError:
                self.cancelled = True
                raise

    async def exercise():
        engine = BlockingResume()
        adapter = ng.ProtocolHostAdapter(engine, stream_node="answer")
        await _collect(adapter.stream("x", thread_id="c", interrupts=True))
        stream = adapter.resume_stream("go", thread_id="c", request_id="t-1")
        assert (await stream.__anext__()).data == "partial"
        await stream.aclose()
        await asyncio.sleep(0)
        return engine, adapter

    engine, adapter = asyncio.run(exercise())
    assert engine.cancelled is True
    assert adapter.active_count() == 0


# -- what did not change -----------------------------------------------------


def test_a_graph_that_never_pauses_streams_the_same_events_with_or_without_the_flag():
    class StreamingEngine:
        async def run_stream_async(self, cfg, callback):
            for token in ("hello", " ", "world"):
                callback(_token("answer", token))
            return SimpleNamespace(output=_messages("hello world"))

    async def exercise(**kwargs):
        adapter = ng.ProtocolHostAdapter(StreamingEngine(), stream_node="answer")
        return [
            (e.kind, e.data, e.interrupt)
            async for e in adapter.stream("x", thread_id="s", **kwargs)
        ]

    expected = [
        ("token", "hello", None), ("token", " ", None),
        ("token", "world", None), ("final", "hello world", None),
    ]
    assert asyncio.run(exercise()) == expected
    assert asyncio.run(exercise(interrupts=True)) == expected
    assert ng.ProtocolStreamEvent("final", "x").interrupt is None


def test_a_custom_output_reader_still_sees_a_finished_run_only():
    class Engine:
        async def run_async(self, cfg):
            return SimpleNamespace(output={"answer": "ok"})

    adapter = ng.ProtocolHostAdapter(
        Engine(), output_reader=lambda result: result.output["answer"]
    )
    assert asyncio.run(adapter.run("x", thread_id="s")) == "ok"
    assert adapter.pending_interrupt("s") is None


# -- a real engine ------------------------------------------------------------


class _AskNode(ng.GraphNode):
    """Pauses on a message that starts with "deploy" and answers once it is resumed."""

    def get_name(self):
        return "ask"

    def run(self, input):
        answer = input.ctx.resume_value
        if answer is None:
            messages = input.state.get("messages") or []
            if not messages[-1]["content"].startswith("deploy"):
                return [ng.ChannelWrite(
                    "messages", [{"role": "assistant", "content": "plain"}]
                )]
            raise ng.NodeInterrupt({"command": "deploy"}, reason="Run `deploy`?")
        words = ("ran ", "deploy") if answer == "approve" else ("skipped ", "deploy")
        for word in words:
            if input.stream_cb:
                event = ng.GraphEvent()
                event.type = ng.GraphEvent.Type.LLM_TOKEN
                event.node_name = "ask"
                event.data = word
                input.stream_cb(event)
        return [ng.ChannelWrite(
            "messages", [{"role": "assistant", "content": "".join(words)}]
        )]


def _real_adapter(**kwargs):
    ng.NodeFactory.register_type(
        "protocol_interrupts_ask", lambda name, config, ctx: _AskNode()
    )
    definition = {
        "name": "protocol_interrupts",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"ask": {"type": "protocol_interrupts_ask"}},
        "edges": [
            {"from": ng.START_NODE, "to": "ask"},
            {"from": "ask", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
    return engine, ng.ProtocolHostAdapter(engine, stream_node="ask", **kwargs)


def test_real_graph_pauses_and_the_answer_resumes_it_with_streamed_tokens():
    engine, adapter = _real_adapter()

    async def exercise():
        first = await _collect(adapter.stream(
            "deploy it", thread_id="ctx-1", request_id="t-1", interrupts=True
        ))
        interrupt = first[-1].interrupt
        second = await _collect(adapter.resume_stream(
            "approve", thread_id="ctx-1", request_id="t-1"
        ))
        return first, interrupt, second

    first, interrupt, second = asyncio.run(exercise())

    assert [e.kind for e in first] == ["interrupt"]
    assert interrupt.node == "ask"
    assert interrupt.reason == "Run `deploy`?"
    assert interrupt.payload == {"command": "deploy"}
    assert interrupt.checkpoint_id and interrupt.interrupt_id
    assert [(e.kind, e.data) for e in second] == [
        ("token", "ran "), ("token", "deploy"), ("final", "ran deploy"),
    ]
    messages = engine.get_state("ctx-1")["channels"]["messages"]["value"]
    assert [m["content"] for m in messages] == ["deploy it", "approve", "ran deploy"]


def test_real_graph_run_does_not_report_the_users_text_as_the_answer():
    _, adapter = _real_adapter()

    with pytest.raises(ng.ProtocolInterrupted) as caught:
        asyncio.run(adapter.run("deploy it", thread_id="ctx-2"))

    assert caught.value.interrupt.payload == {"command": "deploy"}


def test_real_graph_resume_value_builder_changes_what_the_node_reads():
    _, adapter = _real_adapter(
        resume_value=lambda answer, interrupt: "approve" if answer == "yes" else "no"
    )

    async def exercise():
        await _collect(adapter.stream("deploy x", thread_id="c", interrupts=True))
        return await adapter.resume_payload("yes", thread_id="c")

    assert asyncio.run(exercise()) == "ran deploy"


def test_real_graph_a_question_that_a_newer_turn_replaced_is_not_answered_with_old_text():
    """The engine returns the last result of a thread that is not paused, so the adapter refuses."""
    _, adapter = _real_adapter()

    async def exercise():
        await _collect(adapter.stream(
            "deploy it", thread_id="ctx-3", request_id="A", interrupts=True
        ))
        # The conversation moves on in another request before the answer arrives.
        assert await adapter.run("hello", thread_id="ctx-3", request_id="B") == "plain"
        assert adapter.pending_interrupt("ctx-3") is None
        return await adapter.resume_payload("approve", thread_id="ctx-3", request_id="A")

    with pytest.raises(LookupError):
        asyncio.run(exercise())
