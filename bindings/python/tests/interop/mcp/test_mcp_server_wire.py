"""Raw-wire conformance checks of the NeoGraph MCP server (no SDK).

These cover rules an SDK client never exercises (malformed input, batches,
transport headers, Origin, sessions, auth). The oracle is the MCP 2025-11-25
text and schema.json; every exchange is written to a transcript and later
validated by validate_transcripts.py.
"""

import json
import queue
import subprocess
import threading
import time

import pytest

from mcp_sdk_requirement import require_official_python_sdk

httpx = pytest.importorskip("httpx")
require_official_python_sdk()  # the suite's pinned environment

from mcp_server_interop import (PROBE, http_server, missing_binaries, probe_launch,  # noqa: E402
                                transcript_path)

if missing_binaries():
    pytest.skip(f"NEOGRAPH_BUILD_DIR lacks {missing_binaries()}", allow_module_level=True)

INIT = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                   "clientInfo": {"name": "raw", "version": "1"}}}
INITIALIZED = {"jsonrpc": "2.0", "method": "notifications/initialized"}
ACCEPT = "application/json, text/event-stream"


class Stdio:
    def __init__(self, log, *extra):
        self.log = log
        self.proc = subprocess.Popen([str(PROBE), "stdio", *extra], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.lines: "queue.Queue[str]" = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.proc.stdout:
            line = raw.decode().rstrip("\n")
            self._rec("s2c", line)
            self.lines.put(line)

    def _rec(self, d, raw):
        with open(self.log, "a") as f:
            f.write(json.dumps({"t": time.time(), "dir": d, "raw": raw}) + "\n")

    def send(self, payload):
        raw = payload if isinstance(payload, str) else json.dumps(payload)
        self._rec("c2s", raw)
        self.proc.stdin.write(raw.encode() + b"\n")
        self.proc.stdin.flush()

    def recv(self, timeout=3.0):
        try:
            return json.loads(self.lines.get(timeout=timeout))
        except queue.Empty:
            return None

    def init(self):
        self.send(INIT)
        r = self.recv()
        self.send(INITIALIZED)
        return r

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(5)
        return self.proc.stderr.read().decode()


@pytest.fixture
def stdio(request):
    s = Stdio(transcript_path(f"raw-{request.node.name}"))
    yield s
    s.close()


# --- lifecycle / JSON-RPC over stdio -------------------------------------------

@pytest.mark.parametrize("revision", ["2025-06-18", "2025-03-26", "2024-11-05"])
def test_initialize_older_revision_is_answered_with_that_revision(stdio, revision):
    """lifecycle: the server answers the requested version when it supports it (it also
    answers 2025-06-18, 2025-03-26 and 2024-11-05), with the fields that revision has."""
    stdio.send({**INIT, "params": {**INIT["params"], "protocolVersion": revision}})
    r = stdio.recv()
    assert r["result"]["protocolVersion"] == revision
    stdio.send(INITIALIZED)
    stdio.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    tools = {t["name"]: t for t in stdio.recv()["result"]["tools"]}
    assert "icons" not in tools["decorated"] and "execution" not in tools["decorated"]
    if revision in ("2025-03-26", "2024-11-05"):
        assert "outputSchema" not in tools["echo"] and "title" not in tools["decorated"]
        assert ("annotations" in tools["decorated"]) == (revision == "2025-03-26")
        stdio.send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                    "params": {"name": "echo", "arguments": {"message": "old"}}})
        result = stdio.recv()["result"]
        assert "structuredContent" not in result
        assert json.loads(result["content"][0]["text"]) == {"message": "old"}
        # resource_link came with 2025-06-18: it is sent as text holding its JSON.
        stdio.send({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                    "params": {"name": "decorated", "arguments": {}}})
        content = stdio.recv()["result"]["content"]
        assert [b["type"] for b in content] == ["text", "image", "text"]
        assert json.loads(content[2]["text"])["type"] == "resource_link"


