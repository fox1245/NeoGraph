"""SchemaProvider's endpoints on Provider (#531) from Python, against a loopback peer: the typed requests and
results, prepared calls, the async form and its cancellation, and the strict ``from_dict`` of the requests."""

import asyncio
import base64
import gc
import hashlib
import hmac
import json
import select
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
import neograph_engine as ng
from neograph_engine import llm as nglm
from neograph_engine.provider_endpoints import FIELDS

PNG = ("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==")
PATHS = {"openai.chat": "/v1/chat/completions", "openai.responses": "/v1/responses"}
OPENAI, OPENROUTER = "https://api.openai.com", "https://openrouter.ai"


def _closed(connection):
    """Whether the client has closed the connection (a cancelled call closes it)."""
    readable, _, _ = select.select([connection], [], [], 0)
    if not readable:
        return False
    try:
        return connection.recv(1, socket.MSG_PEEK) == b""
    except OSError:
        return True


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def _answer(self):
        peer = self.server.peer
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length)
        with peer.changed:
            peer.requests.append({"method": self.command, "path": self.path,
                                  "authorization": self.headers.get("Authorization"),
                                  "body": json.loads(raw) if raw else None})
            peer.changed.notify_all()
        if peer.hold:
            while not peer.released.wait(0.02):
                if _closed(self.connection):
                    peer.disconnected.set()
                    return
        payload = peer.body if isinstance(peer.body, bytes) else json.dumps(peer.body).encode()
        try:
            self.send_response(peer.status)
            self.send_header("Content-Type", peer.content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # A cancelled client closes the socket before the peer replies.

    do_POST = do_GET = do_DELETE = _answer


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    # socketserver's backlog of 5 refuses some of eight concurrent connects on Windows.
    request_queue_size = 64

    def handle_error(self, request, client_address):
        pass  # A cancelled call closes its connection while the peer still holds or answers the request.


class _Peer:
    """A plain-HTTP loopback peer that answers every request with `body`, or holds it until released."""

    def __init__(self):
        self.requests = []
        self.changed = threading.Condition()
        self.body = {}
        self.status = 200
        self.content_type = "application/json"
        self.hold = False
        self.released = threading.Event()
        self.disconnected = threading.Event()
        self.server = _Server(("127.0.0.1", 0), _Handler)
        self.server.peer = self
        self.origin = f"http://127.0.0.1:{self.server.server_port}"
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()

    def await_requests(self, count, timeout=5.0):
        with self.changed:
            return self.changed.wait_for(lambda: len(self.requests) >= count, timeout)

    def close(self):
        self.released.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


@pytest.fixture
def peer():
    peer = _Peer()
    try:
        yield peer
    finally:
        peer.close()


def _policy_at(origin, anchor=OPENAI):
    """The built-in policy, with every list that names `anchor` also naming `origin`."""
    text = json.dumps(json.loads(ng.provider_policy_json()), separators=(",", ":"))
    named = f'"{anchor}"'
    return ng.load_provider_policy(text.replace(named, f'{named},"{origin}"'), ng.provider_codec_defaults_json())


def _descriptor(origin, family="openai.chat", anchor=OPENAI):
    """A descriptor at `origin` under a policy that admits `anchor`'s endpoints there; no anchor, the stock policy."""
    source = {"descriptor_version": 1, "revision": 1, "id": "python-endpoints", "family": family,
              "connection": {"base_url": origin, "paths": {"buffered": PATHS[family], "streaming": PATHS[family]}}}
    return ng.load_provider_descriptor(json.dumps(source), _policy_at(origin, anchor) if anchor else None)


def _provider(peer, family="openai.chat", anchor=OPENAI, max_operations=None):
    options = ng.ProviderRuntimeOptions(api_key="fixture-key", default_timeout_ms=5000)
    options.retry_tokens = 0
    options.retry_tokens_per_second = 0
    if max_operations is not None:
        limits = ng.ProviderRuntimeLimits()
        limits.max_operations = max_operations
        options.limits = limits
    return nglm.SchemaProvider(_descriptor(peer.origin, family, anchor), options)


def _embedding_body():
    return {"object": "list", "model": "fixture-embed",
            "data": [{"object": "embedding", "index": 0, "embedding": [0.25, -0.5]}],
            "usage": {"prompt_tokens": 3, "total_tokens": 3}}


def _embed(text="hello"):
    return ng.EmbeddingsRequest.from_dict({"model": "fixture-embed", "inputs": [{"text": text}]})


def _expect_vector(result):
    assert isinstance(result, ng.EmbeddingsResult), getattr(getattr(result, "error", None), "safe_message", result)
    assert result.vectors == [[0.25, -0.5]]
    assert result.usage.input_total.value == 3


async def _complete_async(provider, request):
    return await provider.complete_endpoint_async(request)


def _user(text="ping"):
    return ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(text)])


