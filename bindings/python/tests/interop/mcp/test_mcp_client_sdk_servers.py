"""NeoGraph's MCP client against the official MCP SDK servers (MCP 2025-11-25).

Direction: NeoGraph client -> official server.  Peers (see _mcp_interop_support):
  * Python `mcp` 2.3.0: lowlevel Server and MCPServer, stdio and Streamable HTTP
    (stateful/stateless x SSE/JSON responses)
  * TypeScript `@modelcontextprotocol/sdk` 1.32.1: low-level Server, stdio and
    Streamable HTTP (stateful/stateless x SSE/JSON responses)

Each test asserts what the MCP 2025-11-25 specification (or its schema.json)
requires of a client.  A failing test is a conformance or interop defect; the
assertion message carries the NeoGraph exception and the offending wire message.
No network beyond loopback, no model calls, no keys.
"""

from __future__ import annotations

import gc
import json
import threading
import time

import pytest

from mcp_sdk_requirement import require_official_python_sdk

# The official Python servers run on this interpreter's pinned SDK
# (requirements-mcp-interop.txt); skipped without it.
require_official_python_sdk()
pytest.importorskip("uvicorn")
pytest.importorskip("starlette")
pytest.importorskip("httpx")
pytest.importorskip("jsonschema")

from mcp_interop_support import (MCP1_PYTHON, MCPSERVER_PEERS, NODE_DEPS, PEERS,  # noqa: E402
                                 PY1_PEERS, TS2_PEERS, Peer, Run, ng, oracle, record_result,
                                 server, short)

REVISION = "2025-11-25"


def _params(peers, pred=lambda p: True):
    out = []
    for p in peers:
        if not pred(p):
            continue
        marks = []
        if p.sdk in ("ts", "ts2") and not NODE_DEPS:
            marks.append(pytest.mark.skip(reason="NG_MCP_NODE_DEPS not set"))
        if p.sdk == "py1" and not MCP1_PYTHON:
            marks.append(pytest.mark.skip(reason="NG_MCP1_PYTHON not set"))
        out.append(pytest.param(p, id=p.id, marks=marks))
    return out


ALL = _params(PEERS)
HTTP = _params(PEERS, lambda p: p.transport == "http")
STATEFUL = _params(PEERS, lambda p: p.stateful_http)
STDIO = _params(PEERS, lambda p: p.transport == "stdio")
SSE_HTTP = _params(PEERS, lambda p: p.transport == "http" and not p.json_response)
# Peers that can deliver a server->client request while a client request is open:
# stdio, and stateful Streamable HTTP with SSE responses.  (JSON-response and
# stateless servers have no back-channel; the Python SDK raises NoBackChannelError.)
SERVER_REQ = _params(PEERS, lambda p: p.transport == "stdio" or (p.stateful_http and not p.json_response))


def _name(request) -> str:
    return request.node.name.replace("/", "_").replace("[", "__").replace("]", "")


def _messages(run: Run, method: str | None = None):
    out = []
    for m in run.client_messages():
        msg = m["message"]
        if isinstance(msg, dict) and (method is None or msg.get("method") == method):
            out.append(m)
    return out


def _initialized(run: Run):
    client = run.client()
    client.initialize("ng-interop")
    return client


# --------------------------------------------------------------------------- lifecycle

