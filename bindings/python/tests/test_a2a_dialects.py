"""Native A2A discovery, dialect encoding, errors, and owned SSE consumers."""

import gc
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

import neograph_engine as ng


a2a = ng.a2a


def _contains_kind(value):
    if isinstance(value, dict):
        return "kind" in value or any(_contains_kind(item) for item in value.values())
    if isinstance(value, list):
        return any(_contains_kind(item) for item in value)
    return False


def _frame(result=None, error=None):
    envelope = {"jsonrpc": "2.0"}
    envelope["error" if error is not None else "result"] = (
        error if error is not None else result
    )
    return "data: " + json.dumps(envelope) + "\r\n\r\n"


class _A2APeer:
    """Local protocol endpoint; requests use the real native HTTP transport."""

    def __init__(self):
        self.requests = []
        self.dialect = "1.0"
        self.result = None
        self.error = None
        self.stream = None
        peer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def reply(self, status, body, content_type="application/json"):
                data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)
                self.close_connection = True

            def do_GET(self):
                peer.requests.append({"method": "GET", "path": self.path})
                if self.path != "/.well-known/agent-card.json":
                    self.reply(404, {"error": "unknown discovery path"})
                    return
                self.reply(200, peer.card)

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                headers = {key.lower(): value for key, value in self.headers.items()}
                peer.requests.append(
                    {"method": "POST", "path": self.path, "headers": headers, "body": body}
                )
                envelope = {"jsonrpc": "2.0", "id": body["id"]}
                if self.path != "/":
                    self.reply(404, {"error": "unknown RPC endpoint"})
                    return
                methods = (
                    {"SendMessage", "GetTask", "CancelTask", "SendStreamingMessage"}
                    if peer.dialect == "1.0"
                    else {"message/send", "tasks/get", "tasks/cancel", "message/stream"}
                )
                if body["method"] not in methods:
                    envelope["error"] = {"code": -32601, "message": "Method not found"}
                elif peer.dialect == "1.0" and headers.get("a2a-version") != "1.0":
                    envelope["error"] = {"code": -32009, "message": "Version not supported"}
                elif peer.dialect == "1.0" and _contains_kind(body["params"]):
                    envelope["error"] = {"code": -32602, "message": "Invalid ProtoJSON"}
                elif peer.error is not None:
                    envelope["error"] = peer.error
                elif body["method"] in {"SendStreamingMessage", "message/stream"}:
                    lines = []
                    for line in peer.stream.splitlines(keepends=True):
                        if line.startswith("data: "):
                            frame = json.loads(line[6:])
                            frame["id"] = body["id"]
                            ending = line[len(line.rstrip("\r\n")):]
                            line = "data: " + json.dumps(frame) + ending
                        lines.append(line)
                    self.reply(200, "".join(lines), "text/event-stream")
                    return
                else:
                    result = peer.result
                    if body["method"] in {"GetTask", "CancelTask"}:
                        result = result.get("task", result)
                    envelope["result"] = result
                self.reply(200, envelope)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"
        self.configure("1.0")
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def rpc_requests(self):
        return [request for request in self.requests if request["method"] == "POST"]

    def configure(self, dialect):
        self.dialect = dialect
        self.card = {
            "name": "local-dialect-agent",
            "description": "Deterministic localhost protocol peer",
            "version": "1.0.0",
            "capabilities": {"streaming": True},
            "defaultInputModes": ["text/plain", "application/json"],
            "defaultOutputModes": ["text/plain"],
            "skills": [],
        }
        if dialect == "1.0":
            self.card["supportedInterfaces"] = [
                {
                    "url": self.base_url + "/",
                    "protocolBinding": "JSONRPC",
                    "protocolVersion": "1.0",
                }
            ]
            message = {
                "messageId": "remote-answer",
                "role": "ROLE_AGENT",
                "parts": [
                    {"text": "remote answer", "mediaType": "text/markdown"},
                    {"data": {"accepted": True}, "mediaType": "application/json"},
                    {"raw": "b3V0", "filename": "answer.txt", "mediaType": "text/plain"},
                ],
            }
            task = {
                "id": "remote-task",
                "contextId": "remote-context",
                "status": {"state": "TASK_STATE_COMPLETED", "message": message},
                "history": [message],
            }
            self.result = {"task": task}
        else:
            self.card.update(
                url=self.base_url + "/",
                protocolVersion="0.3.0",
                preferredTransport="JSONRPC",
                additionalInterfaces=[{"url": self.base_url + "/grpc", "transport": "GRPC"}],
            )
            message = {
                "kind": "message",
                "messageId": "remote-answer",
                "role": "agent",
                "parts": [{"kind": "text", "text": "remote answer"}],
            }
            self.result = {
                "kind": "task",
                "id": "remote-task",
                "contextId": "remote-context",
                "status": {"state": "completed", "message": message},
                "history": [message],
            }
        self.stream = _frame(self.result)

    def close(self):
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=3)
            self.server = None