def _chat_request(provider):
    controls = ng.ProviderControls()
    controls.max_output_tokens = 64
    if provider.family() != "openai.chat":
        controls.account_scope = "fixture-account"
    return ng.make_provider_request(provider, "fixture-model", [_user()], [], controls)


def _stock_provider(family):
    """A provider at the family's public origin; nothing is sent."""
    origins = {"google.interactions": "https://generativelanguage.googleapis.com",
               "openai.responses": OPENAI, "openai.chat": OPENAI}
    paths = {"google.interactions": "/v1beta/interactions", **PATHS}
    source = {"descriptor_version": 1, "revision": 1, "id": "python-stock", "family": family,
              "connection": {"base_url": origins[family], "paths": {"buffered": paths[family],
                                                                   "streaming": paths[family]}}}
    return nglm.SchemaProvider(ng.load_provider_descriptor(json.dumps(source)), ng.ProviderRuntimeOptions())


class _NoEndpoints(ng.Provider):
    def __init__(self):
        super().__init__("openai.chat")

    def get_name(self):
        return "no-endpoints"

    def prepare(self, request):
        raise NotImplementedError


def test_embeddings_complete_on_the_providers_client_with_its_credential(peer):
    peer.body = _embedding_body()
    result = _provider(peer).complete_endpoint(_embed())
    _expect_vector(result)
    assert [r["path"] for r in peer.requests] == ["/v1/embeddings"]
    assert peer.requests[0]["authorization"] == "Bearer fixture-key"
    assert peer.requests[0]["body"]["model"] == "fixture-embed"


def test_images_come_back_as_media(peer):
    peer.body = {"created": 1, "output_format": "png", "data": [{"b64_json": PNG}],
                 "usage": {"input_tokens": 5, "output_tokens": 7, "total_tokens": 12}}
    request = ng.ImagesRequest()
    request.model, request.prompt, request.output_format = "fixture-image", "a square", "png"
    result = _provider(peer).complete_endpoint(request)
    assert isinstance(result, ng.ImagesResult)
    assert [image.kind for image in result.images] == [ng.ProviderMediaKind.Image]
    assert result.images[0].mime == "image/png"
    assert result.usage.total.value == 12
    assert peer.requests[0]["path"] == "/v1/images/generations"
    assert peer.requests[0]["body"]["prompt"] == "a square"


def test_async_completion_resolves_an_asyncio_future(peer):
    peer.body = _embedding_body()
    provider = _provider(peer)

    async def scenario():
        future = provider.complete_endpoint_async(_embed(), timeout_seconds=5)
        assert isinstance(future, asyncio.Future)
        first = await future
        prepared = provider.prepare_endpoint(_embed("again"))
        return first, await provider.complete_endpoint_async(prepared)

    first, second = asyncio.run(scenario())
    _expect_vector(first)
    _expect_vector(second)
    assert [r["body"]["input"] for r in peer.requests] == [["hello"], ["again"]]


def test_async_calls_overlap_and_outlive_their_provider(peer):
    peer.body = _embedding_body()

    async def scenario():
        provider = _provider(peer)
        futures = [provider.complete_endpoint_async(_embed(f"text {n}")) for n in range(8)]
        del provider  # each awaitable owns the client and its call
        gc.collect()
        return await asyncio.gather(*futures)

    results = asyncio.run(scenario())
    assert len(results) == 8
    for result in results:
        _expect_vector(result)
    assert sorted(r["body"]["input"][0] for r in peer.requests) == [f"text {n}" for n in range(8)]


def test_async_completion_needs_a_running_loop(peer):
    with pytest.raises(RuntimeError, match="no running event loop"):
        _provider(peer).complete_endpoint_async(_embed())


def test_a_prepared_call_names_its_endpoint_and_model_and_completes_once(peer):
    peer.body = _embedding_body()
    provider = _provider(peer)
    prepared = provider.prepare_endpoint(_embed())
    assert (prepared.endpoint, prepared.model, prepared.error) == ("embeddings", "fixture-embed", None)
    assert not prepared.consumed and not peer.requests
    _expect_vector(provider.complete_endpoint(prepared))
    assert prepared.consumed and prepared.endpoint == "embeddings"
    with pytest.raises(ValueError, match="already been completed"):
        provider.complete_endpoint(prepared)
    with pytest.raises(ValueError, match="already been completed"):
        asyncio.run(_complete_async(provider, prepared))
    assert len(peer.requests) == 1


