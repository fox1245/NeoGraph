"""Real localhost TLS calls through prepared typed requests and owned outcomes."""

import asyncio
import gc
import sys
from concurrent.futures import ThreadPoolExecutor

import pytest
import neograph_engine as ng


def _request(provider, mode=ng.ProviderMode.Collect):
    return ng.make_provider_request(provider, "local-model", [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("local request")]),
    ], mode=mode)


def test_prepared_request_is_consumed_once(provider_peer):
    provider = provider_peer.provider()
    prepared = provider.prepare(_request(provider))
    result = provider.dispatch(prepared)
    assert result.failure is None
    assert result.completion.messages[0].parts[0].value == "ok"
    with pytest.raises((ValueError, RuntimeError)):
        provider.dispatch(prepared)
    assert len(provider_peer.requests) == 1


def test_owned_outcome_keeps_raw_and_native_history_after_provider_collection(provider_peer):
    provider = provider_peer.provider()
    request = _request(provider)
    result = provider.invoke(request)
    messages = list(request.messages) + result.completion.messages
    raw = next(event for event in result.completion.raw_events
               if event.type == "chat.completion")
    del provider, result
    gc.collect()
    assert messages[-1].parts[0].value == "ok"
    assert raw.payload["id"] == "local-completion"
    assert raw.payload["choices"][0]["message"]["content"] == "ok"
    assert messages[-1].native is not None
    provider = provider_peer.provider()
    omitted_prefix = ng.make_provider_request(provider, "local-model", [messages[-1]])
    rejected = provider.invoke(omitted_prefix)
    assert rejected.completion is None
    assert rejected.failure.error.kind == ng.ProviderErrorKind.ReplayIneligible
    assert len(provider_peer.requests) == 1
    replay = ng.make_provider_request(provider, "local-model", messages)
    assert provider.invoke(replay).failure is None
    assert len(provider_peer.requests) == 2
    assert [(message["role"], message["text"])
            for message in provider_peer.logical_requests[-1]] == [
        ("user", "local request"), ("assistant", "ok"),
    ]


def test_remote_failure_is_owned_failure_not_completion(provider_peer):
    provider_peer.status = 429
    provider_peer.error = {"error": {"message": "fixture quota", "type": "rate_limit_error",
                                    "code": "rate_limit_exceeded"}}
    provider = provider_peer.provider()
    outcome = provider.invoke(_request(provider))
    assert outcome.completion is None
    assert outcome.failure.error.http_status == 429
    assert outcome.failure.partial.usage.total is None
    assert len(provider_peer.requests) == 1


def test_stream_retains_typed_events_and_terminal_usage(provider_peer):
    provider_peer.stream_chunks = ["one", "-", "two"]
    provider = provider_peer.provider()
    request = _request(provider, ng.ProviderMode.Stream)
    events = []
    request.on_event = events.append
    outcome = provider.invoke(request)
    assert outcome.failure is None
    assert outcome.completion.messages[0].parts[0].value == "one-two"
    assert outcome.completion.usage.input_total.value == 10
    assert outcome.completion.usage.output_total.value == 5
    assert outcome.completion.usage.total.value == 15
    del request, provider
    gc.collect()
    deltas = [event for event in events if event.kind == "PartDelta"]
    assert [event.value.bytes for event in deltas] == ["one", "-", "two"]


def test_observer_error_retains_the_drained_outcome(provider_peer):
    provider = provider_peer.provider()
    request = _request(provider, ng.ProviderMode.Stream)
    refusal = ValueError("application observer refusal")

    def reject(_event):
        raise refusal

    request.on_event = reject
    with pytest.raises(ng.ProviderObserverError) as caught:
        provider.invoke(request)
    assert caught.value.outcome is not None
    assert caught.value.cause is refusal
    assert len(provider_peer.requests) == 1


def test_cancel_token_interrupts_blocked_transport(provider_peer):
    provider_peer.release.clear()
    provider = provider_peer.provider()
    request = _request(provider)
    token = ng.CancelToken()
    request.cancel_token = token

    async def exercise():
        call = asyncio.create_task(asyncio.to_thread(provider.invoke, request))
        try:
            assert await asyncio.to_thread(provider_peer.started.wait, 2)
            token.cancel()
            outcome = await asyncio.wait_for(call, 2)
            assert outcome.completion is None
            assert outcome.failure.error.kind == ng.ProviderErrorKind.Cancelled
        finally:
            provider_peer.release.set()
            if not call.done():
                await call

    asyncio.run(exercise())