@pytest.fixture
def a2a_peer():
    peer = _A2APeer()
    try:
        yield peer
    finally:
        peer.close()


def _client(peer):
    client = a2a.A2AClient(peer.base_url)
    client.set_timeout(3)
    return client


def _multipart_params():
    text = a2a.Part.text_part("inspect attachment")
    text.media_type = "text/plain"
    data = a2a.Part()
    data.kind = "data"
    data.data = {"question": "count", "items": [2, 3]}
    data.media_type = "application/json"
    data.metadata = {"source": "consumer"}
    attachment = a2a.Part()
    attachment.kind = "file"
    attachment.file = {"bytes": "aW4=", "name": "input.txt", "mimeType": "text/plain"}
    message = a2a.Message()
    message.message_id = "multipart-consumer"
    message.role = a2a.Role.User
    message.task_id = "prior-task"
    message.context_id = "prior-context"
    message.parts = [text, data, attachment]
    configuration = a2a.MessageSendConfiguration()
    configuration.blocking = True
    configuration.history_length = 7
    configuration.accepted_output_modes = ["text/plain"]
    params = a2a.MessageSendParams()
    params.message = message
    params.configuration = configuration
    return params


@pytest.mark.parametrize("dialect", ["0.3", "1.0"])
def test_discovered_dialect_encodes_multipart_and_decodes_native_task(a2a_peer, dialect):
    a2a_peer.configure(dialect)
    client = _client(a2a_peer)
    card = client.fetch_agent_card()
    task = client.send_message(_multipart_params())

    assert task.id == "remote-task"
    assert task.context_id == "remote-context"
    assert task.state == a2a.TaskState.Completed
    answer = task.history[0]
    assert answer.role == a2a.Role.Agent
    assert answer.parts[0].text == "remote answer"
    request, = a2a_peer.rpc_requests
    body = request["body"]
    params = body["params"]
    message = params["message"]
    assert message["messageId"] == "multipart-consumer"
    assert (message["taskId"], message["contextId"]) == ("prior-task", "prior-context")
    assert params["configuration"]["historyLength"] == 7
    assert params["configuration"]["acceptedOutputModes"] == ["text/plain"]
    if dialect == "1.0":
        assert client.wire_dialect() == a2a.WireDialect.V1_0
        assert body["method"] == "SendMessage"
        assert request["headers"]["a2a-version"] == "1.0"
        assert not _contains_kind(params)
        assert message["role"] == "ROLE_USER"
        assert params["configuration"] == {
            "returnImmediately": False, "historyLength": 7,
            "acceptedOutputModes": ["text/plain"],
        }
        assert message["parts"] == [
            {"text": "inspect attachment", "mediaType": "text/plain"},
            {"data": {"question": "count", "items": [2, 3]},
             "mediaType": "application/json", "metadata": {"source": "consumer"}},
            {"raw": "aW4=", "filename": "input.txt", "mediaType": "text/plain"},
        ]
        assert answer.parts[0].media_type == "text/markdown"
        assert answer.parts[1].data == {"accepted": True}
        assert answer.parts[1].media_type == "application/json"
        assert answer.parts[2].file == {
            "bytes": "b3V0", "name": "answer.txt", "mimeType": "text/plain",
        }
    else:
        assert client.wire_dialect() == a2a.WireDialect.V0_3
        assert body["method"] == "message/send"
        assert "a2a-version" not in request["headers"]
        assert message["kind"] == "message"
        assert message["role"] == "user"
        assert params["configuration"]["blocking"] is True
        assert "returnImmediately" not in params["configuration"]
        assert message["parts"] == [
            {"kind": "text", "text": "inspect attachment"},
            {"kind": "data", "data": {"question": "count", "items": [2, 3]},
             "metadata": {"source": "consumer"}},
            {"kind": "file", "file": {
                "bytes": "aW4=", "name": "input.txt", "mimeType": "text/plain",
            }},
        ]
        assert [(item.protocol_binding, item.protocol_version) for item in card.supported_interfaces] == [
            ("JSONRPC", "0.3.0"), ("GRPC", "0.3.0"),
        ]