def test_the_stock_policy_refuses_an_endpoint_at_the_loopback_origin(peer):
    peer.body = _embedding_body()
    provider = _provider(peer, anchor=None)
    prepared = provider.prepare_endpoint(_embed())
    assert isinstance(prepared.error, ng.ProviderError)
    failure = provider.complete_endpoint(prepared)
    assert isinstance(failure, ng.ProviderFailure)
    assert failure.error.kind == prepared.error.kind
    assert isinstance(provider.complete_endpoint(_embed()), ng.ProviderFailure)
    assert not peer.requests


def test_a_remote_failure_is_returned_as_a_failure(peer):
    peer.status = 429
    peer.body = {"error": {"message": "fixture quota", "type": "rate_limit_error", "code": "rate_limit_exceeded"}}
    failure = _provider(peer).complete_endpoint(_embed())
    assert isinstance(failure, ng.ProviderFailure)
    assert failure.error.http_status == 429 and failure.error.kind == ng.ProviderErrorKind.RateLimited


def test_timeout_seconds_is_the_deadline_of_the_call(peer):
    peer.hold = True
    provider = _provider(peer)
    started = time.monotonic()
    failure = provider.complete_endpoint(_embed(), timeout_seconds=0.3)
    assert time.monotonic() - started < 3
    assert isinstance(failure, ng.ProviderFailure) and failure.error.kind == ng.ProviderErrorKind.DeadlineExceeded
    peer.released.set()
    for bad in (-1, float("nan"), float("inf")):
        with pytest.raises(ValueError, match="timeout_seconds"):
            provider.prepare_endpoint(_embed(), timeout_seconds=bad)
    prepared = provider.prepare_endpoint(_embed(), timeout_seconds=5)
    with pytest.raises(ValueError, match="when a call is prepared"):
        provider.complete_endpoint(prepared, timeout_seconds=1)


def test_cancelling_the_awaiting_task_cancels_the_call(peer):
    peer.hold = True
    provider = _provider(peer)

    async def call():
        return await provider.complete_endpoint_async(_embed())

    async def scenario():
        task = asyncio.create_task(call())
        assert await asyncio.get_running_loop().run_in_executor(None, peer.await_requests, 1)
        await asyncio.sleep(0.2)
        assert not peer.disconnected.is_set()
        started = time.monotonic()
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        return time.monotonic() - started

    elapsed = asyncio.run(scenario())
    assert elapsed < 2
    # The SDK call was cancelled, not only the future: the client closes its connection to the held peer, long
    # before the call's 5 s deadline.
    assert peer.disconnected.wait(2)
    peer.released.set()


def test_an_awaited_call_does_not_block_the_loop(peer):
    peer.hold = True
    provider = _provider(peer)

    async def scenario():
        future = provider.complete_endpoint_async(_embed())
        assert await asyncio.get_running_loop().run_in_executor(None, peer.await_requests, 1)
        ticks = 0
        for _ in range(10):  # the loop keeps running while the call is held at the peer
            await asyncio.sleep(0.02)
            ticks += 1
        assert ticks == 10 and not future.done()
        peer.body = _embedding_body()
        peer.released.set()
        return await future

    _expect_vector(asyncio.run(scenario()))


def test_a_provider_without_endpoints_refuses_them():
    provider = _NoEndpoints()
    for call in (provider.prepare_endpoint, provider.complete_endpoint):
        with pytest.raises(RuntimeError, match="no SchemaProvider endpoints"):
            call(_embed())

    async def scenario():
        provider.complete_endpoint_async(_embed())

    with pytest.raises(RuntimeError, match="no SchemaProvider endpoints"):
        asyncio.run(scenario())


def test_only_endpoint_requests_and_prepared_calls_are_taken(peer):
    provider = _provider(peer)
    for call in (provider.prepare_endpoint, provider.complete_endpoint):
        with pytest.raises(TypeError, match="expected an endpoint request"):
            call(ng.ProviderControls())
    with pytest.raises(TypeError, match="expected an endpoint request"):
        provider.prepare_endpoint(provider.prepare_endpoint(_embed()))


def test_no_free_slot_refuses_the_preparation(peer):
    provider = _provider(peer, max_operations=1)
    held = provider.prepare_endpoint(_embed())
    with pytest.raises(RuntimeError, match="admission rejected"):
        provider.prepare_endpoint(_embed())
    # A call that finds no slot is a failure outcome, in both forms.
    failure = provider.complete_endpoint(_embed())
    assert isinstance(failure, ng.ProviderFailure) and failure.error.kind == ng.ProviderErrorKind.ResourceLimit
    assert isinstance(asyncio.run(_complete_async(provider, _embed())), ng.ProviderFailure)
    assert not peer.requests
    del held