@pytest.mark.parametrize("revision", ["2025-03-26", "2024-11-05"])
def test_batch_is_answered_for_revisions_with_batching(stdio, revision):
    """basic (2025-03-26), "Batching": a receiver MUST support JSON-RPC batches. One
    array answers the requests; notifications get nothing. 2024-11-05 peers alike."""
    stdio.send({**INIT, "params": {**INIT["params"], "protocolVersion": revision}})
    assert stdio.recv()["result"]["protocolVersion"] == revision
    stdio.send(INITIALIZED)
    stdio.send([{"jsonrpc": "2.0", "id": 9, "method": "ping"},
                {"jsonrpc": "2.0", "id": 10, "method": "tools/call",
                 "params": {"name": "echo", "arguments": {"message": "b"}}},
                {"jsonrpc": "2.0", "method": "notifications/progress",
                 "params": {"progressToken": 1, "progress": 1}}])
    r = stdio.recv()
    assert isinstance(r, list) and sorted(x["id"] for x in r) == [9, 10], r
    by_id = {x["id"]: x for x in r}
    assert by_id[9]["result"] == {}
    assert json.loads(by_id[10]["result"]["content"][0]["text"]) == {"message": "b"}


def test_initialize_unknown_revision_counter_offers(stdio):
    stdio.send({**INIT, "params": {**INIT["params"], "protocolVersion": "1999-01-01"}})
    assert stdio.recv()["result"]["protocolVersion"] == "2025-11-25"


def test_requests_before_initialized_notification(stdio):
    stdio.send(INIT)
    stdio.recv()
    stdio.send({"jsonrpc": "2.0", "id": 2, "method": "ping"})
    assert stdio.recv()["result"] == {}
    stdio.send({"jsonrpc": "2.0", "id": 3, "method": "tools/list"})
    r = stdio.recv()
    # Strict (official SDK servers also refuse); code -32002 is not a JSON-RPC/MCP code.
    assert "error" in r and r["error"]["code"] == -32002


def test_notifications_get_no_response(stdio):
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "method": "notifications/unknown_thing", "params": {}})
    stdio.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                "params": {"requestId": 999}})
    stdio.send({"jsonrpc": "2.0", "id": 5, "method": "ping"})
    assert stdio.recv()["id"] == 5


def test_method_not_found(stdio):
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "id": "s-1", "method": "nope/nope"})
    r = stdio.recv()
    assert r["id"] == "s-1" and r["error"]["code"] == -32601


def test_invalid_params(stdio):
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"arguments": {}}})
    assert stdio.recv()["error"]["code"] == -32602
    stdio.send({"jsonrpc": "2.0", "id": 8, "method": "tools/list", "params": {"cursor": "x"}})
    assert stdio.recv()["error"]["code"] == -32602


def test_parse_error_response_is_schema_valid(stdio):
    """2025-11-25 schema: JSONRPCErrorResponse.id is optional RequestId (string|integer), not null."""
    stdio.init()
    stdio.send("{not json")
    r = stdio.recv()
    assert r["error"]["code"] == -32700
    assert "id" not in r or r["id"] is not None, "error response carries id:null: " + json.dumps(r)


def test_batch_is_rejected_with_schema_valid_error(stdio):
    """Batching was removed in 2025-06-18; a batch must be refused (and not executed)."""
    stdio.init()
    stdio.send([{"jsonrpc": "2.0", "id": 9, "method": "ping"}])
    r = stdio.recv()
    assert isinstance(r, dict) and r["error"]["code"] == -32600
    assert "id" not in r or r["id"] is not None, "error response carries id:null: " + json.dumps(r)


def test_null_and_fractional_ids_are_invalid_requests(stdio):
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "id": None, "method": "ping"})
    assert stdio.recv()["error"]["code"] == -32600
    stdio.send({"jsonrpc": "2.0", "id": 1.5, "method": "ping"})
    assert stdio.recv()["error"]["code"] == -32600


def test_invalid_request_echoes_detectable_id(stdio):
    """JSON-RPC 2.0 s5: the error's id is the request id when it could be read."""
    stdio.init()
    stdio.send({"jsonrpc": "1.0", "id": 42, "method": "ping"})
    r = stdio.recv()
    assert r["error"]["code"] == -32600
    assert r.get("id") == 42, json.dumps(r)


def test_stdout_carries_only_mcp_messages(stdio):
    stdio.init()
    stdio.send("garbage")
    stdio.recv()
    stdio.send({"jsonrpc": "2.0", "id": 11, "method": "tools/call",
                "params": {"name": "echo", "arguments": {"message": "line1\nline2"}}})
    r = stdio.recv()
    assert r["result"]["structuredContent"]["message"] == "line1\nline2"
    stderr = stdio.close()
    assert "parse error" in stderr  # diagnostics go to stderr, never stdout
    for line in open(stdio.log):
        rec = json.loads(line)
        if rec["dir"] == "s2c":
            json.loads(rec["raw"])  # every stdout line is one JSON message