@pytest.mark.parametrize("peer", ALL)
def test_initialize_handshake(peer: Peer, request):
    """Lifecycle: initialize request, version negotiation, notifications/initialized."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        result = client.get_initialize_result()
        sent = [m["message"] for m in _messages(run)]
        methods = [m.get("method") for m in sent]
        assert result.protocol_version == REVISION
        assert methods[:2] == ["initialize", "notifications/initialized"], methods
        init = sent[0]
        assert init["params"]["protocolVersion"] == REVISION
        assert isinstance(init["params"]["capabilities"], dict)
        assert {"name", "version"} <= set(init["params"]["clientInfo"])
        if peer.transport == "http":
            note = _messages(run, "notifications/initialized")[0]["http"]
            assert note["status"] == 202, note


@pytest.mark.parametrize("peer", HTTP)
def test_http_request_headers(peer: Peer, request):
    """Streamable HTTP: Accept, MCP-Protocol-Version and Mcp-Session-Id on every later request."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        client.get_tool_definitions()
        client.call_tool("echo", {"text": "hdr"})
        exchanges = run.http_exchanges()
        init = exchanges[0]
        session = init["http"]["response_headers"].get("mcp-session-id")
        problems = []
        for ex in exchanges:
            h = ex["http"]["headers"]
            accept = h.get("accept", "")
            # A POST accepts both; the optional GET event stream asks for
            # text/event-stream (transports, "Listening for Messages").
            wanted = (("text/event-stream",) if ex["http"]["method"] == "GET"
                      else ("application/json", "text/event-stream"))
            if not all(kind in accept for kind in wanted):
                problems.append(("accept", ex["http"]["method"], accept))
            if ex is init:
                continue
            if h.get("mcp-protocol-version") != REVISION:
                problems.append(("mcp-protocol-version", short(ex["message"]), h.get("mcp-protocol-version")))
            if peer.stateful_http and h.get("mcp-session-id") != session:
                problems.append(("mcp-session-id", short(ex["message"]), h.get("mcp-session-id")))
            if ex["http"]["status"] >= 400:
                problems.append(("rejected", ex["http"]["status"], short(ex["http"]["response_body"])))
        assert not problems, problems


# --------------------------------------------------------------------------- tools

@pytest.mark.parametrize("peer", ALL)
def test_tools_list_pagination(peer: Peer, request):
    with server(peer, _name(request), extra=["--paginate"]) as run:
        client = _initialized(run)
        names = [d.name for d in client.get_tool_definitions()]
        assert len(names) == len(set(names)) >= 11 and names[0] == "echo", names
        cursors = [m["message"].get("params", {}).get("cursor") for m in _messages(run, "tools/list")]
        assert cursors == [None, "4", "8"], cursors


@pytest.mark.parametrize("peer", ALL)
def test_call_text_structured_and_metadata(peer: Peer, request):
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        defs = {d.name: d for d in client.get_tool_definitions()}
        assert defs["structured"].output_schema["required"] == ["answer"]
        assert defs["structured"].annotations.get("readOnlyHint") is True
        assert client.call_tool("echo", {"text": "hi"})["content"][0]["text"] == "hi"
        res = client.call_tool_result("structured", {"n": 1})
        assert res.structured_content == {"answer": 2} and not res.is_error
        res = client.call_tool_result("structured_only", {})
        assert res.structured_content == {"answer": 7} and res.content == []


@pytest.mark.parametrize("peer", ALL)
def test_is_error_result_is_data(peer: Peer, request):
    """Tool execution errors are results with isError=true, not protocol errors."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        client.get_tool_definitions()
        res = client.call_tool_result("tool_error", {})
        assert res.is_error and res.content[0]["text"] == "boom: tool failed"
        res = client.call_tool_result("tool_error_with_schema", {})
        assert res.is_error and res.content[0]["text"] == "boom: schema tool failed"


@pytest.mark.parametrize("peer", ALL)
def test_jsonrpc_error_is_raised_with_code(peer: Peer, request):
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        with pytest.raises(ng.mcp.MCPError) as info:
            client.call_tool("protocol_error", {})
        assert "Invalid params: probe" in str(info.value)
        code = getattr(info.value, "code", None)
        record_result(request.node.name, {"python_error_code_attr": code})
        assert code == -32602, (
            "the JSON-RPC error code is not reachable from Python "
            f"(ng.mcp.MCPError has no 'code'; message={str(info.value)!r})")


@pytest.mark.parametrize("peer", ALL)
def test_all_content_block_types_preserved(peer: Peer, request):
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        res = client.call_tool_result("all_content", {})
        types = [b["type"] for b in res.content]
        assert types == ["text", "image", "audio", "resource_link", "resource", "resource"], types
        assert res.content[4]["resource"]["text"] == "inline text"
        assert "blob" in res.content[5]["resource"]


# --------------------------------------------------------------------------- streams and server->client

@pytest.mark.parametrize("peer", ALL)
def test_notifications_interleaved_before_response(peer: Peer, request):
    """Server notifications related to a request may precede its response (SSE or stdio)."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        try:
            res = client.call_tool_result("log_then_answer", {})
        except Exception as exc:  # noqa: BLE001
            body = ""
            calls = _messages(run, "tools/call")
            if calls and "http" in calls[-1]:
                body = calls[-1]["http"]["response_body"]
            pytest.fail(f"NeoGraph rejected a valid response stream: {type(exc).__name__}: {exc}\n"
                        f"server response body (repr): {body[:700]!r}")
        assert res.content[0]["text"] == "answered after logs"