def test_moderation_results_keep_their_pairs(peer):
    provider = _provider(peer)
    peer.body = {"id": "modr-1", "model": "omni-moderation-latest", "results": [
        {"flagged": True, "categories": {"violence": True, "hate": False},
         "category_scores": {"violence": 0.9, "hate": 0.01}}]}
    moderation = ng.ModerationRequest.from_dict({"model": "omni-moderation-latest", "inputs": [{"text": "x"}]})
    result = provider.complete_endpoint(moderation)
    assert isinstance(result, ng.ModerationResult), result.error.safe_message
    verdict = result.verdicts[0]
    assert verdict.flagged and result.id == "modr-1"
    assert dict(verdict.categories) == {"violence": True, "hate": False}
    assert dict(verdict.scores) == {"violence": 0.9, "hate": 0.01}
    assert peer.requests[-1]["path"] == "/v1/moderations"


def test_decision_answers_keep_their_probabilities(peer):
    peer.body = {"model": "fixture-decider", "answers": [
        {"type": "predicate", "name": "is_bug", "probability": 0.93},
        {"type": "choice", "name": "team", "choice": "payments", "confidence": 0.88,
         "probabilities": [{"value": "payments", "probability": 0.88}, {"value": "frontend", "probability": 0.12}]},
        {"type": "refusal", "name": None}],
        "usage": {"input_tokens": 42, "output_tokens": 0, "total_tokens": 42}}
    request = ng.DecisionsRequest.from_dict({"model": "fixture-decider", "input": "The checkout crashes.", "questions": [
        {"name": "is_bug", "kind": "Predicate", "instructions": "Is this a bug?"},
        {"name": "team", "kind": "Choice", "instructions": "Which team?", "options": [
            {"value": "payments"}, {"value": "frontend"}]}]})
    result = _provider(peer).complete_endpoint(request)
    assert isinstance(result, ng.DecisionsResult), result.error.safe_message
    predicate, choice, refusal = result.answers
    assert (predicate.name, predicate.kind, predicate.probability) == ("is_bug", ng.DecisionKind.Predicate, 0.93)
    assert (choice.choice, choice.confidence) == ("payments", 0.88)
    assert choice.probabilities == [("payments", 0.88), ("frontend", 0.12)]
    assert choice.raw["type"] == "choice"
    assert refusal.refused and refusal.kind is None
    assert result.usage.input_total.value == 42
    assert peer.requests[0]["path"] == "/v1/decisions"


def test_speech_is_inline_audio(peer):
    audio = b"ID3\x03\x00\x00\x00\x00\x00\x00fixture"
    peer.body, peer.content_type = audio, "audio/mpeg"
    request = ng.SpeechRequest.from_dict({"model": "fixture-voice", "input": "Hello there.", "voice": "alloy",
                                          "format": "mp3", "speed": 1.25})
    result = _provider(peer).complete_endpoint(request)
    assert isinstance(result, ng.SpeechResult), result.error.safe_message
    assert (result.audio.kind, result.audio.mime) == (ng.ProviderMediaKind.Audio, "audio/mpeg")
    assert base64.b64decode(result.audio.data) == audio
    assert peer.requests[0]["path"] == "/v1/audio/speech"
    assert peer.requests[0]["body"]["speed"] == 1.25


def test_rerank_and_generation_statistics_at_an_openrouter_origin(peer):
    provider = _provider(peer, anchor=OPENROUTER)
    peer.body = {"id": "gen-rr-1", "model": "fixture-rerank", "provider": "Fixture", "results": [
        {"index": 1, "relevance_score": 0.97, "document": {"text": "b"}},
        {"index": 0, "relevance_score": 0.2, "document": {"text": "a"}}], "usage": {"search_units": 1}}
    request = ng.RerankRequest.from_dict({"model": "fixture-rerank", "query": "q", "documents": ["a", "b"], "top": 2})
    result = provider.complete_endpoint(request)
    assert isinstance(result, ng.RerankResult), result.error.safe_message
    assert [(r.index, r.relevance) for r in result.ranked] == [(1, 0.97), (0, 0.2)]
    assert (result.id, result.provider) == ("gen-rr-1", "Fixture")
    assert peer.requests[0]["path"] == "/api/v1/rerank" and peer.requests[0]["body"]["top_n"] == 2
    peer.body = {"data": {"created_at": "2026-10-19T14:41:35.908Z", "model": "fixture/model", "streamed": True,
                          "cancelled": False, "latency": 781, "generation_time": 807, "tokens_prompt": 2,
                          "tokens_completion": 1, "native_tokens_prompt": 3, "native_tokens_completion": 1,
                          "finish_reason": "stop", "native_finish_reason": "STOP", "id": "gen-1",
                          "upstream_id": "up-1", "provider_name": "Fixture", "total_cost": 2.25e-06}}
    stats = provider.complete_endpoint(ng.GenerationGet.from_dict({"id": "gen-1"}))
    assert isinstance(stats, ng.GenerationResult), stats.error.safe_message
    assert (stats.id, stats.model, stats.provider, stats.upstream_id) == ("gen-1", "fixture/model", "Fixture", "up-1")
    assert (stats.tokens_prompt, stats.latency_ms, stats.generation_ms) == (2, 781, 807)
    assert stats.streamed and not stats.cancelled and stats.finish_reason == "stop"
    assert stats.usage.input_total.value == 3 and stats.raw["data"]["id"] == "gen-1"
    assert peer.requests[1]["path"] == "/api/v1/generation?id=gen-1"


