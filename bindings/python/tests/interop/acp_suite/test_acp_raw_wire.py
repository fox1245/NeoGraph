"""Raw NDJSON probes of the native NeoGraph ACPServer (stdlib + pytest).

These cover what an official SDK client never sends on purpose: malformed
JSON, batches, out-of-order lifecycle calls, version edge cases and malformed
content blocks. Expected outcomes come from the ACP v1 schema (ErrorCode,
ProtocolVersion, ContentBlock) and JSON-RPC 2.0. Every message the agent sends
is also validated against the vendored schema (spec/schema.json, v1.25.0).

Environment: NEOGRAPH_ACP_PROBE_AGENT (else <build>/tests/neograph_acp_probe_agent),
optional NEOGRAPH_ACP_EXAMPLE (<build>/example_acp_server) and ACP_TRANSCRIPT_DIR.
"""

import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from acp_interop_support import (  # noqa: E402
    TAP,
    neograph_build_dir,
    require_module,
    require_probe_agent,
    transcript_dir,
)

require_module("jsonschema")
AGENT = require_probe_agent()

from acp_schema_check import agent_problems  # noqa: E402

_example = os.environ.get("NEOGRAPH_ACP_EXAMPLE") or (
    str(Path(neograph_build_dir()) / "example_acp_server") if neograph_build_dir() else "")
EXAMPLE = _example if _example and Path(_example).is_file() else ""
TRANSCRIPTS = transcript_dir("raw")

INIT = {"protocolVersion": 1, "clientCapabilities": {}, "clientInfo": {"name": "raw-probe", "version": "1"}}