@pytest.mark.parametrize("peer", SERVER_REQ)
def test_server_ping_is_answered(peer: Peer, request):
    """basic/utilities/ping: the receiver MUST respond promptly with an empty response."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        started = time.time()
        try:
            res = client.call_tool_result("ping_client", {})
            outcome = {"is_error": res.is_error, "content": res.content}
        except Exception as exc:  # noqa: BLE001
            outcome = {"exception": f"{type(exc).__name__}: {exc}"}
        events = [e for e in run.server_events() if e["event"] == "ping_client"]
        record_result(request.node.name, {"outcome": outcome, "events": events,
                                          "elapsed": round(time.time() - started, 2)})
        assert events and events[-1]["ok"], (
            f"official server's ping got no answer: events={events} client outcome={outcome}")


@pytest.mark.parametrize("peer", SERVER_REQ)
def test_undeclared_server_request_gets_error_response(peer: Peer, request):
    """JSON-RPC 2.0: every request gets a response; an unsupported method -> -32601."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        try:
            client.call_tool_result("roots_client", {})
        except Exception:  # noqa: BLE001
            pass
        events = [e for e in run.server_events() if e["event"] == "roots_client"]
        record_result(request.node.name, {"events": events})
        assert events and events[-1].get("error") == "jsonrpc" and events[-1].get("code") == -32601, (
            f"server-initiated roots/list was not answered with a JSON-RPC error: {events}")


# --------------------------------------------------------------------------- cancellation / timeouts

@pytest.mark.parametrize("peer", HTTP)
def test_request_timeout_sends_cancelled_and_session_survives(peer: Peer, request):
    """basic/utilities/cancellation: on timeout the sender SHOULD send notifications/cancelled."""
    with server(peer, _name(request)) as run:
        client = run.client(request_timeout=1.0)
        client.initialize("ng-interop")
        started = time.time()
        with pytest.raises(Exception) as info:
            client.call_tool("slow", {"seconds": 3})
        elapsed = time.time() - started
        time.sleep(2.5)
        after = None
        try:
            after = client.call_tool("echo", {"text": "after-timeout"})["content"][0]["text"]
        except Exception as exc:  # noqa: BLE001
            after = f"{type(exc).__name__}: {exc}"
        # The server's own record names the slow call: an HTTP recorder
        # cannot log the abandoned POST, which never completes.
        started = [e for e in run.server_events() if e["event"] == "slow_started"]
        slow_id = started[0].get("request_id") if started else None
        cancels = [m["message"] for m in _messages(run, "notifications/cancelled")]
        record_result(request.node.name, {"timeout_exc": str(info.value)[:200], "elapsed": round(elapsed, 2),
                                          "cancels": cancels, "after": after,
                                          "server_events": run.server_events()})
        assert after == "after-timeout", after
        assert any(c["params"].get("requestId") == slow_id for c in cancels), (
            f"no notifications/cancelled for timed-out request id {slow_id}; client sent: "
            f"{[m['message'].get('method') for m in _messages(run)]}")


def _cancel_graph(tool_name: str, arguments: str):
    class Caller(ng.GraphNode):
        def run(self, _input):
            return [ng.ChannelWrite("messages", [{
                "role": "assistant", "content": "",
                "tool_calls": [{"id": "c1", "name": tool_name, "arguments": arguments}],
            }])]

        def get_name(self):
            return "llm"

    ng.NodeFactory.register_type("mcp_cancel_llm", lambda _n, _c, _x: Caller())
    return {
        "name": "mcp_cancel_graph",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "mcp_cancel_llm"}, "tools": {"type": "tool_dispatch"}},
        "edges": [{"from": "__start__", "to": "llm"}, {"from": "llm", "to": "tools"},
                  {"from": "tools", "to": "__end__"}],
    }