def test_a_video_job_is_submitted_polled_and_downloaded(peer):
    provider = _provider(peer, anchor=OPENROUTER)
    peer.status = 202
    peer.body = {"id": "job-1", "status": "pending"}
    submit = ng.VideoRequest.from_dict({"model": "fixture-video", "prompt": "A fox.", "duration": 1})
    state = provider.complete_endpoint(submit)
    assert isinstance(state, ng.JobState), state.error.safe_message
    assert state.status == ng.JobStatus.Pending and state.raw_status == "pending"
    assert (state.handle.endpoint, state.handle.origin, state.handle.id, state.handle.model) == (
        "videos", peer.origin, "job-1", "fixture-video")
    assert peer.requests[0]["path"] == "/api/v1/videos" and peer.requests[0]["body"]["prompt"] == "A fox."
    # A host journals the four members and rebuilds the handle to poll after a restart.
    kept = {name: getattr(state.handle, name) for name in ("endpoint", "origin", "id", "model")}
    peer.status, peer.body = 200, {"id": "job-1", "status": "completed", "unsigned_urls": ["u"]}
    polled = provider.complete_endpoint(ng.JobPoll.from_dict({"handle": kept}))
    assert polled.status == ng.JobStatus.Completed and polled.outputs == 1
    assert (peer.requests[1]["method"], peer.requests[1]["path"]) == ("GET", "/api/v1/videos/job-1")
    video = b"\x00\x00\x00\x18ftypmp42synthetic"
    peer.body, peer.content_type = video, "video/mp4"
    content = provider.complete_endpoint(ng.JobDownload.from_dict({"handle": polled.handle, "index": 0}))
    assert isinstance(content, ng.JobContent), content.error.safe_message
    assert content.media.kind == ng.ProviderMediaKind.Video and base64.b64decode(content.media.data) == video
    assert (content.bytes, content.path, content.content_type) == (len(video), "", "video/mp4")
    assert content.sha256 == hashlib.sha256(video).hexdigest()
    assert peer.requests[2]["path"] == "/api/v1/videos/job-1/content?index=0"


def test_files_keep_their_state_and_become_media(peer):
    provider = _provider(peer)
    peer.body = {"id": "file-fixture", "object": "file", "bytes": 3, "created_at": 1, "filename": "a.txt",
                 "purpose": "user_data", "status": "processed"}
    file = provider.complete_endpoint(ng.FileGet.from_dict({"id": "file-fixture"}))
    assert isinstance(file, ng.ProviderFile), file.error.safe_message
    assert (file.id, file.name, file.bytes, file.purpose) == ("file-fixture", "a.txt", 3, "user_data")
    assert file.state == ng.FileState.Active and file.raw["id"] == "file-fixture"
    assert (peer.requests[-1]["method"], peer.requests[-1]["path"]) == ("GET", "/v1/files/file-fixture")
    media = ng.files_media(file, peer.origin, 100)
    assert (media.source, media.reference) == (ng.ProviderMediaSource.File, "file-fixture")
    with pytest.raises(ValueError):
        ng.files_media(file, "https://other.example", 100)
    peer.body = {"id": "file-fixture", "object": "file.deleted", "deleted": True}
    deleted = provider.complete_endpoint(ng.FileDelete.from_dict({"id": "file-fixture"}))
    assert isinstance(deleted, ng.FileDeleted) and deleted.id == "file-fixture"
    assert peer.requests[-1]["method"] == "DELETE"


def test_a_file_source_holds_bytes_or_a_file_slice():
    source = ng.FileSource()
    assert source.bytes is None and source.path == "" and source.size == 0
    source.bytes = b"abc\x00"
    assert source.bytes == b"abc\x00"
    source.bytes = None
    source.path, source.offset, source.size = "/tmp/part.bin", 4, 8
    assert (source.bytes, source.path, source.offset, source.size) == (None, "/tmp/part.bin", 4, 8)
    with pytest.raises(TypeError):
        source.bytes = "text"
    built = ng.FileUpload.from_dict({"source": {"bytes": bytearray(b"xyz")}, "filename": "a.bin",
                                     "mime": "application/octet-stream", "purpose": "user_data"})
    assert built.source.bytes == b"xyz" and built.filename == "a.bin"