@pytest.mark.parametrize("stream", [False, True])
def test_graph_async_cancellation_stops_real_provider_call(provider_peer, stream):
    provider_peer.release.clear()
    graph = {
        "name": "local-cancel", "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "llm_call"}},
        "edges": [{"from": ng.START_NODE, "to": "llm"},
                  {"from": "llm", "to": ng.END_NODE}],
    }
    engine = ng.GraphEngine.compile(
        graph, ng.NodeContext(provider=provider_peer.provider(), model="local-model"))

    async def exercise():
        cfg = ng.RunConfig(thread_id="async-local-cancel")
        cfg.input = {"messages": [{"role": "user", "content": "wait for cancellation"}]}
        pending = asyncio.ensure_future(
            engine.run_stream_async(cfg, lambda _event: None) if stream else
            engine.run_async(cfg))
        try:
            assert await asyncio.to_thread(provider_peer.started.wait, 2)
            pending.cancel()
            with pytest.raises(asyncio.CancelledError):
                await asyncio.wait_for(pending, 2)
        finally:
            provider_peer.release.set()
            if not pending.done():
                pending.cancel()
                try:
                    await pending
                except asyncio.CancelledError:
                    pass

    asyncio.run(exercise())


def test_cancellation_after_preparation_prevents_transport(provider_peer):
    provider = provider_peer.provider()
    request = _request(provider)
    token = ng.CancelToken()
    request.cancel_token = token
    prepared = provider.prepare(request)
    token.cancel()
    outcome = provider.dispatch(prepared)
    assert outcome.completion is None
    assert outcome.failure.error.kind == ng.ProviderErrorKind.Cancelled
    assert provider_peer.requests == []


def test_graph_checkpoint_continuation_keeps_native_messages_and_outcomes(provider_peer):
    provider_peer.reply = "first answer"
    graph = {
        "name": "native-checkpoint", "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "llm_call"}},
        "edges": [{"from": ng.START_NODE, "to": "llm"},
                  {"from": "llm", "to": ng.END_NODE}],
    }
    engine = ng.GraphEngine.compile(
        graph, ng.NodeContext(provider=provider_peer.provider(), model="local-model"),
        ng.InMemoryCheckpointStore())
    first_config = ng.RunConfig(thread_id="native-history")
    first_config.provider_messages = [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("first")]),
    ]
    first = engine.run(first_config)
    assert first.native_messages[-1].native is not None
    assert first.provider_outcomes[0].completion.messages[0].native is not None
    continuation = list(first.native_messages) + [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("second")]),
    ]
    del first
    gc.collect()
    provider_peer.reply = "second answer"
    next_config = ng.RunConfig(thread_id="native-history", resume_if_exists=True)
    next_config.provider_messages = continuation
    second = engine.run(next_config)
    assert len(provider_peer.requests) == 2
    assert [(message["role"], message["text"])
            for message in provider_peer.logical_requests[-1]] == [
        ("user", "first"), ("assistant", "first answer"), ("user", "second"),
    ]
    assert [outcome.text for outcome in second.provider_outcomes] == [
        "first answer", "second answer",
    ]
    assert [(message.role, message.parts[0].value)
            for message in second.native_messages] == [
        (ng.ProviderRole.User, "first"),
        (ng.ProviderRole.Assistant, "first answer"),
        (ng.ProviderRole.User, "second"),
        (ng.ProviderRole.Assistant, "second answer"),
    ]
    assert second.native_messages[-1].native is not None
    assert second.provider_outcomes[-1].completion.usage.total.value == 15
    assert second.usage.total.value == sum(
        outcome.usage.total.value for outcome in second.provider_outcomes)


def test_reused_tls_socket_delivers_each_distinct_request(provider_peer):
    provider = provider_peer.provider(max_host_connections=1)
    for turn in range(8):
        request = ng.make_provider_request(provider, "local-model", [
            ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(f"turn-{turn}")]),
        ])
        assert provider.invoke(request).text == "ok"
    assert [request[0]["text"] for request in provider_peer.logical_requests] == [
        f"turn-{turn}" for turn in range(8)
    ]
    assert len(set(provider_peer.clients)) == 1