@pytest.mark.parametrize("peer", ALL)
def test_run_cancellation_reaches_server(peer: Peer, request):
    """A cancelled NeoGraph run stops promptly and tells the server (notifications/cancelled)."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        tools = [t for t in client.get_tools() if t.get_name() == "slow"]
        engine = ng.GraphEngine.compile(_cancel_graph("slow", '{"seconds": 6}'), ng.NodeContext(tools=tools))
        cfg = ng.RunConfig()
        cfg.thread_id = "cancel-probe"
        token = ng.CancelToken()
        cfg.cancel_token = token
        outcome: dict = {}

        def runner():
            t0 = time.time()
            try:
                engine.run(cfg)
                outcome["result"] = "completed"
            except Exception as exc:  # noqa: BLE001
                outcome["result"] = f"{type(exc).__name__}: {str(exc)[:160]}"
            outcome["returned_at"] = time.time() - t0

        th = threading.Thread(target=runner)
        th.start()
        time.sleep(1.5)
        token.cancel()
        th.join(15)
        time.sleep(1.0)
        cancels = [m["message"] for m in _messages(run, "notifications/cancelled")]
        events = run.server_events()
        record_result(request.node.name, {"outcome": outcome, "cancels": cancels, "server_events": events})
        assert outcome.get("returned_at", 99) < 4.0, f"run did not stop promptly after cancel: {outcome}"
        # The server's own record names the slow call: an HTTP recorder
        # cannot log the abandoned POST, which never completes.
        started = [e for e in events if e["event"] == "slow_started"]
        slow_id = started[-1].get("request_id") if started else None
        assert any(c["params"].get("requestId") == slow_id for c in cancels), (
            f"cancelled run sent no notifications/cancelled for request {slow_id}; "
            f"server events: {[e['event'] for e in events]}")


# --------------------------------------------------------------------------- sessions

@pytest.mark.parametrize("peer", STATEFUL)
def test_session_terminated_by_server_reinitializes(peer: Peer, request):
    """transports#session-management: on HTTP 404 for a session id the client MUST start a new session."""
    import httpx

    with server(peer, _name(request)) as run:
        client = _initialized(run)
        assert client.call_tool("echo", {"text": "one"})["content"][0]["text"] == "one"
        sid = run.http_exchanges()[0]["http"]["response_headers"]["mcp-session-id"]
        r = httpx.delete(run.url, headers={"mcp-session-id": sid, "mcp-protocol-version": REVISION})
        assert r.status_code in (200, 202, 204), r.text
        outcomes = []
        for attempt in range(2):
            try:
                outcomes.append(client.call_tool("echo", {"text": "two"})["content"][0]["text"])
            except Exception as exc:  # noqa: BLE001
                outcomes.append(f"{type(exc).__name__}: {str(exc)[:240]}")
        tail = [(m["http"]["status"], (m["message"] or {}).get("method"), m["http"]["headers"].get("mcp-session-id"))
                for m in run.http_exchanges()[-3:]]
        record_result(request.node.name, {"outcomes": outcomes, "tail": tail})
        assert outcomes[-1] == "two", f"client never re-initialized after 404: {outcomes}; last exchanges {tail}"


@pytest.mark.parametrize("peer", _params(PEERS, lambda p: p.stateful_http and p.sdk == "py"))
def test_idle_session_expiry_by_official_server(peer: Peer, request):
    """The Python SDK reaps idle sessions (default 30 min); reproduced with a 1.5 s idle timeout."""
    with server(peer, _name(request), extra=["--idle-timeout", "1.5"]) as run:
        client = _initialized(run)
        assert client.call_tool("echo", {"text": "one"})["content"][0]["text"] == "one"
        time.sleep(4.0)
        try:
            out = client.call_tool("echo", {"text": "two"})["content"][0]["text"]
        except Exception as exc:  # noqa: BLE001
            out = f"{type(exc).__name__}: {str(exc)[:300]}"
        record_result(request.node.name, {"after_idle": out})
        assert out == "two", out


@pytest.mark.parametrize("peer", STATEFUL)
def test_server_restart_reinitializes(peer: Peer, request):
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        assert client.call_tool("echo", {"text": "one"})["content"][0]["text"] == "one"
        run.restart()
        try:
            out = client.call_tool("echo", {"text": "two"})["content"][0]["text"]
        except Exception as exc:  # noqa: BLE001
            out = f"{type(exc).__name__}: {str(exc)[:300]}"
        assert out == "two", out