def test_a_job_handle_and_an_upload_session_are_plain_values():
    handle = ng.JobPoll.from_dict({"handle": {"endpoint": "videos", "origin": "https://openrouter.ai", "id": "job-1",
                                              "model": "fixture-video"}}).handle
    assert (handle.endpoint, handle.id) == ("videos", "job-1")
    session = ng.FileUploadSession.from_dict({"origin": "https://api.openai.com", "shape": "uploads", "id": "upload-1",
                                              "size": 10, "sent": 4, "parts": ["part-1"], "expires_at": 7})
    assert (session.id, session.sent, session.parts, session.expires_at) == ("upload-1", 4, ["part-1"], 7)
    part = ng.FileUploadPart.from_dict({"session": session, "source": {"bytes": b"abcdef"}})
    assert part.session.id == "upload-1" and part.source.bytes == b"abcdef"


def test_batch_lines_are_encoded_and_decoded_as_the_family_would():
    provider = _stock_provider("openai.chat")
    request = _chat_request(provider)
    item = ng.BatchItem.from_dict({"custom_id": "first", "request": request})
    assert item.custom_id == "first"
    with pytest.raises(AttributeError):
        item.request
    descriptor = _descriptor(OPENAI, anchor=None)
    lines = ng.batch_jsonl(descriptor, [item]).splitlines()
    assert len(lines) == 1
    line = json.loads(lines[0])
    assert line["custom_id"] == "first" and line["url"] == "/v1/chat/completions"
    assert line["body"]["model"] == "fixture-model"
    result = json.dumps({"id": "batch_req_1", "custom_id": "first", "response": {"status_code": 200, "body": {
        "id": "chat-1", "object": "chat.completion", "created": 1, "model": "fixture-model",
        "choices": [{"index": 0, "message": {"role": "assistant", "content": "pong"}, "finish_reason": "stop"}],
        "usage": {"prompt_tokens": 3, "completion_tokens": 2, "total_tokens": 5}}}, "error": None})
    assert ng.batch_custom_id(result) == "first" and ng.batch_custom_id("{}") is None
    for original in (request, item):
        decoded = ng.batch_decode_line(descriptor, original, result)
        assert isinstance(decoded, ng.BatchLine) and decoded.custom_id == "first"
        assert decoded.outcome.failure is None and decoded.outcome.text == "pong"
    with pytest.raises(ValueError):
        ng.batch_decode_line(descriptor, request, "not json")
    with pytest.raises(ValueError):
        ng.batch_jsonl(descriptor, [item, item])


def test_an_interactions_request_is_not_a_batch_item_or_a_token_count():
    request = _chat_request(_stock_provider("google.interactions"))
    for build in (ng.BatchItem, ng.TokenCount):
        value = build()
        with pytest.raises(ValueError, match="Interactions"):
            value.request = request
    with pytest.raises(ValueError, match="Interactions"):
        ng.TokenCount.from_dict({"request": request})
    with pytest.raises(TypeError, match="expected a ProviderRequest"):
        ng.TokenCount.from_dict({"request": {"model": "m"}})


def test_a_token_count_takes_the_request_of_a_provider(peer):
    peer.body = {"object": "response.input_tokens", "input_tokens": 7}
    provider = _provider(peer, "openai.responses")
    count = ng.TokenCount.from_dict({"request": _chat_request(provider)})
    prepared = provider.prepare_endpoint(count)
    assert prepared.error is None and prepared.endpoint == "tokens"
    result = provider.complete_endpoint(prepared)
    assert isinstance(result, ng.TokenCountResult), result.error.safe_message
    assert result.input == 7 and result.cached is None and result.modalities == []
    assert peer.requests[0]["path"] == "/v1/responses/input_tokens"
    assert peer.requests[0]["body"]["model"] == "fixture-model"


def test_a_batch_result_is_the_request_of_its_results(peer):
    peer.body = {"id": "batch_fixture", "object": "batch", "endpoint": "/v1/chat/completions",
                 "status": "completed", "input_file_id": "file-in", "output_file_id": "file-out",
                 "created_at": 1, "request_counts": {"total": 2, "completed": 1, "failed": 1}}
    batch = _provider(peer).complete_endpoint(ng.BatchGet.from_dict({"id": "batch_fixture"}))
    assert isinstance(batch, ng.Batch), batch.error.safe_message
    assert (batch.id, batch.status, batch.output_file) == ("batch_fixture", ng.JobStatus.Completed, "file-out")
    counts = batch.counts
    assert (counts.total, counts.succeeded, counts.failed) == (2, 1, 1)
    results = ng.BatchResults.from_dict({"batch": batch, "errors": True})
    assert results.batch.id == "batch_fixture" and results.errors and results.path == ""
    with pytest.raises(TypeError, match="expected a Batch"):
        ng.BatchResults.from_dict({"batch": {"id": "batch_fixture"}})
    assert (peer.requests[0]["method"], peer.requests[0]["path"]) == ("GET", "/v1/batches/batch_fixture")