def test_unknown_tool_is_protocol_error_raw(stdio):
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "id": 12, "method": "tools/call",
                "params": {"name": "missing", "arguments": {}}})
    r = stdio.recv()
    assert "error" in r and r["error"]["code"] == -32602, json.dumps(r)


def test_cancelled_request_gets_no_response_raw(stdio):
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "id": 13, "method": "tools/call",
                "params": {"name": "slow", "arguments": {"seconds": 5}}})
    time.sleep(0.2)
    stdio.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                "params": {"requestId": 13, "reason": "test"}})
    late = stdio.recv(timeout=1.5)
    assert late is None, "response to a cancelled request: " + json.dumps(late)


def test_tool_names_follow_sep_986(stdio):
    """2025-11-25 tools: names SHOULD be 1-128 chars of [A-Za-z0-9_.-]."""
    import re
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "id": 14, "method": "tools/list"})
    for tool in stdio.recv()["result"]["tools"]:
        assert re.fullmatch(r"[A-Za-z0-9_.\-]{1,128}", tool["name"])


# --- Streamable HTTP --------------------------------------------------------------

class Http:
    def __init__(self, port, log):
        self.base = f"http://127.0.0.1:{port}/mcp"
        self.log = log
        self.c = httpx.Client(timeout=10)

    def _rec(self, d, **kw):
        with open(self.log, "a") as f:
            f.write(json.dumps({"t": time.time(), "dir": d, **kw}) + "\n")

    def req(self, method, body=None, headers=None, raw=None):
        h = {"Accept": ACCEPT, "Content-Type": "application/json", **(headers or {})}
        h = {k: v for k, v in h.items() if v is not None}
        content = raw if raw is not None else (json.dumps(body) if body is not None else None)
        self._rec("c2s", method=method, headers=h, raw=content or "")
        r = self.c.request(method, self.base, headers=h, content=content)
        self._rec("s2c", status=r.status_code, headers=dict(r.headers), raw=r.text)
        return r

    def session(self, revision="2025-11-25"):
        r = self.req("POST", {**INIT, "params": {**INIT["params"], "protocolVersion": revision}})
        assert r.json()["result"]["protocolVersion"] == revision
        sid = r.headers["mcp-session-id"]
        h = {"Mcp-Session-Id": sid, "MCP-Protocol-Version": revision}
        assert self.req("POST", INITIALIZED, h).status_code == 202
        return h


@pytest.fixture
def http(request, probe_http):
    return Http(probe_http, transcript_path(f"raw-{request.node.name}"))


def test_http_session_id_is_visible_ascii(http):
    r = http.req("POST", INIT)
    assert r.status_code == 200 and r.headers["content-type"].startswith("application/json")
    sid = r.headers["mcp-session-id"]
    assert sid and all(0x21 <= ord(ch) <= 0x7E for ch in sid)


def test_http_notification_and_response_get_202_without_body(http):
    h = http.session()
    r = http.req("POST", {"jsonrpc": "2.0", "method": "notifications/cancelled",
                          "params": {"requestId": 1}}, h)
    assert r.status_code == 202 and r.content == b""
    r = http.req("POST", {"jsonrpc": "2.0", "id": "srv-1", "result": {}}, h)
    assert r.status_code == 202 and r.content == b""


def test_http_origin_validation(http):
    r = http.req("POST", INIT, {"Origin": "http://evil.example"})
    assert r.status_code == 403
    r = http.req("POST", INIT, {"Origin": "http://allowed.example"})
    assert r.status_code == 200


def test_http_host_validation(http):
    # #499: a loopback server answers only loopback Host names, with or
    # without an Origin, so a DNS-rebinding page is refused either way.
    r = http.req("POST", INIT, {"Host": "rebind.example"})
    assert r.status_code == 403 and r.json()["error"]["code"] == -32003
    r = http.req("POST", INIT, {"Host": "rebind.example", "Origin": "http://allowed.example"})
    assert r.status_code == 403
    r = http.req("POST", INIT, {"Host": "localhost:1"})
    assert r.status_code == 200