@pytest.mark.parametrize("peer", STATEFUL)
def test_delete_session_on_close(peer: Peer, request):
    """transports#session-management: clients SHOULD send DELETE when they leave a session."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        client.call_tool("echo", {"text": "x"})
        sid = run.http_exchanges()[0]["http"]["response_headers"]["mcp-session-id"]
        del client
        gc.collect()
        time.sleep(1.0)
        deletes = [m for m in run.http_exchanges() if m["http"]["method"] == "DELETE"]
        assert deletes and deletes[0]["http"]["headers"].get("mcp-session-id") == sid, (
            f"no DELETE for session {sid}; methods seen: {[m['http']['method'] for m in run.http_exchanges()]}")


@pytest.mark.parametrize("peer", _params(PEERS, lambda p: p.transport == "http" and p.sdk == "ts"
                                            and p.stateful_http and not p.json_response))
@pytest.mark.parametrize("path", ["/", "/mcp/", "/v1/rpc"])
def test_endpoint_url_used_verbatim(peer: Peer, path: str, request):
    """transports#streamable-http: the server exposes ONE MCP endpoint URL; the client must use it."""
    with server(peer, _name(request), path=path) as run:
        try:
            client = run.client(url=run.url)
            client.initialize("ng-interop")
            out = "ok"
        except Exception as exc:  # noqa: BLE001
            out = f"{type(exc).__name__}: {str(exc)[:240]}"
        posted = [m["http"]["path"] for m in run.http_exchanges()]
        record_result(request.node.name, {"configured": run.url, "posted_paths": posted, "outcome": out})
        assert out == "ok" and posted and all(p == path for p in posted), (
            f"configured {run.url!r}; client POSTed to {posted}; outcome {out}")


# --------------------------------------------------------------------------- version negotiation (stdio tap)

@pytest.mark.parametrize("peer", _params(PEERS, lambda p: p.transport == "stdio" and p.sdk == "py"))
@pytest.mark.parametrize("answer,acceptable", [("2025-06-18", True), ("1999-01-01", False)])
def test_server_version_answer(peer: Peer, answer: str, acceptable: bool, request):
    """lifecycle#version-negotiation: unsupported server version -> client SHOULD disconnect."""
    with server(peer, _name(request), tap_args=["--rewrite-init-version", answer]) as run:
        client = run.client()
        try:
            client.initialize("ng-interop")
            out = ("accepted", client.get_initialize_result().protocol_version)
        except Exception as exc:  # noqa: BLE001
            out = ("rejected", f"{type(exc).__name__}: {exc}")
        record_result(request.node.name, {"server_answer": answer, "client": out})
        if acceptable:
            assert out[0] == "accepted", out
        else:
            assert out[0] == "rejected", (
                f"client accepted unsupported protocolVersion {answer!r} instead of disconnecting: {out}")


# --------------------------------------------------------------------------- stdio lifecycle