def _signed(secret_bytes, body, now, webhook_id="evt-delivery"):
    content = f"{webhook_id}.{now}.{body}".encode()
    signature = base64.b64encode(hmac.new(secret_bytes, content, hashlib.sha256).digest()).decode()
    return [("webhook-id", webhook_id), ("webhook-timestamp", str(now)), ("webhook-signature", "v1," + signature)]


def test_a_signed_webhook_is_verified_and_a_changed_one_is_refused():
    key = b"fixture-webhook-key"
    secret = "whsec_" + base64.b64encode(key).decode()
    body = json.dumps({"id": "evt_1", "type": "batch.completed", "created_at": 1700000000,
                       "data": {"id": "batch_abc"}})
    headers = _signed(key, body, 1700000100)
    event = ng.verify_webhook(secret, headers, body.encode(), 1700000100)
    assert isinstance(event, ng.WebhookEvent)
    assert (event.id, event.event_id, event.type, event.created_at, event.subject) == (
        "evt-delivery", "evt_1", "batch.completed", 1700000000, "batch_abc")
    assert event.raw["data"]["id"] == "batch_abc"
    assert ng.verify_webhook(secret, dict(headers), body, 1700000100).subject == "batch_abc"
    with pytest.raises(ValueError, match="signature does not match"):
        ng.verify_webhook(secret, headers, body.replace("batch_abc", "batch_xyz"), 1700000100)
    with pytest.raises(ValueError, match="outside the tolerance"):
        ng.verify_webhook(secret, headers, body, 1700000100 + 301)
    assert ng.verify_webhook(secret, headers, body, 1700000100 + 301, tolerance=400).type == "batch.completed"


def _properties(cls):
    return {name for name in dir(cls) if not name.startswith("_") and isinstance(getattr(cls, name), property)}


@pytest.mark.parametrize("name", sorted(FIELDS))
def test_from_dict_table_matches_the_binding(name):
    assert set(FIELDS[name]) == _properties(getattr(ng, name))


def _samples():
    media = ng.ProviderMedia.image("image/png", PNG)
    request = _chat_request(_stock_provider("openai.chat"))
    handle = {"endpoint": "videos", "origin": "https://openrouter.ai", "id": "job-1", "model": "fixture-video"}
    session = {"origin": "https://api.openai.com", "shape": "uploads", "id": "upload-1", "size": 10}
    return {
        "ImagesRequest": {"model": "m", "prompt": "p", "references": [media], "count": 2, "size": "1024x1024",
                          "quality": "high", "background": "transparent", "output_format": "png",
                          "output_compression": 80, "extensions": {"moderation": "low"}},
        "DecisionOption": {"value": "yes", "guidance": {"hint": 1}},
        "DecisionQuestion": {"name": "q", "kind": "Choice", "instructions": "pick", "options": [
            {"value": "a"}, {"value": "b"}], "when_true": "t", "when_false": {"f": 1}},
        "DecisionsRequest": {"model": "m", "input": "text", "questions": [{"name": "q", "kind": "Score"}],
                             "extensions": {"safety_identifier": "s"}},
        "EmbeddingInput": {"text": "t", "media": [media]},
        "EmbeddingsRequest": {"model": "m", "inputs": [{"text": "t"}], "dimensions": 8, "task": "query",
                              "extensions": {"user": "u"}},
        "RerankRequest": {"model": "m", "query": "q", "documents": ["a", "b"], "top": 1, "extensions": {"x": 1}},
        "ModerationInput": {"text": "t", "media": [media]},
        "ModerationRequest": {"model": "m", "inputs": [{"text": "t"}], "extensions": {"x": 1}},
        "SpeechRequest": {"model": "m", "input": "hi", "voice": "v", "instructions": "calm", "format": "wav",
                          "speed": 1, "extensions": {"x": 1}},
        "TranscriptionRequest": {"model": "m", "audio": ng.ProviderMedia.audio("audio/wav", "UklGRg=="),
                                 "language": "en", "prompt": "p", "translate": True, "extensions": {"x": 1}},
        "VideoRequest": {"model": "m", "prompt": "p", "references": [media], "duration": 4,
                         "aspect_ratio": "16:9", "resolution": "480p", "size": "1280x720", "audio": True,
                         "extensions": {"x": 1}},
        "JobHandle": handle,
        "JobPoll": {"handle": handle},
        "JobDownload": {"handle": handle, "index": 1, "path": "/tmp/out.mp4", "max_bytes": 99},
        "FileSource": {"bytes": b"abc", "path": "/tmp/f", "offset": 1, "size": 2, "sha256": "00"},
        "FileUpload": {"source": {"bytes": b"abc"}, "filename": "f", "mime": "text/plain", "purpose": "user_data",
                       "expires_after": 3600},
        "FileGet": {"id": "file-1"},
        "FileDelete": {"id": "file-1"},
        "FileList": {"limit": 10, "after": "file-0", "purpose": "batch"},
        "FileDownload": {"id": "file-1", "path": "/tmp/out", "max_bytes": 99},
        "FileUploadSession": {**session, "url": "u", "sent": 4, "granularity": 8, "parts": ["p"], "expires_at": 5,
                              "status": "pending"},
        "FileUploadStart": {"filename": "f", "mime": "text/plain", "size": 10, "purpose": "user_data",
                            "expires_after": 60},
        "FileUploadPart": {"session": session, "source": {"bytes": b"abc"}},
        "FileUploadComplete": {"session": session},
        "FileUploadQuery": {"session": session},
        "FileUploadCancel": {"session": session},
        "BatchItem": {"custom_id": "one", "request": request},
        "BatchSubmit": {"items": [{"custom_id": "one", "request": request}], "input_file": "file-1",
                        "endpoint": "/v1/chat/completions", "display_name": "d", "extensions": {"metadata": {}}},
        "BatchGet": {"id": "batch_1"},
        "BatchCancel": {"id": "batch_1"},
        "BatchDelete": {"id": "batch_1"},
        "TokenCount": {"request": request},
        "GenerationGet": {"id": "gen-1"},
    }