class Wire:
    def __init__(self, name, binary=None, args=()):
        TRANSCRIPTS.mkdir(parents=True, exist_ok=True)
        self.transcript = TRANSCRIPTS / f"{name}.jsonl"
        if self.transcript.exists():
            self.transcript.unlink()
        self.proc = subprocess.Popen(
            [sys.executable, "-I", str(TAP), str(self.transcript), "--", str(binary or AGENT), *args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.inbox = queue.Queue()
        self.seen = []
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.inbox.put(line.decode("utf-8", "replace").rstrip("\n"))

    def send_raw(self, text):
        self.proc.stdin.write(text.encode() + b"\n")
        self.proc.stdin.flush()

    def send(self, obj):
        self.send_raw(json.dumps(obj))

    def request(self, rid, method, params=None):
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        return self.wait(lambda m: "method" not in m and m.get("id") == rid)

    def wait(self, pred, timeout=5.0):
        deadline = time.time() + timeout
        for m in self.seen:
            if pred(m):
                self.seen.remove(m)
                return m
        while time.time() < deadline:
            try:
                raw = self.inbox.get(timeout=max(0.01, deadline - time.time()))
            except queue.Empty:
                break
            try:
                m = json.loads(raw)
            except ValueError:
                m = {"__invalid__": raw}
            if isinstance(m, dict) and pred(m):
                return m
            self.seen.append(m)
        return None

    def drain(self, seconds=0.3):
        time.sleep(seconds)
        out = list(self.seen)
        self.seen.clear()
        while True:
            try:
                raw = self.inbox.get_nowait()
            except queue.Empty:
                return out
            try:
                out.append(json.loads(raw))
            except ValueError:
                out.append({"__invalid__": raw})

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        # Close the pipe explicitly: a pipe left to the garbage collector
        # raises a ResourceWarning inside a later test.
        self.reader.join(2)
        if self.proc.stdout:
            self.proc.stdout.close()

    def schema_problems(self):
        """Agent messages that fail the ACP schema or use an unnamed error code."""
        return agent_problems(self.transcript)


@pytest.fixture
def wire(request):
    w = Wire(re.sub(r"[^A-Za-z0-9_.-]+", "_", request.node.name).strip("_"))
    yield w
    w.close()
    problems = w.schema_problems()
    assert not problems, "agent messages fail the ACP schema:\n" + "\n".join(problems)


def init(w):
    resp = w.request(1, "initialize", INIT)
    assert resp and resp["result"]["protocolVersion"] == 1
    return resp


def new_session(w, rid=2):
    return w.request(rid, "session/new", {"cwd": "/tmp", "mcpServers": []})["result"]["sessionId"]


def code(resp):
    return (resp or {}).get("error", {}).get("code")


# --- JSON-RPC framing -------------------------------------------------------

def test_parse_error_has_null_id_and_connection_survives(wire):
    wire.send_raw("{not json")
    err = wire.wait(lambda m: "error" in m)
    assert err == {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}
    assert init(wire)


def test_unknown_request_method_not_found_echoes_string_id(wire):
    init(wire)
    resp = wire.request("abc", "session/does_not_exist", {})
    assert resp["id"] == "abc" and code(resp) == -32601


def test_unknown_notification_is_ignored(wire):
    init(wire)
    wire.send({"jsonrpc": "2.0", "method": "$/cancel_request", "params": {"requestId": 99}})
    wire.send({"jsonrpc": "2.0", "method": "_vendor/notice", "params": {}})
    assert wire.drain() == []


def test_session_cancel_as_request_is_invalid_request(wire):
    init(wire)
    sid = new_session(wire)
    assert code(wire.request(3, "session/cancel", {"sessionId": sid})) == -32600


def test_batch_request(wire):
    # JSON-RPC 2.0 batches; ACP defines no batch use. Record the behaviour.
    wire.send([{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": INIT}])
    resp = wire.wait(lambda m: True)
    assert code(resp) == -32600, resp


# --- initialize -----------------------------------------------------------------

@pytest.mark.parametrize("version,expected", [(0, 1), (1, 1), (2, 1), (65535, 1)])
def test_protocol_version_negotiation(wire, version, expected):
    resp = wire.request(1, "initialize", {"protocolVersion": version, "clientCapabilities": {}})
    assert resp["result"]["protocolVersion"] == expected, resp


@pytest.mark.parametrize("version", [-1, 65536, "1", 1.5, None])
def test_protocol_version_outside_uint16_is_invalid_params(wire, version):
    resp = wire.request(1, "initialize", {"protocolVersion": version, "clientCapabilities": {}})
    assert code(resp) == -32602, resp


def test_initialize_twice_is_rejected(wire):
    init(wire)
    assert code(wire.request(2, "initialize", INIT)) == -32600


@pytest.mark.parametrize("method,params", [
    ("session/new", {"cwd": "/tmp", "mcpServers": []}),
    ("session/prompt", {"sessionId": "x", "prompt": [{"type": "text", "text": "hi"}]}),
])
def test_call_before_initialize_does_not_claim_auth_required(wire, method, params):
    resp = wire.request(7, method, params)
    assert "error" in resp, resp
    # Not -32000: ACP reserves it for "Authentication required".
    assert code(resp) == -32600, f"pre-initialize {method} is not an invalid request: {resp}"


# --- session/new ------------------------------------------------------------------

@pytest.mark.parametrize("params", [
    {"cwd": "/tmp"},                                         # mcpServers is required
    {"cwd": "tmp", "mcpServers": []},                        # cwd MUST be absolute
    {"cwd": "/tmp", "mcpServers": [], "additionalDirectories": ["rel"]},
])
def test_session_new_invalid_params(wire, params):
    init(wire)
    assert code(wire.request(2, "session/new", params)) == -32602


# --- session/prompt content validation ---------------------------------------------

@pytest.mark.parametrize("block", [
    {"type": "text"},                                        # TextContent requires text
    {"type": "resource_link", "uri": "file:///x"},           # ResourceLink requires name
    {"type": "nonsense"},                                    # not a ContentBlock
    {"type": "image", "data": "aGVsbG8=", "mimeType": "image/png"},   # not advertised
    {"type": "resource", "resource": {"uri": "file:///x", "text": "t"}},  # not advertised
])
def test_malformed_content_block_is_invalid_params(wire, block):
    init(wire)
    sid = new_session(wire)
    resp = wire.request(3, "session/prompt", {"sessionId": sid, "prompt": [block]})
    assert code(resp) == -32602, f"malformed block {block} accepted: {resp}"


# --- stdout hygiene of the shipped example ---------------------------------------

@pytest.mark.skipif(not EXAMPLE or not Path(EXAMPLE).exists(), reason="set NEOGRAPH_ACP_EXAMPLE")
def test_shipped_example_keeps_stdout_clean():
    w = Wire("example_acp_server_stdout", binary=EXAMPLE)
    try:
        init(w)
        sid = new_session(w)
        resp = w.request(3, "session/prompt", {"sessionId": sid, "prompt": [{"type": "text", "text": "hi"}]})
        assert resp["result"]["stopReason"] == "end_turn"
    finally:
        w.close()
    rows = [json.loads(line) for line in w.transcript.read_text().splitlines()]
    bad = [r for r in rows if r["dir"] == "a2c" and (r.get("invalid") or r.get("invalid_utf8"))]
    assert not bad, bad
    assert not w.schema_problems()


# --- cancellation reach, lifecycle and metadata -------------------------------------

def test_cancel_reaches_a_pending_agent_to_client_request():
    # prompt-turn.mdx: on session/cancel the Agent SHOULD stop all operations
    # as soon as possible. Here the client never answers fs/read_text_file.
    w = Wire("cancel_with_pending_fs_request")
    try:
        caps = {"protocolVersion": 1, "clientCapabilities": {"fs": {"readTextFile": True}}}
        assert w.request(1, "initialize", caps)
        sid = new_session(w)
        w.send({"jsonrpc": "2.0", "id": 3, "method": "session/prompt",
                "params": {"sessionId": sid, "prompt": [{"type": "text", "text": "fsread /tmp/never"}]}})
        outbound = w.wait(lambda m: m.get("method") == "fs/read_text_file")
        assert outbound, "agent never asked for the file"
        start = time.time()
        w.send({"jsonrpc": "2.0", "method": "session/cancel", "params": {"sessionId": sid}})
        resp = w.wait(lambda m: m.get("id") == 3 and "method" not in m, timeout=15)
        latency = time.time() - start
        cancel_req = [m for m in w.drain(0.1) if m.get("method") == "$/cancel_request"]
        assert resp and resp["result"]["stopReason"] == "cancelled", resp
        assert latency < 2.0, (f"cancelled response took {latency:.1f}s: the pending fs/read_text_file "
                               f"(id {outbound['id']}) was not abandoned; $/cancel_request sent: {bool(cancel_req)}")
        # cancellation: the abandoned agent->client request is cancelled too.
        assert [m["params"]["requestId"] for m in cancel_req] == [outbound["id"]], cancel_req
    finally:
        w.close()
    assert not w.schema_problems()


def test_stdin_eof_cancels_inflight_prompt():
    w = Wire("eof_with_inflight_prompt")
    init(w)
    sid = new_session(w)
    w.send({"jsonrpc": "2.0", "id": 3, "method": "session/prompt",
            "params": {"sessionId": sid, "prompt": [{"type": "text", "text": "slow"}]}})
    time.sleep(0.3)
    start = time.time()
    w.proc.stdin.close()
    try:
        w.proc.wait(30)
    except subprocess.TimeoutExpired:
        w.proc.kill()
    took = time.time() - start
    w.close()
    assert took < 3.0, f"agent kept running {took:.1f}s after the client closed stdin (in-flight prompt not cancelled)"
    assert not w.schema_problems()


def test_resume_of_live_session_without_checkpoint(wire):
    init(wire)
    sid = new_session(wire)
    resp = wire.request(3, "session/resume", {"sessionId": sid, "cwd": "/tmp", "mcpServers": []})
    assert "result" in resp, f"resuming a session created on this connection failed: {resp}"


@pytest.mark.parametrize("info", ['{"name":"only-name"}', '"free-form string"'])
def test_agent_info_is_a_valid_implementation(info):
    w = Wire("agent_info_" + ("name_only" if "only" in info else "string"), args=("--agent-info", info))
    try:
        resp = w.request(1, "initialize", INIT)
    finally:
        w.close()
    agent_info = resp["result"].get("agentInfo")
    assert isinstance(agent_info, dict) and isinstance(agent_info.get("name"), str) \
        and isinstance(agent_info.get("version"), str), f"agentInfo violates Implementation: {resp}"
    assert not w.schema_problems()
