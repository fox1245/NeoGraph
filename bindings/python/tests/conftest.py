"""Local TLS peers for exercising the real typed provider boundary."""

import json
import shutil
import ssl
import socket
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

import neograph_engine as ng
import neograph_engine.llm as nglm


def _logical_chat_messages(messages):
    """Read Chat's public string-or-content-block input without SDK projections."""
    logical = []
    for message in messages:
        role = message["role"]
        if not isinstance(role, str):
            raise ValueError("Chat role must be text")
        content = message.get("content")
        if isinstance(content, str):
            parts = [{"type": "text", "text": content}]
        elif content is None:
            parts = []
        elif isinstance(content, list):
            parts = content
        else:
            raise ValueError("Chat content must be text, blocks, or null")
        fragments = []
        for part in parts:
            if not isinstance(part, dict) or not isinstance(part.get("type"), str):
                raise ValueError("Chat content block requires a type")
            if part["type"] == "text":
                if not isinstance(part.get("text"), str):
                    raise ValueError("Chat text block requires text")
                fragments.append(part["text"])
        logical.append({"role": role, "text": "".join(fragments), "parts": parts})
    return logical


def _tls_files(directory):
    openssl = shutil.which("openssl")
    if openssl is None:
        pytest.fail("provider TLS fixtures require the openssl executable")
    cert = directory / "peer.crt"
    key = directory / "peer.key"
    subprocess.run(
        [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", str(key), "-out", str(cert), "-days", "1",
         "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1"],
        check=True, capture_output=True,
    )
    return cert, key


class _TLSHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128

    def __init__(self, address, handler, context):
        self._tls_context = context
        super().__init__(address, handler)

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(5)
        try:
            secured = self._tls_context.wrap_socket(connection, server_side=True)
        except BaseException:
            connection.close()
            raise
        secured.settimeout(15)
        return secured, address