@pytest.mark.parametrize("peer", STDIO)
def test_stdio_shutdown_closes_stdin_first(peer: Peer, request):
    """lifecycle#stdio shutdown: close the server's stdin first, then SIGTERM, then SIGKILL."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        client.call_tool("echo", {"text": "x"})
        del client
        gc.collect()
        time.sleep(1.5)
        meta = [e.get("event") for e in run.wire_entries() if e.get("dir") == "meta"]
        record_result(request.node.name, {"meta": meta})
        first = next((m for m in meta if m in ("client_closed_stdin", "tap_signal")), None)
        assert first == "client_closed_stdin", f"shutdown order seen by the server side: {meta}"


# --------------------------------------------------------------------------- schema oracle

@pytest.mark.parametrize("peer", ALL)
def test_client_messages_match_schema(peer: Peer, request):
    """Every client-originated message validates against schema.json (2025-11-25)."""
    o = oracle()
    if o is None:
        pytest.skip("NG_MCP_SCHEMA not set")
    with server(peer, _name(request), extra=["--paginate"]) as run:
        client = _initialized(run)
        client.get_tool_definitions()
        for name, args in [("echo", {"text": "s"}), ("structured", {"n": 2}), ("tool_error", {}),
                           ("all_content", {}), ("ping_client", {}), ("roots_client", {})]:
            try:
                client.call_tool(name, args)
            except Exception:  # noqa: BLE001
                pass
        server_requests = {}
        for e in run.wire_entries():
            msg = e.get("json") if e.get("dir") == "s2c" else None
            if isinstance(msg, dict) and "method" in msg and "id" in msg:
                server_requests[json.dumps(msg["id"])] = msg["method"]
        violations = []
        count = 0
        for m in run.client_messages():
            msg = m["message"]
            if msg is None and m.get("http", {}).get("method") in ("GET", "DELETE"):
                continue
            count += 1
            errs = o.validate_client_message(msg, server_requests)
            if errs:
                violations.append({"message": short(msg), "errors": errs[:5]})
        record_result(request.node.name, {"validated": count, "violations": violations})
        assert count > 0 and not violations, violations


# --------------------------------------------------------------------------- MCPServer (FastMCP-style) servers

@pytest.mark.parametrize("peer", _params(MCPSERVER_PEERS + PY1_PEERS))
def test_mcpserver_typical_tools(peer: Peer, request):
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        defs = {d.name: d for d in client.get_tool_definitions()}
        assert set(defs) >= {"echo", "add", "weather", "fails", "chatty"}
        assert client.call_tool("echo", {"text": "a"})["content"][0]["text"] == "a"
        add = client.call_tool_result("add", {"a": 1, "b": 2})
        assert add.structured_content == {"result": 3}, add.raw
        weather = client.call_tool_result("weather", {"city": "Seoul"})
        assert weather.structured_content == {"city": "Seoul", "celsius": 21.5}
        fails = client.call_tool_result("fails", {"reason": "x"})
        assert fails.is_error and "tool refused: x" in fails.content[0]["text"]
        try:
            chatty = client.call_tool_result("chatty", {})
            chatty_out = chatty.content[0]["text"]
        except Exception as exc:  # noqa: BLE001
            chatty_out = f"{type(exc).__name__}: {exc}"
        assert chatty_out == "chatty done", chatty_out


# --------------------------------------------------------------------------- SSE priming / resumability (py SDK)

RESUMABLE = _params(PEERS, lambda p: p.sdk == "py" and p.transport == "http" and not p.json_response
                    and not p.stateless)


@pytest.mark.parametrize("peer", RESUMABLE)
def test_resumable_server_priming_event(peer: Peer, request):
    """transports#sending-messages: servers SHOULD prime the SSE stream with an id + empty data event."""
    with server(peer, _name(request), extra=["--event-store"]) as run:
        try:
            client = _initialized(run)
            out = client.call_tool("echo", {"text": "primed"})["content"][0]["text"]
        except Exception as exc:  # noqa: BLE001
            out = f"{type(exc).__name__}: {exc}"
        bodies = [m["http"]["response_body"] for m in run.http_exchanges()]
        record_result(request.node.name, {"outcome": out, "first_body": bodies[0][:300] if bodies else None})
        assert out == "primed", f"{out}\nfirst SSE body (repr): {bodies[0][:300]!r}"


@pytest.mark.parametrize("peer", RESUMABLE)
def test_sse_polling_reconnects_with_last_event_id(peer: Peer, request):
    """transports#resumability: after a server-closed stream the client SHOULD GET with
    Last-Event-ID and MUST respect the retry field."""
    with server(peer, _name(request), extra=["--event-store", "--retry-interval", "300"]) as run:
        try:
            client = _initialized(run)
            out = client.call_tool("poll_then_answer", {})["content"][0]["text"]
        except Exception as exc:  # noqa: BLE001
            out = f"{type(exc).__name__}: {exc}"
        gets = [m["http"]["headers"].get("last-event-id") for m in run.http_exchanges()
                if m["http"]["method"] == "GET"]
        record_result(request.node.name, {"outcome": out, "resume_gets": gets})
        assert out == "answered after poll" and gets, f"outcome={out}; GET resumptions={gets}"


# --------------------------------------------------------------------------- TS SDK v2 (@modelcontextprotocol/server)