def test_matching_interface_and_tenant_survive_detached_card_edits(a2a_peer):
    a2a_peer.card["supportedInterfaces"] = [
        {"url": a2a_peer.base_url + "/legacy", "protocolBinding": "JSONRPC",
         "protocolVersion": "0.3"},
        {"url": a2a_peer.base_url + "/", "protocolBinding": "jsonrpc",
         "protocolVersion": "1.0", "tenant": "local-tenant"},
    ]
    client = _client(a2a_peer)
    client.set_authorization_header("Bearer localhost-test-value")
    card = client.fetch_agent_card()
    interfaces = card.supported_interfaces
    assert [(item.protocol_version, item.tenant) for item in interfaces] == [
        ("0.3", ""), ("1.0", "local-tenant"),
    ]
    interfaces[1].protocol_version = "2.0"
    interfaces[1].tenant = "edited-copy"
    raw = card.raw
    raw["supportedInterfaces"][1]["tenant"] = "edited-raw"
    assert card.supported_interfaces[1].tenant == "local-tenant"
    assert card.raw["supportedInterfaces"][1]["tenant"] == "local-tenant"

    sent = client.send_message("select matching interface")
    got = client.get_task(sent.id, 2)
    canceled = client.cancel_task(sent.id)
    streamed = client.send_message_stream("tenant stream", lambda _event: True)
    assert [task.state for task in (sent, got, canceled, streamed)] == [a2a.TaskState.Completed] * 4
    requests = a2a_peer.rpc_requests
    assert [request["body"]["method"] for request in requests] == [
        "SendMessage", "GetTask", "CancelTask", "SendStreamingMessage",
    ]
    assert requests[1]["body"]["params"] == {
        "id": "remote-task", "historyLength": 2, "tenant": "local-tenant",
    }
    assert requests[2]["body"]["params"] == {"id": "remote-task", "tenant": "local-tenant"}
    for request in requests:
        assert request["body"]["params"]["tenant"] == "local-tenant"
        assert request["headers"]["a2a-version"] == "1.0"
        assert request["headers"]["authorization"] == "Bearer localhost-test-value"
    assert client.wire_dialect() == a2a.WireDialect.V1_0


def test_forced_discovery_refresh_replaces_selected_wire_dialect(a2a_peer):
    client = _client(a2a_peer)
    original = client.fetch_agent_card()
    assert client.send_message("first dialect").state == a2a.TaskState.Completed
    assert client.wire_dialect() == a2a.WireDialect.V1_0
    a2a_peer.configure("0.3")
    assert client.fetch_agent_card().raw == original.raw
    refreshed = client.fetch_agent_card(force=True)
    assert refreshed.protocol_version == "0.3.0"
    assert client.wire_dialect() is None
    assert client.send_message("refreshed dialect").state == a2a.TaskState.Completed
    assert client.wire_dialect() == a2a.WireDialect.V0_3
    assert [request["body"]["method"] for request in a2a_peer.rpc_requests] == [
        "SendMessage", "message/send",
    ]
    assert [request["method"] for request in a2a_peer.requests] == ["GET", "POST", "GET", "POST"]