def test_http_session_required_and_unknown_session_404(http):
    r = http.req("POST", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                 {"MCP-Protocol-Version": "2025-11-25"})
    assert r.status_code == 400
    r = http.req("POST", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                 {"Mcp-Session-Id": "ng_doesnotexist", "MCP-Protocol-Version": "2025-11-25"})
    assert r.status_code == 404


def test_http_delete_terminates_session(http):
    h = http.session()
    assert http.req("DELETE", None, {**h, "Connection": "close"}).status_code in (200, 202, 204)
    r = http.req("POST", {"jsonrpc": "2.0", "id": 3, "method": "ping"}, h)
    assert r.status_code == 404


def test_http_connection_survives_delete_with_json_content_type(http):
    """The official Python client sends DELETE with Content-Type: application/json and no
    body; the keep-alive connection must stay usable (RFC 9112 message framing)."""
    h = http.session()
    assert http.req("DELETE", None, h).status_code == 204
    r = http.req("POST", INIT)  # same pooled connection
    assert r.status_code == 200


def test_http_invalid_protocol_version_header_is_400(http):
    h = http.session()
    for bad in ("1999-01-01", "2026-07-28", "garbage"):
        r = http.req("POST", {"jsonrpc": "2.0", "id": 4, "method": "ping"},
                     {**h, "MCP-Protocol-Version": bad})
        assert r.status_code == 400


def test_http_other_supported_protocol_version_header_is_served(http):
    """transports 'Protocol Version Header': 400 is for an invalid or unsupported revision.
    Another supported one is served under the negotiated revision, as the official
    TypeScript server does (conformance 'server-sse-multiple-streams' sends 2025-03-26)."""
    h = http.session()
    for other in ("2025-06-18", "2025-03-26", "2024-11-05"):
        r = http.req("POST", {"jsonrpc": "2.0", "id": 6, "method": "ping"},
                     {**h, "MCP-Protocol-Version": other})
        assert r.status_code == 200, f"{other}: {r.status_code} {r.text}"


def test_http_missing_protocol_version_header_uses_negotiated_version(http):
    """transports 'Protocol Version Header': with no header the server SHOULD rely on the
    version negotiated at initialization (only absent that, assume 2025-03-26)."""
    h = http.session()
    r = http.req("POST", {"jsonrpc": "2.0", "id": 5, "method": "ping"},
                 {**h, "MCP-Protocol-Version": None})
    assert r.status_code == 200, f"{r.status_code} {r.text}"


def test_http_get_without_sse_is_405(http):
    h = http.session()
    r = http.req("GET", None, {**h, "Accept": "text/event-stream", "Content-Type": None})
    assert r.status_code == 405
    r = http.req("GET", None, {**h, "Accept": "text/event-stream", "Content-Type": None,
                               "Last-Event-ID": "x"})
    assert r.status_code == 405


def test_http_batch_rejected(http):
    h = http.session()
    r = http.req("POST", [{"jsonrpc": "2.0", "id": 6, "method": "ping"}], h)
    assert r.status_code == 400


@pytest.mark.parametrize("revision", ["2025-03-26", "2024-11-05"])
def test_http_batch_answered_for_revisions_with_batching(http, revision):
    """transports (2025-03-26): a POST body may be a batch; with requests in it the
    answer is one JSON array, with only notifications it is 202."""
    h = http.session(revision)
    r = http.req("POST", [{"jsonrpc": "2.0", "id": 6, "method": "ping"},
                          {"jsonrpc": "2.0", "id": 7, "method": "tools/call",
                           "params": {"name": "echo", "arguments": {"message": "h"}}}], h)
    assert r.status_code == 200, r.text
    assert r.headers["content-type"].startswith("application/json")
    body = r.json()
    assert isinstance(body, list) and sorted(x["id"] for x in body) == [6, 7], body
    note = http.req("POST", [{"jsonrpc": "2.0", "method": "notifications/progress",
                              "params": {"progressToken": 1, "progress": 1}}], h)
    assert note.status_code == 202 and not note.text