SAMPLES = _samples()


def test_every_request_class_has_a_sample():
    assert set(SAMPLES) | {"BatchResults"} == set(FIELDS)


@pytest.mark.parametrize("name", sorted(SAMPLES))
def test_from_dict_builds_each_request_class_and_refuses_unknown_keys(name):
    cls = getattr(ng, name)
    built = cls.from_dict(SAMPLES[name])
    assert isinstance(built, cls)
    for key, expected in SAMPLES[name].items():
        if key == "request":
            continue  # a ProviderRequest cannot be read back
        value = getattr(built, key)
        if isinstance(expected, (str, int, bool, bytes)) and not isinstance(value, ng.DecisionKind) \
                or key == "extensions":
            assert value == expected, key
    with pytest.raises(ValueError, match=r"unknown key 'typo' for " + name):
        cls.from_dict({**SAMPLES[name], "typo": 1})
    assert isinstance(cls.from_dict({}), cls)


def test_from_dict_builds_nested_values_by_type_and_names_enums():
    request = ng.DecisionsRequest.from_dict(SAMPLES["DecisionsRequest"])
    question = request.questions[0]
    assert question.kind == ng.DecisionKind.Score and request.input == "text"
    nested = ng.DecisionQuestion.from_dict(SAMPLES["DecisionQuestion"])
    assert nested.kind == ng.DecisionKind.Choice and [o.value for o in nested.options] == ["a", "b"]
    assert nested.when_false == {"f": 1} and nested.when_true == "t"
    assert ng.DecisionQuestion.from_dict({"kind": ng.DecisionKind.Box}).kind == ng.DecisionKind.Box
    with pytest.raises(ValueError, match="expected one of"):
        ng.DecisionQuestion.from_dict({"kind": "choice"})
    with pytest.raises(TypeError, match="expected an int"):
        ng.EmbeddingsRequest.from_dict({"dimensions": "8"})
    with pytest.raises(TypeError, match="expected a ProviderMedia"):
        ng.EmbeddingInput.from_dict({"media": [{"mime": "image/png"}]})
    with pytest.raises(ValueError, match=r"inputs\[1\]: unknown key 'txt'"):
        ng.EmbeddingsRequest.from_dict({"inputs": [{"text": "a"}, {"txt": "b"}]})
    assert ng.SpeechRequest.from_dict({"speed": 2}).speed == 2.0


def test_the_classes_and_functions_are_exported():
    for name in list(FIELDS) + ["PreparedEndpoint", "ImagesResult", "ProviderFile", "Batch", "BatchLine",
                                "WebhookEvent", "batch_jsonl", "batch_decode_line", "files_media",
                                "verify_webhook"]:
        assert name in ng.__all__ and getattr(ng, name) is not None