@pytest.fixture
def provider_peer(tmp_path):
    cert, key = _tls_files(tmp_path)

    class Peer:
        def __init__(self):
            self.requests = []
            self.logical_requests = []
            self.clients = []
            self.changed = threading.Condition()
            self.headers_seen = threading.Event()
            self.allow_body_read = threading.Event()
            self.allow_body_read.set()
            self.reply = "ok"
            self.usage = {"prompt_tokens": 10, "completion_tokens": 5,
                          "total_tokens": 15}
            self.status = 200
            self.error = {"error": {"message": "local fixture refusal",
                                    "type": "invalid_request_error"}}
            self.started = threading.Event()
            self.release = threading.Event()
            self.release.set()
            self.stream_chunks = ["ok"]
            self.response_extra = {}

        def provider(self, *, max_operations=256, max_host_connections=0,
                     timeout_ms=5000, resource_bytes=None):
            descriptor = {
                "descriptor_version": 1, "revision": 1,
                "id": "python-local-chat", "family": "openai.chat",
                "connection": {
                    "base_url": f"https://127.0.0.1:{server.server_port}",
                    "paths": {"buffered": "/v1/chat/completions",
                              "streaming": "/v1/chat/completions"},
                },
                "bindings": {"model": "model", "messages": "messages",
                             "stream": "stream", "max_output_tokens": "max_tokens",
                             "usage": ["usage"]},
                "stop_reasons": {"stop": "EndTurn", "length": "MaxTokens",
                                 "tool_calls": "ToolUse"},
            }
            policy = None
            if resource_bytes is not None:
                resources = json.loads(ng.provider_codec_defaults_json())
                resources["resources"]["chat_text_request_bytes"] = resource_bytes
                resources["resources"]["json_bytes"] = resource_bytes
                policy = ng.load_provider_policy(
                    ng.provider_policy_json(), json.dumps(resources))
            options = ng.ProviderRuntimeOptions(
                ca_file=str(cert), default_timeout_ms=timeout_ms)
            options.http_version = ng.ProviderHttpVersion.Http1_1
            limits = ng.ProviderRuntimeLimits()
            limits.max_operations = max_operations
            options.limits = limits
            transport = ng.ProviderTransportOptions()
            transport.max_host_connections = max_host_connections
            options.transport = transport
            return nglm.SchemaProvider(
                ng.load_provider_descriptor(json.dumps(descriptor), policy), options)

    peer = Peer()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass

        def do_POST(self):
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            peer.headers_seen.set()
            if not peer.allow_body_read.wait(15):
                self.send_error(504)
                return
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            try:
                logical = _logical_chat_messages(body["messages"])
            except (KeyError, TypeError, ValueError):
                self.send_error(400)
                return
            with peer.changed:
                peer.requests.append(body)
                peer.logical_requests.append(logical)
                peer.clients.append(self.client_address)
                peer.changed.notify_all()
            peer.started.set()
            if not peer.release.wait(15):
                self.send_error(504)
                return
            if peer.status != 200:
                payload = json.dumps(peer.error).encode()
                content_type = "application/json"
            elif body.get("stream"):
                frames = []
                for chunk in peer.stream_chunks:
                    frames.append({"id": "local-completion", "object": "chat.completion.chunk",
                                   "model": body["model"], "created": 1,
                                   "choices": [{"index": 0, "delta": {"content": chunk},
                                                "finish_reason": None}]})
                frames.append({"id": "local-completion", "object": "chat.completion.chunk",
                               "model": body["model"], "created": 1,
                               "choices": [{"index": 0, "delta": {},
                                            "finish_reason": "stop"}]})
                if peer.usage is not None:
                    frames.append({"id": "local-completion", "object": "chat.completion.chunk",
                                   "model": body["model"], "created": 1, "choices": [],
                                   "usage": peer.usage})
                payload = ("".join("data: " + json.dumps(frame) + "\n\n"
                                   for frame in frames) + "data: [DONE]\n\n").encode()
                content_type = "text/event-stream"
            else:
                document = {"id": "local-completion", "object": "chat.completion",
                            "model": body["model"], "created": 1,
                            "choices": [{"index": 0,
                                         "message": {"role": "assistant", "content": peer.reply},
                                         "finish_reason": "stop"}], **peer.response_extra}
                if peer.usage is not None:
                    document["usage"] = peer.usage
                payload = json.dumps(document).encode()
                content_type = "application/json"
            self.send_response(peer.status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass  # A cancelled client closes the socket before the peer replies.

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server = _TLSHTTPServer(("127.0.0.1", 0), Handler, context)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield peer
    finally:
        peer.release.set()
        peer.allow_body_read.set()
        server.shutdown()
        server.server_close()
        thread.join()


@pytest.fixture
def typed_peer(tmp_path):
    """A real TLS peer whose tests define each declared family's wire protocol."""
    directory = tmp_path / "typed-peer"
    directory.mkdir(mode=0o700)
    cert, key = _tls_files(directory)

    class Peer:
        def __init__(self):
            self.requests = []
            self.responder = None
            self.changed = threading.Condition()
            self.ca_file = str(cert)

        @property
        def base_url(self):
            return f"https://127.0.0.1:{server.server_port}"

        def provider(self, family, *, paths=None, policy=None, headers=None,
                     defaults=None, timeout_ms=5000):
            descriptor = {
                "descriptor_version": 1, "revision": 1,
                "id": "python-local-" + family.replace(".", "-"),
                "family": family,
                "connection": {
                    "base_url": self.base_url,
                    "paths": paths or {"buffered": "/operation", "streaming": "/operation"},
                },
            }
            if headers:
                descriptor["connection"]["headers"] = headers
            options = ng.ProviderRuntimeOptions(
                ca_file=self.ca_file, default_timeout_ms=timeout_ms)
            options.http_version = ng.ProviderHttpVersion.Http1_1
            return nglm.SchemaProvider(
                ng.load_provider_descriptor(json.dumps(descriptor), policy), options, defaults)

    peer = Peer()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass

        def do_POST(self):
            request = {
                "method": "POST", "path": self.path,
                "headers": dict(self.headers),
                "raw_headers": list(self.headers.raw_items()),
                "body": json.loads(self.rfile.read(int(self.headers["Content-Length"]))),
            }
            with peer.changed:
                peer.requests.append(request)
                peer.changed.notify_all()
            if peer.responder is None:
                self.send_error(501)
                return
            status, body, content_type = peer.responder(request)
            payload = body if isinstance(body, bytes) else json.dumps(body).encode()
            try:
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass  # The request's explicit cancellation may close the peer socket.

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server = _TLSHTTPServer(("127.0.0.1", 0), Handler, context)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield peer
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