def test_http_error_bodies_have_no_null_id(http):
    """transports: 4xx bodies MAY be a JSON-RPC error response *that has no id*."""
    r = http.req("POST", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    body = r.json()
    assert r.status_code == 400 and "id" not in body, json.dumps(body)


def test_http_accept_header_requirements(http):
    r = http.req("POST", INIT, {"Accept": "application/json"})
    assert r.status_code == 406
    r = http.req("POST", INIT, {"Accept": "*/*"})
    assert r.status_code in (200, 406)


def test_http_modern_2026_request_falls_back_cleanly(http):
    """A dual-era client's first 2026-07-28 request must yield a 4xx without a recognized
    modern error, so the client falls back to initialize (versioning 'Backward Compatibility')."""
    body = {"jsonrpc": "2.0", "id": 1, "method": "server/discover",
            "params": {"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}}}
    r = http.req("POST", body, {"MCP-Protocol-Version": "2026-07-28",
                                "Mcp-Method": "server/discover"})
    assert 400 <= r.status_code < 500
    assert r.json().get("error", {}).get("code") not in (-32020, -32021, -32022)


def test_http_bearer_auth_challenge_and_metadata(request):
    log = transcript_path(f"raw-{request.node.name}")
    with http_server(probe_launch({"NG_PROBE_BEARER": "s3cret"})) as server:
        port = server.port
        h = Http(port, log)
        r = h.req("POST", INIT)
        assert r.status_code == 401
        challenge = r.headers.get("www-authenticate", "")
        assert challenge.startswith("Bearer")
        ok = h.req("POST", INIT, {"Authorization": "Bearer s3cret"})
        assert ok.status_code == 200
        # authorization: servers MUST implement RFC 9728 metadata discovery (resource_metadata
        # in WWW-Authenticate, or a well-known URI) when HTTP authorization is supported
        meta = httpx.get(f"http://127.0.0.1:{port}/.well-known/oauth-protected-resource/mcp")
        meta_root = httpx.get(f"http://127.0.0.1:{port}/.well-known/oauth-protected-resource")
        assert ("resource_metadata=" in challenge or meta.status_code == 200
                or meta_root.status_code == 200), (
            f"401 challenge {challenge!r}; well-known: {meta.status_code}/{meta_root.status_code}")


# --- JSON Schema 2020-12 (basic 'JSON Schema Usage': MUST support 2020-12) ----------

def test_input_validation_enforces_2020_12_keywords(stdio):
    """`decorated` declares {"n": {"type": "integer", "minimum": 0}}; n=-1 must be rejected
    as a tool execution error (SEP-1303), not executed."""
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "id": 21, "method": "tools/call",
                "params": {"name": "decorated", "arguments": {"n": -1}}})
    r = stdio.recv()
    assert r["result"].get("isError") is True, "minimum ignored, tool executed: " + json.dumps(r)


def test_input_validation_accepts_integral_number_as_integer(stdio):
    """JSON Schema 2020-12 s6.1.1: 'integer' matches any number with a zero fractional part."""
    stdio.init()
    stdio.send({"jsonrpc": "2.0", "id": 22, "method": "tools/call",
                "params": {"name": "decorated", "arguments": {"n": 1.0}}})
    r = stdio.recv()
    assert r["result"].get("isError") is not True, "1.0 rejected as integer: " + json.dumps(r)


def test_http_cancellation_not_starved_by_in_flight_calls(http):
    """With as many in-flight tools/call POSTs as HTTP worker threads (default 8), a
    notifications/cancelled POST must still be processed promptly."""
    h = http.session()
    calls = [threading.Thread(target=lambda i=i: httpx.post(
        http.base, headers={"Accept": ACCEPT, "Content-Type": "application/json", **h},
        json={"jsonrpc": "2.0", "id": 100 + i, "method": "tools/call",
              "params": {"name": "slow", "arguments": {"seconds": 3}}}, timeout=30))
        for i in range(8)]
    for t in calls:
        t.start()
    time.sleep(0.5)
    # Every pooled connection of a real client is busy with a call, so the cancel
    # travels on a new connection.
    t0 = time.monotonic()
    http.c.close()
    http.c = httpx.Client(timeout=30)
    r = http.req("POST", {"jsonrpc": "2.0", "method": "notifications/cancelled",
                          "params": {"requestId": 100}}, h)
    waited = time.monotonic() - t0
    for t in calls:
        t.join()
    assert r.status_code == 202
    assert waited < 1.0, f"cancellation waited {waited:.2f}s behind in-flight calls"