def test_incompatible_discovered_interfaces_fail_before_rpc(a2a_peer):
    a2a_peer.card["supportedInterfaces"] = [
        {"url": a2a_peer.base_url + "/grpc", "protocolBinding": "GRPC", "protocolVersion": "1.0"},
        {"url": a2a_peer.base_url + "/", "protocolBinding": "JSONRPC", "protocolVersion": "2.0"},
    ]
    client = _client(a2a_peer)
    client.fetch_agent_card()
    with pytest.raises(RuntimeError):
        client.send_message("cannot admit this interface")
    assert a2a_peer.rpc_requests == []
    assert client.wire_dialect() is None


@pytest.mark.parametrize("streaming", [False, True])
@pytest.mark.parametrize("remote_code", [-32601, -32602, -32009])
def test_remote_rpc_code_is_preserved_without_card_selected_redispatch(a2a_peer, streaming, remote_code):
    a2a_peer.error = {"code": remote_code, "message": "Method not found (-32601)"}
    client = _client(a2a_peer)
    client.fetch_agent_card()
    with pytest.raises(a2a.A2ARpcError) as raised:
        if streaming:
            client.send_message_stream("rejected stream", lambda _event: True)
        else:
            client.send_message("rejected send")
    assert raised.value.code == remote_code
    assert isinstance(raised.value, RuntimeError)
    request, = a2a_peer.rpc_requests
    assert request["body"]["method"] == ("SendStreamingMessage" if streaming else "SendMessage")
    assert client.wire_dialect() == a2a.WireDialect.V1_0


def test_sse_snapshots_outlive_client_and_preserve_artifact_chunks(a2a_peer):
    a2a_peer.stream = (
        _frame({"task": {"id": "stream-task", "contextId": "stream-context",
                         "status": {"state": "TASK_STATE_SUBMITTED"}}})
        + ": ping\r\n\r\n"
        + _frame({"statusUpdate": {"taskId": "stream-task", "contextId": "stream-context",
                                  "status": {"state": "TASK_STATE_WORKING"}}})
        + _frame({"artifactUpdate": {"taskId": "stream-task", "contextId": "stream-context",
                    "artifact": {"artifactId": "stream-artifact", "parts": [{"text": "Hel"}]},
                    "append": False, "lastChunk": False}})
        + _frame({"artifactUpdate": {"taskId": "stream-task", "contextId": "stream-context",
                    "artifact": {"artifactId": "stream-artifact", "parts": [{"text": "lo"}]},
                    "append": True, "lastChunk": True}})
        + _frame({"statusUpdate": {"taskId": "stream-task", "contextId": "stream-context",
                                  "status": {"state": "TASK_STATE_COMPLETED"}}})
    )
    events = []

    def retain(event):
        events.append(event)
        return True

    client = _client(a2a_peer)
    client.fetch_agent_card()
    task = client.send_message_stream(_multipart_params(), retain)
    assert client.wire_dialect() == a2a.WireDialect.V1_0
    request, = a2a_peer.rpc_requests
    assert request["body"]["method"] == "SendStreamingMessage"
    assert request["headers"]["a2a-version"] == "1.0"
    assert not _contains_kind(request["body"]["params"])
    del client
    a2a_peer.close()
    gc.collect()

    opening, working, first_chunk, second_chunk, completed = events
    assert [event.type for event in events] == [
        a2a.StreamEvent.Type.Task, a2a.StreamEvent.Type.StatusUpdate,
        a2a.StreamEvent.Type.ArtifactUpdate, a2a.StreamEvent.Type.ArtifactUpdate,
        a2a.StreamEvent.Type.StatusUpdate,
    ]
    assert opening.task.id == "stream-task"
    assert opening.task.state == a2a.TaskState.Submitted
    assert working.status_update.status.state == a2a.TaskState.Working
    assert [event.is_final() for event in events] == [False, False, False, False, True]
    assert first_chunk.artifact_update.artifact.parts[0].text == "Hel"
    assert second_chunk.artifact_update.artifact.parts[0].text == "lo"
    assert first_chunk.artifact_update.append is False
    assert second_chunk.artifact_update.append is True
    assert second_chunk.artifact_update.last_chunk is True
    assert completed.status_update.status.state == a2a.TaskState.Completed
    assert task.id == "stream-task"
    assert task.context_id == "stream-context"
    assert task.state == a2a.TaskState.Completed
    assert [part.text for part in task.artifacts[0].parts] == ["Hel", "lo"]
    detached = first_chunk.artifact_update.artifact
    detached.parts = second_chunk.artifact_update.artifact.parts
    assert first_chunk.artifact_update.artifact.parts[0].text == "Hel"
    assert [part.text for part in task.artifacts[0].parts] == ["Hel", "lo"]