@pytest.mark.parametrize("peer", _params(TS2_PEERS))
def test_ts2_core_tools(peer: Peer, request):
    """The v2 TS SDK serves 2025-11-25 clients as legacy traffic; core tool flows must work."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        assert client.get_initialize_result().protocol_version == REVISION
        assert {d.name for d in client.get_tool_definitions()} >= {"echo", "structured"}
        assert client.call_tool("echo", {"text": "v2"})["content"][0]["text"] == "v2"
        assert client.call_tool_result("structured", {"n": 4}).structured_content == {"answer": 5}
        assert client.call_tool_result("tool_error", {}).is_error
        with pytest.raises(ng.mcp.MCPError):
            client.call_tool("protocol_error", {})
        assert client.call_tool_result("log_then_answer", {}).content[0]["text"] == "answered after logs"
        o = oracle()
        if o is not None:
            bad = [(short(m["message"]), o.validate_client_message(m["message"], {}))
                   for m in run.client_messages() if isinstance(m["message"], dict)]
            bad = [b for b in bad if b[1]]
            assert not bad, bad


@pytest.mark.parametrize("peer", _params(TS2_PEERS, lambda p: p.transport == "stdio"))
def test_ts2_server_ping_is_answered(peer: Peer, request):
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        res = client.call_tool_result("ping_client", {})
        events = [e for e in run.server_events() if e["event"] == "ping_client"]
        record_result(request.node.name, {"events": events, "content": res.content})
        assert events and events[-1]["ok"], f"official v2 server's ping got no answer: {events}"


@pytest.mark.parametrize("peer", _params(PY1_PEERS, lambda p: p.transport == "stdio"
                                            or (p.stateful_http and not p.json_response)))
def test_py1_server_ping_is_answered(peer: Peer, request):
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        try:
            res = client.call_tool_result("ping_client", {})
            out = res.content
        except Exception as exc:  # noqa: BLE001
            out = f"{type(exc).__name__}: {exc}"
        events = [e for e in run.server_events() if e["event"] == "ping_client"]
        record_result(request.node.name, {"events": events, "client": out})
        assert events and events[-1]["ok"], f"FastMCP v1 server's ping got no answer: {events}; client saw {out}"


@pytest.mark.parametrize("peer", _params(PEERS, lambda p: p.sdk == "py" and not p.stateless
                                            and not p.json_response))
def test_server_request_id_does_not_collide_with_client_ids(peer: Peer, request):
    """JSON-RPC: a request from the server is never a response to the client's request,
    even when both sides happen to use the same numeric id."""
    with server(peer, _name(request)) as run:
        client = _initialized(run)
        try:
            res = client.call_tool_result("ping_client", {"count": 3, "timeout": 0.7})
            out = {"is_error": res.is_error, "content": res.content}
        except Exception as exc:  # noqa: BLE001
            out = {"exception": f"{type(exc).__name__}: {exc}"}
        call_id = _messages(run, "tools/call")[-1]["message"]["id"]
        if peer.transport == "stdio":
            server_reqs = [e["json"] for e in run.wire_entries()
                           if e.get("dir") == "s2c" and isinstance(e.get("json"), dict) and "method" in e["json"]
                           and "id" in e["json"]]
        else:
            body = _messages(run, "tools/call")[-1]["http"]["response_body"]
            server_reqs = [json.loads(line[5:].strip()) for line in body.replace("\r\n", "\n").split("\n")
                           if line.startswith("data:") and '"method":"ping"' in line.replace(" ", "")]
        record_result(request.node.name, {"client_call_id": call_id, "server_request_ids":
                                          [r.get("id") for r in server_reqs], "client": out})
        assert "exception" not in out, (
            f"tools/call id={call_id} failed while the server sent requests with ids "
            f"{[r.get('id') for r in server_reqs]}: {out}")


@pytest.mark.xfail(strict=True, reason="MCP 2026-07-28 (no initialize, per-request _meta) "
                                       "is a later client mode")
@pytest.mark.parametrize("peer", _params(TS2_PEERS, lambda p: p.transport == "http"))
def test_ts2_modern_only_server_is_reachable(peer: Peer, request):
    """A v2 server deployed modern-only (createMcpHandler legacy:'reject',
    MCP 2026-07-28) is reachable only by clients that speak the per-request envelope."""
    with server(peer, _name(request), extra=["--legacy", "reject"]) as run:
        try:
            client = _initialized(run)
            out = client.get_initialize_result().protocol_version
        except Exception as exc:  # noqa: BLE001
            out = f"{type(exc).__name__}: {exc}"
        first = run.http_exchanges()[0]["http"] if run.http_exchanges() else {}
        record_result(request.node.name, {"client": out, "status": first.get("status"),
                                          "body": (first.get("response_body") or "")[:400]})
        assert out == REVISION, f"{out}"