def test_large_post_resumes_after_peer_starts_reading(provider_peer):
    text = "transport-output-boundary-" * 170000
    provider_peer.reply = text
    provider_peer.allow_body_read.clear()
    provider = provider_peer.provider(timeout_ms=15000, resource_bytes=8 * 1024 * 1024)
    request = ng.make_provider_request(provider, "local-model", [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(text)]),
    ])
    prepared = provider.prepare(request)
    assert prepared.valid, prepared.error.safe_message if prepared.error is not None else "invalid admission"
    with ThreadPoolExecutor(max_workers=1) as workers:
        pending = workers.submit(provider.dispatch, prepared)
        try:
            assert provider_peer.headers_seen.wait(5)
            assert not pending.done()
        finally:
            provider_peer.allow_body_read.set()
        outcome = pending.result(timeout=15)
    assert outcome.failure is None
    assert outcome.text == text
    assert provider_peer.logical_requests[0][0]["text"] == text


def test_large_post_does_not_stall_when_the_send_buffer_fills(provider_peer):
    # On Windows, libcurl's write wait relied on FD_WRITE, which Winsock records only after a send()
    # has failed with WSAEWOULDBLOCK. A send that succeeded but left the socket full produced no event,
    # so about 4-6% of 4 MB uploads stopped (typically at 131 KB or 197 KB) until the deadline although
    # the peer was reading. One request catches that rarely, so Windows repeats it on fresh providers
    # and connections; the other platforms wait on level-triggered readiness and run it a few times.
    rounds = 150 if sys.platform == "win32" else 3
    text = "transport-output-boundary-" * 170000
    provider_peer.reply = "ok"
    for index in range(rounds):
        provider = provider_peer.provider(timeout_ms=6000, resource_bytes=8 * 1024 * 1024)
        request = ng.make_provider_request(provider, "local-model", [
            ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(text)]),
        ])
        prepared = provider.prepare(request)
        assert prepared.valid, prepared.error.safe_message if prepared.error is not None else "invalid admission"
        outcome = provider.dispatch(prepared)
        assert outcome.failure is None, f"round {index}: {outcome.failure.error.kind}"
        assert outcome.text == "ok"
    assert len(provider_peer.logical_requests) == rounds
    assert provider_peer.logical_requests[-1][0]["text"] == text
def test_more_than_64_tls_sockets_make_progress_together(provider_peer):
    width = 72
    provider_peer.release.clear()
    provider = provider_peer.provider(
        max_operations=width, max_host_connections=width, timeout_ms=15000)
    requests = [
        ng.make_provider_request(provider, "local-model", [
            ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(f"socket-{index}")]),
        ]) for index in range(width)
    ]
    with ThreadPoolExecutor(max_workers=width) as workers:
        pending = [workers.submit(provider.invoke, request) for request in requests]
        try:
            with provider_peer.changed:
                all_started = provider_peer.changed.wait_for(
                    lambda: len(provider_peer.requests) == width, timeout=10)
            assert all_started, f"only {len(provider_peer.requests)} concurrent requests reached the peer"
            assert len(set(provider_peer.clients)) == width
        finally:
            provider_peer.release.set()
        outcomes = [future.result(timeout=15) for future in pending]
    assert all(outcome.failure is None and outcome.text == "ok" for outcome in outcomes)
    assert {request[0]["text"] for request in provider_peer.logical_requests} == {
        f"socket-{index}" for index in range(width)
    }


def test_expired_prepared_deadline_prevents_transport(provider_peer):
    provider = provider_peer.provider()
    request = _request(provider)
    request.timeout_ms = 0
    prepared = provider.prepare(request)
    outcome = provider.dispatch(prepared)
    assert outcome.failure.error.kind == ng.ProviderErrorKind.DeadlineExceeded
    assert request.timeout_ms == 0
    assert provider_peer.requests == []


@pytest.mark.parametrize("timeout_ms", [-1, 2**63 - 1])
def test_unrepresentable_deadline_is_rejected_before_preparation(provider_peer, timeout_ms):
    provider = provider_peer.provider()
    request = _request(provider)
    with pytest.raises(ValueError):
        request.timeout_ms = timeout_ms
    assert provider_peer.requests == []


def test_repeated_observer_cause_reads_preserve_original_exception_and_traceback(provider_peer):
    provider = provider_peer.provider()
    for _ in range(2):
        refusal = LookupError("observer retains original traceback")
        request = _request(provider, ng.ProviderMode.Stream)

        def reject(_event):
            raise refusal

        request.on_event = reject
        with pytest.raises(ng.ProviderObserverError) as caught:
            provider.invoke(request)
        error = caught.value
        original_traceback = refusal.__traceback__
        assert original_traceback is not None
        first = error.cause
        second = error.cause
        assert first is second is refusal
        assert first.__traceback__ is second.__traceback__ is original_traceback
        assert error.__cause__ is refusal
        assert error.outcome is not None
    assert len(provider_peer.requests) == 2