def test_callback_abort_returns_observed_partial_task_without_later_events(a2a_peer):
    a2a_peer.stream = (
        _frame({"statusUpdate": {"taskId": "aborted-task", "contextId": "aborted-context",
                                  "status": {"state": "TASK_STATE_WORKING"}}})
        + _frame({"task": {"id": "aborted-task", "status": {"state": "TASK_STATE_COMPLETED"}}})
    )
    events = []

    def stop(event):
        events.append(event)
        return False

    client = _client(a2a_peer)
    client.fetch_agent_card()
    task = client.send_message_stream("stop after observation", stop)
    event, = events
    assert event.status_update.task_id == "aborted-task"
    assert event.status_update.status.state == a2a.TaskState.Working
    assert task.id == "aborted-task"
    assert task.context_id == "aborted-context"
    assert task.state == a2a.TaskState.Working
    request, = a2a_peer.rpc_requests
    assert request["body"]["method"] == "SendStreamingMessage"


def test_observed_stream_error_preserves_code_and_never_redispatches(a2a_peer):
    a2a_peer.configure("0.3")
    a2a_peer.stream = (
        _frame({"kind": "status-update", "taskId": "observed-task",
                "status": {"state": "working"}, "final": False})
        + _frame(error={"code": -32601, "message": "Method not found"})
    )
    events = []

    def retain(event):
        events.append(event)
        return True

    client = _client(a2a_peer)
    with pytest.raises(a2a.A2ARpcError) as raised:
        client.send_message_stream("observe then fail", retain)
    assert raised.value.code == -32601
    event, = events
    assert event.status_update.task_id == "observed-task"
    assert event.status_update.status.state == a2a.TaskState.Working
    request, = a2a_peer.rpc_requests
    assert request["body"]["method"] == "message/stream"
    assert client.wire_dialect() is None


def test_unbound_stream_probes_v1_only_before_any_event_is_observed(a2a_peer):
    events = []

    def retain(event):
        events.append(event)
        return True

    client = _client(a2a_peer)
    task = client.send_message_stream("unobserved method rejection", retain)
    assert task.id == "remote-task"
    assert task.state == a2a.TaskState.Completed
    event, = events
    assert event.type == a2a.StreamEvent.Type.Task
    assert event.task.history[0].parts[0].text == "remote answer"
    legacy, modern = a2a_peer.rpc_requests
    assert legacy["body"]["method"] == "message/stream"
    assert "a2a-version" not in legacy["headers"]
    assert modern["body"]["method"] == "SendStreamingMessage"
    assert modern["headers"]["a2a-version"] == "1.0"
    assert not _contains_kind(modern["body"]["params"])
    assert client.wire_dialect() == a2a.WireDialect.V1_0
