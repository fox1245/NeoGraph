"""Official ACP Python SDK (agent-client-protocol) -> native NeoGraph ACPServer.

Every test spawns the deterministic native agent (``neograph_acp_probe_agent``,
built from tests/fixtures/acp_probe_agent.cpp) behind acp_stdio_tap.py, drives
it with the official ``acp`` ClientSideConnection and asserts what the ACP v1
specification requires. The SDK parses every agent message into its pydantic
models, so a test passes only when the official client accepted the wire
without raising, logging an error or warning. Every message the agent sent is
also validated against the vendored ACP schema (spec/schema.json, v1.25.0).

Runs on the pinned SDK lines only (requirements.txt: 0.12.1,
requirements-next.txt: 1.0.0rc2); it skips when the SDK, a pinned version or
the probe agent is missing.
"""

import asyncio
import contextlib
import json
import logging
import sys
import warnings
from pathlib import Path

import pytest

from acp_interop_support import (  # noqa: E402
    TAP,
    require_module,
    require_pinned_acp_sdk,
    require_probe_agent,
    transcript_dir,
)

SDK_VERSION = require_pinned_acp_sdk()
require_module("jsonschema")
AGENT = require_probe_agent()

from acp import PROTOCOL_VERSION, RequestError, connect_to_agent  # noqa: E402
from acp.schema import (  # noqa: E402
    ClientCapabilities,
    EnvVariable,
    FileSystemCapabilities,
    HttpMcpServer,
    ImageContentBlock,
    Implementation,
    McpServerStdio,
    ReadTextFileResponse,
    RequestPermissionResponse,
    ResourceContentBlock,
    TextContentBlock,
)

from acp_schema_check import agent_problems  # noqa: E402

HERE = Path(__file__).resolve().parent
MCP_SERVER = HERE / "acp_mcp_server.py"
SECRETS = ("hunter2", "/srv/secret")


def text_block(text):
    # acp 0.12 exports acp.text_block; 1.0 dropped it. Build the model directly.
    return TextContentBlock(type="text", text=text)


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------

class SdkComplaints(logging.Handler):
    """Collects every warning/error the SDK logs while a test runs."""

    def __init__(self):
        super().__init__(level=logging.WARNING)
        self.records = []

    def emit(self, record):
        self.records.append(f"{record.levelname} {record.name}: {record.getMessage()}"
                            + (f" | {record.exc_info[1]!r}" if record.exc_info else ""))


class RecordingClient:
    """An ACP client: records everything, answers agent->client requests."""

    def __init__(self, permission="allow", files=None):
        self.permission = permission          # "allow" | "reject" | "cancel-turn"
        self.files = dict(files or {})
        self.updates = []
        self.permission_requests = []
        self.reads = []
        self.writes = []
        self.terminal_calls = []
        self.ext_calls = []
        self.connection = None

    def on_connect(self, conn):
        self.connection = conn

    async def session_update(self, session_id, update, **kwargs):
        self.updates.append((session_id, update))

    async def request_permission(self, session_id, tool_call, options, **kwargs):
        self.permission_requests.append((session_id, tool_call, options))
        if self.permission == "cancel-turn":
            # ACP prompt-turn cancellation: the client sends session/cancel and
            # MUST answer the pending permission request with "cancelled".
            await self.connection.cancel(session_id=session_id)
            return RequestPermissionResponse.model_validate({"outcome": {"outcome": "cancelled"}})
        option = next(o for o in options if (o.kind.startswith("allow") if self.permission == "allow"
                                             else o.kind.startswith("reject")))
        return RequestPermissionResponse.model_validate(
            {"outcome": {"outcome": "selected", "optionId": option.option_id}})

    async def read_text_file(self, session_id, path, line=None, limit=None, **kwargs):
        self.reads.append((session_id, path, line, limit))
        if path not in self.files:
            raise RequestError.resource_not_found(path)
        return ReadTextFileResponse(content=self.files[path])

    async def write_text_file(self, session_id, path, content, **kwargs):
        self.writes.append((session_id, path, content))
        self.files[path] = content
        return None

    async def create_terminal(self, session_id, command, **kwargs):
        self.terminal_calls.append((session_id, command, kwargs))
        raise RequestError.method_not_found("terminal/create")

    async def ext_method(self, method, params):
        self.ext_calls.append((method, params))
        raise RequestError.method_not_found(method)

    async def ext_notification(self, method, params):
        self.ext_calls.append((method, params))

    def agent_text(self):
        out = []
        for _, update in self.updates:
            if getattr(update, "session_update", None) == "agent_message_chunk":
                out.append(getattr(update.content, "text", f"[{update.content.type}]"))
        return out


class Session:
    def __init__(self, name):
        self.name = name
        self.transcript = transcript_dir(f"python-{SDK_VERSION}") / f"{name}.jsonl"
        self.process = None
        self.conn = None
        self.complaints = SdkComplaints()

    def raw_wire(self):
        if not self.transcript.exists():
            return ""
        return self.transcript.read_text()

    def wire(self, direction="a2c", contains=None, limit=6):
        """The offending NeoGraph wire messages, for failure messages."""
        if not self.transcript.exists():
            return "<no transcript>"
        rows = [json.loads(line) for line in self.transcript.read_text().splitlines() if line.strip()]
        rows = [r["raw"] for r in rows if r["dir"] == direction and (contains is None or contains in r["raw"])]
        return "\n    " + "\n    ".join(rows[-limit:])


@contextlib.asynccontextmanager
async def agent_session(name, client, agent_args=(), capabilities=None, protocol_version=PROTOCOL_VERSION,
                        initialize=True):
    s = Session(name)
    if s.transcript.exists():
        s.transcript.unlink()
    root = logging.getLogger()
    root.addHandler(s.complaints)
    s.process = await asyncio.create_subprocess_exec(
        sys.executable, "-I", str(TAP), str(s.transcript), "--", str(AGENT), *agent_args,
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
        limit=16 * 1024 * 1024)
    s.conn = connect_to_agent(client, s.process.stdin, s.process.stdout)
    client.connection = s.conn
    completed = False
    try:
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            if initialize:
                s.init = await asyncio.wait_for(s.conn.initialize(
                    protocol_version=protocol_version,
                    client_capabilities=capabilities or ClientCapabilities(),
                    client_info=Implementation(name="acp-interop-probe", version="1.0"),
                ), 10)
            yield s
            for w in caught:
                s.complaints.records.append(f"WARNING(warnings) {w.category.__name__}: {w.message}")
        completed = True
    finally:
        root.removeHandler(s.complaints)
        with contextlib.suppress(Exception):
            await s.conn.close()
        if s.process.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                s.process.stdin.close()
            try:
                await asyncio.wait_for(s.process.wait(), 5)
            except asyncio.TimeoutError:
                s.process.kill()
                await s.process.wait()
        # Close the pipes now: transports left to the garbage collector raise
        # a ResourceWarning inside a later test.
        transport = getattr(s.process, "_transport", None)
        if transport is not None:
            transport.close()
    if completed:
        # Every message the agent put on the wire is valid against the
        # official schema and uses only error codes ACP names; none carries
        # exception text.
        problems = agent_problems(s.transcript)
        assert not problems, "agent messages fail the ACP schema:\n" + "\n".join(problems)
        wire = s.raw_wire()
        assert not any(secret in wire for secret in SECRETS), "exception text reached the wire"


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 60))


def assert_no_sdk_complaints(s):
    assert not s.complaints.records, (
        f"official SDK logged problems: {s.complaints.records}\n  NeoGraph wire:{s.wire()}")


async def new_session(s, cwd="/tmp", **kwargs):
    kwargs.setdefault("mcp_servers", [])
    return (await asyncio.wait_for(s.conn.new_session(cwd=cwd, **kwargs), 30)).session_id


async def prompt(s, sid, blocks, timeout=15):
    return await asyncio.wait_for(s.conn.prompt(session_id=sid, prompt=blocks), timeout)


# ---------------------------------------------------------------------------
# initialize / version negotiation
# ---------------------------------------------------------------------------

def test_initialize_v1_round_trip():
    async def body():
        async with agent_session("initialize_v1", RecordingClient()) as s:
            init = s.init
            assert init.protocol_version == 1, s.wire()
            assert init.agent_info is not None and init.agent_info.name, (
                "agentInfo SHOULD carry name+version" + s.wire())
            assert init.auth_methods == [], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_initialize_newer_client_version_gets_latest_supported():
    # The client announces v2 (the v2 schema is in alpha); a v1-only agent
    # MUST answer with the latest version it supports.
    async def body():
        async with agent_session("initialize_v2_client", RecordingClient(), protocol_version=2) as s:
            assert s.init.protocol_version == 1, s.wire()
            assert_no_sdk_complaints(s)
    run(body())


@pytest.mark.parametrize("info", ['{"name":"only-name"}', '"free-form string"'])
def test_agent_info_is_always_an_implementation(info):
    async def body():
        args = ("--agent-info", info)
        async with agent_session("agent_info", RecordingClient(), agent_args=args) as s:
            assert s.init.agent_info is not None, s.wire()
            assert s.init.agent_info.name and s.init.agent_info.version, s.wire()
            assert_no_sdk_complaints(s)
    run(body())


# ---------------------------------------------------------------------------
# session/new + session/prompt + session/update
# ---------------------------------------------------------------------------

def test_prompt_streams_agent_message_chunk_and_end_turn():
    async def body():
        client = RecordingClient()
        async with agent_session("prompt_echo", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("echo hello acp")])
            assert resp.stop_reason == "end_turn", s.wire()
            assert client.agent_text() == ["[probe] hello acp"], s.wire()
            assert all(update_sid == sid for update_sid, _ in client.updates), s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_relative_cwd_rejected_with_invalid_params():
    async def body():
        async with agent_session("session_new_relative_cwd", RecordingClient()) as s:
            with pytest.raises(RequestError) as err:
                await s.conn.new_session(cwd="relative/dir", mcp_servers=[])
            assert err.value.code == -32602, s.wire()
    run(body())


def test_session_cwd_and_additional_directories_reach_the_graph():
    # session-setup: the cwd is the session's working directory, the base
    # for relative paths.
    async def body():
        client = RecordingClient()
        async with agent_session("session_cwd", client) as s:
            sid = await new_session(s, cwd="/tmp", additional_directories=["/var/tmp"])
            resp = await prompt(s, sid, [text_block("pwd")])
            assert resp.stop_reason == "end_turn", s.wire()
            assert client.agent_text() == ["cwd:/tmp|dirs:/var/tmp"], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_unknown_session_prompt_is_resource_not_found():
    async def body():
        async with agent_session("prompt_unknown_session", RecordingClient()) as s:
            with pytest.raises(RequestError) as err:
                await prompt(s, "no-such-session", [text_block("echo x")])
            assert err.value.code == -32002, f"got {err.value.code}: {err.value}" + s.wire()
    run(body())


def test_resource_link_is_baseline_prompt_content():
    # initialization: "all Agents MUST support ContentBlock::Text and
    # ContentBlock::ResourceLink in session/prompt requests."
    async def body():
        client = RecordingClient()
        async with agent_session("prompt_resource_link", client) as s:
            sid = await new_session(s)
            link = ResourceContentBlock(type="resource_link", uri="file:///tmp/spec.md", name="spec.md")
            resp = await prompt(s, sid, [text_block("summarise"), link])
            assert resp.stop_reason == "end_turn", s.wire()
            text = " ".join(client.agent_text())
            assert "[spec.md](file:///tmp/spec.md)" in text, (
                f"the resource_link never reached the graph; agent replied {text!r}" + s.wire())
    run(body())


def test_unadvertised_image_prompt_is_rejected():
    # Clients MUST NOT send image unless promptCapabilities.image; an agent
    # that receives one anyway says so rather than silently dropping it.
    async def body():
        client = RecordingClient()
        async with agent_session("prompt_unadvertised_image", client) as s:
            assert not s.init.agent_capabilities.prompt_capabilities.image
            sid = await new_session(s)
            image = ImageContentBlock(type="image", data="aGVsbG8=", mime_type="image/png")
            with pytest.raises(RequestError) as err:
                await prompt(s, sid, [text_block("look"), image])
            assert err.value.code == -32602, s.wire()
    run(body())


# ---------------------------------------------------------------------------
# cancellation
# ---------------------------------------------------------------------------

def test_session_cancel_resolves_prompt_with_cancelled():
    async def body():
        client = RecordingClient()
        async with agent_session("cancel_inflight", client) as s:
            sid = await new_session(s)
            task = asyncio.create_task(prompt(s, sid, [text_block("slow")]))
            await asyncio.sleep(0.5)
            await s.conn.cancel(session_id=sid)
            resp = await asyncio.wait_for(task, 10)
            assert resp.stop_reason == "cancelled", s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_cancel_without_active_turn_does_not_cancel_next_prompt():
    # session/cancel cancels "an ongoing prompt turn"; a stray cancel sent
    # while no turn runs must not cancel a prompt the client sends later.
    async def body():
        client = RecordingClient()
        async with agent_session("cancel_before_prompt", client) as s:
            sid = await new_session(s)
            await s.conn.cancel(session_id=sid)
            await asyncio.sleep(0.2)
            resp = await prompt(s, sid, [text_block("echo after stray cancel")])
            assert resp.stop_reason == "end_turn", (
                f"a prompt sent after a stray session/cancel came back {resp.stop_reason!r}" + s.wire())
    run(body())


def test_cancel_while_permission_pending():
    async def body():
        client = RecordingClient(permission="cancel-turn")
        async with agent_session("cancel_during_permission", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("permission")])
            assert len(client.permission_requests) == 1, s.wire()
            assert resp.stop_reason == "cancelled", s.wire()
            assert_no_sdk_complaints(s)
    run(body())


# ---------------------------------------------------------------------------
# agent -> client requests
# ---------------------------------------------------------------------------

def test_request_permission_selected():
    async def body():
        client = RecordingClient(permission="allow")
        async with agent_session("permission_selected", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("permission")])
            assert resp.stop_reason == "end_turn", s.wire()
            assert len(client.permission_requests) == 1, s.wire()
            req_sid, tool_call, options = client.permission_requests[0]
            assert req_sid == sid and tool_call.tool_call_id == "call_probe_1", s.wire()
            assert {o.kind for o in options} == {"allow_once", "reject_once"}, s.wire()
            assert client.agent_text() == ["permission:selected:allow"], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_fs_round_trip_when_client_advertises_fs():
    async def body():
        caps = ClientCapabilities(fs=FileSystemCapabilities(read_text_file=True, write_text_file=True))
        client = RecordingClient(files={"/tmp/in.txt": "hello file"})
        async with agent_session("fs_round_trip", client, capabilities=caps) as s:
            sid = await new_session(s)
            r1 = await prompt(s, sid, [text_block("fsread /tmp/in.txt")])
            r2 = await prompt(s, sid, [text_block("fswrite /tmp/out.txt")])
            assert (r1.stop_reason, r2.stop_reason) == ("end_turn", "end_turn"), s.wire()
            assert client.reads == [(sid, "/tmp/in.txt", None, None)], s.wire()
            assert client.writes == [(sid, "/tmp/out.txt", "probe-content")], s.wire()
            assert client.agent_text() == ["read:hello file", "written"], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


@pytest.mark.parametrize("verb,recorded", [("fsread /tmp/in.txt", "reads"),
                                           ("terminal", "terminal_calls")])
def test_capability_gated_methods_are_not_called_without_the_capability(verb, recorded):
    # file-system / terminals: without the client capability the agent MUST
    # NOT call fs/* or terminal/*. The node's call fails, so the turn fails.
    async def body():
        client = RecordingClient(files={"/tmp/in.txt": "x"})
        async with agent_session("gated_" + recorded, client) as s:
            sid = await new_session(s)
            with pytest.raises(RequestError) as err:
                await prompt(s, sid, [text_block(verb)])
            assert err.value.code == -32603, s.wire()
            assert getattr(client, recorded) == [], (
                f"agent sent {verb.split()[0]} without the capability" + s.wire())
    run(body())


# ---------------------------------------------------------------------------
# failure classes -> stop reasons / errors
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("verb,code,retryable", [
    ("fail", "NG.TaskFailed", True),
    ("coded", "app.tool_error", False),
    ("logic", "NG.LogicError", False),
    ("hook", "NG.HookBlocked", False),
    ("budget", "NG.BudgetExhausted", False),
    ("provider", "NG.ProviderFailed", True),
])
def test_failed_turn_is_a_json_rpc_error_without_exception_text(verb, code, retryable):
    # StopReason "end_turn" is defined as "The turn ended successfully"; a turn
    # whose graph failed is a JSON-RPC error carrying only typed public fields.
    async def body():
        client = RecordingClient()
        async with agent_session(f"failure_{verb}", client) as s:
            sid = await new_session(s)
            with pytest.raises(RequestError) as err:
                await prompt(s, sid, [text_block(verb)])
            exc = err.value
            assert exc.code == -32603, s.wire()
            assert exc.data == {"code": code, "retryable": retryable}, s.wire()
            if verb == "coded":
                # The author's public message is the only free text allowed.
                assert "the probe tool failed" in str(exc), s.wire()
            assert client.agent_text() == [], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_provider_refusal_is_the_refusal_stop_reason():
    async def body():
        client = RecordingClient()
        async with agent_session("failure_refusal", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("refusal")])
            assert resp.stop_reason == "refusal", s.wire()
            assert client.agent_text() == [], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_step_limit_maps_to_max_turn_requests():
    async def body():
        client = RecordingClient()
        async with agent_session("step_limit", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("loop")])
            assert resp.stop_reason == "max_turn_requests", s.wire()
            assert client.agent_text() == [], "an empty agent_message_chunk was sent" + s.wire()
    run(body())


def test_model_token_budget_maps_to_max_tokens():
    async def body():
        client = RecordingClient()
        async with agent_session("token_budget", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("token-budget")])
            assert resp.stop_reason == "max_tokens", s.wire()
    run(body())


@pytest.mark.parametrize("answer,expected", [("allow", "resumed:approve"),
                                             ("reject", "resumed:reject")])
def test_interrupt_asks_permission_and_resumes_in_the_same_turn(answer, expected):
    # D3: a NodeInterrupt becomes session/request_permission in the same turn;
    # the selected option resumes the run.
    async def body():
        client = RecordingClient(permission=answer)
        async with agent_session(f"interrupt_{answer}", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("interrupt")])
            assert resp.stop_reason == "end_turn", s.wire()
            assert len(client.permission_requests) == 1, s.wire()
            req_sid, tool_call, options = client.permission_requests[0]
            assert req_sid == sid and tool_call.title == "approval needed", s.wire()
            assert tool_call.raw_input == {"tool": "probe"}, s.wire()
            meta = (tool_call.field_meta or {}).get("neograph/interrupt")
            assert meta and meta.get("node") == "probe", s.wire()
            assert {o.kind for o in options} == {"allow_once", "reject_once"}, s.wire()
            assert client.agent_text() == [expected], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_interrupt_cancelled_by_the_client_ends_the_turn_cancelled():
    async def body():
        client = RecordingClient(permission="cancel-turn")
        async with agent_session("interrupt_cancelled", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("interrupt")])
            assert resp.stop_reason == "cancelled", s.wire()
            assert client.agent_text() == [], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


# ---------------------------------------------------------------------------
# incremental updates: model tokens, tool calls
# ---------------------------------------------------------------------------

def test_model_tokens_stream_as_agent_message_chunks():
    async def body():
        client = RecordingClient()
        async with agent_session("stream_tokens", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("stream")])
            assert resp.stop_reason == "end_turn", s.wire()
            assert client.agent_text() == ["str", "eam"], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_tool_calls_are_reported_with_their_status():
    # tool-calls: agents SHOULD report tool calls; a failed call's error text
    # stays local.
    async def body():
        client = RecordingClient()
        async with agent_session("tool_calls", client) as s:
            sid = await new_session(s)
            resp = await prompt(s, sid, [text_block("toolcall")])
            assert resp.stop_reason == "end_turn", s.wire()
            kinds = [(u.session_update, getattr(u, "tool_call_id", None), getattr(u, "status", None))
                     for _, u in client.updates if u.session_update != "agent_message_chunk"]
            assert kinds == [
                ("tool_call", "call_probe_ok", "in_progress"),
                ("tool_call", "call_probe_fail", "in_progress"),
                ("tool_call_update", "call_probe_ok", "completed"),
                ("tool_call_update", "call_probe_fail", "failed"),
            ], s.wire()
            failed = [u for _, u in client.updates
                      if getattr(u, "tool_call_id", None) == "call_probe_fail"][-1]
            assert not failed.content, s.wire()
            assert client.agent_text() == ["tools done"], s.wire()
            assert_no_sdk_complaints(s)
    run(body())


# ---------------------------------------------------------------------------
# client-supplied MCP servers (session-setup: stdio support is a MUST)
# ---------------------------------------------------------------------------

def mcp_stdio_server(**env):
    return McpServerStdio(
        name="interop-mcp",
        command=sys.executable,
        args=["-I", str(MCP_SERVER)],
        env=[EnvVariable(name=k, value=v) for k, v in env.items()],
    )


def test_client_stdio_mcp_server_tools_reach_the_graph(monkeypatch):
    require_module("mcp")
    # The agent's environment may hold provider keys; the MCP server the
    # client names gets PATH and the client's env only.
    monkeypatch.setenv("ACP_INTEROP_AGENT_SECRET", "sk-interop-fake-0000")

    async def body():
        client = RecordingClient()
        async with agent_session("mcp_stdio", client) as s:
            sid = await new_session(s, mcp_servers=[mcp_stdio_server(ACP_INTEROP_ENV="from-client")])
            r1 = await prompt(s, sid, [text_block("mcp-list")])
            r2 = await prompt(s, sid, [text_block('mcp-call add {"a": 2, "b": 3}')])
            r3 = await prompt(s, sid, [text_block("mcp-call env_probe {}")])
            r4 = await prompt(s, sid, [text_block('mcp-call env_probe {"name": "ACP_INTEROP_AGENT_SECRET"}')])
            assert (r1.stop_reason, r2.stop_reason, r3.stop_reason, r4.stop_reason) == ("end_turn",) * 4, s.wire()
            listed, added, env, secret = client.agent_text()
            assert set(listed.removeprefix("mcp-tools:").split(",")) == {"add", "env_probe"}, s.wire()
            assert added.startswith("mcp-result:") and "5" in added, s.wire()
            assert "from-client" in env, s.wire()
            assert "<unset>" in secret and "sk-interop" not in secret, s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_unadvertised_http_mcp_server_is_invalid_params():
    async def body():
        async with agent_session("mcp_http_unadvertised", RecordingClient()) as s:
            assert not s.init.agent_capabilities.mcp_capabilities.http, s.wire()
            server = HttpMcpServer(type="http", name="remote", url="http://127.0.0.1:9/mcp", headers=[])
            with pytest.raises(RequestError) as err:
                await s.conn.new_session(cwd="/tmp", mcp_servers=[server])
            assert err.value.code == -32602, s.wire()
    run(body())


def test_mcp_server_that_cannot_start_fails_session_new_without_detail():
    async def body():
        async with agent_session("mcp_unavailable", RecordingClient()) as s:
            server = McpServerStdio(name="missing", command="/nonexistent/acp-interop-mcp",
                                    args=[], env=[])
            with pytest.raises(RequestError) as err:
                await s.conn.new_session(cwd="/tmp", mcp_servers=[server])
            assert err.value.code == -32603, s.wire()
            # The failure detail (path, stderr, transport error) stays local.
            assert "/nonexistent" not in s.wire(limit=1000), s.wire()
    run(body())


# ---------------------------------------------------------------------------
# session lifecycle methods
# ---------------------------------------------------------------------------

def test_session_resume_known_session_returns_resume_response():
    async def body():
        client = RecordingClient()
        async with agent_session("resume_known", client) as s:
            sid = await new_session(s)
            await prompt(s, sid, [text_block("echo once")])
            resp = await asyncio.wait_for(s.conn.resume_session(session_id=sid, cwd="/tmp", mcp_servers=[]), 10)
            assert resp is not None, s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_session_resume_of_a_live_session_without_a_checkpoint():
    async def body():
        async with agent_session("resume_live", RecordingClient()) as s:
            sid = await new_session(s)
            resp = await asyncio.wait_for(s.conn.resume_session(session_id=sid, cwd="/tmp", mcp_servers=[]), 10)
            assert resp is not None, s.wire()
            assert_no_sdk_complaints(s)
    run(body())


def test_session_resume_unknown_session_uses_acp_error_code():
    async def body():
        async with agent_session("resume_unknown", RecordingClient()) as s:
            with pytest.raises(RequestError) as err:
                await s.conn.resume_session(session_id="sess-missing", cwd="/tmp", mcp_servers=[])
            assert err.value.code == -32002, (
                f"unknown session answered with code {err.value.code} (ACP ErrorCode: -32002 Resource not found)"
                + s.wire(contains='"error"'))
    run(body())


def test_unsupported_methods_answer_method_not_found():
    async def body():
        async with agent_session("unsupported_methods", RecordingClient()) as s:
            sid = await new_session(s)
            calls = {
                "session/load": s.conn.load_session(cwd="/tmp", session_id=sid, mcp_servers=[]),
                "authenticate": s.conn.authenticate(method_id="none"),
                "session/set_mode": s.conn.set_session_mode(session_id=sid, mode_id="x"),
                "session/list": s.conn.list_sessions(),
                "session/close": s.conn.close_session(session_id=sid),
                "_neograph/probe": s.conn.ext_method("_neograph/probe", {}),
            }
            codes = {}
            for method, call in calls.items():
                try:
                    await asyncio.wait_for(call, 10)
                    codes[method] = "result"
                except RequestError as exc:
                    codes[method] = exc.code
            assert all(code == -32601 for code in codes.values()), f"{codes}" + s.wire()
            await s.conn.ext_notification("_neograph/notice", {})  # must be ignored
            resp = await prompt(s, sid, [text_block("echo still alive")])
            assert resp.stop_reason == "end_turn"
    run(body())


def test_second_prompt_on_busy_session_does_not_claim_auth_required():
    # ErrorCode -32000 is "Authentication required"; ACP clients start an
    # auth flow on it. A busy session is a different condition.
    async def body():
        client = RecordingClient()
        async with agent_session("busy_session", client) as s:
            sid = await new_session(s)
            first = asyncio.create_task(prompt(s, sid, [text_block("slow")]))
            await asyncio.sleep(0.3)
            with pytest.raises(RequestError) as err:
                await prompt(s, sid, [text_block("echo second")])
            await s.conn.cancel(session_id=sid)
            await asyncio.wait_for(first, 10)
            assert err.value.code == -32603, s.wire(contains='"error"')
            assert (err.value.data or {}).get("reason") == "session_busy", s.wire(contains='"error"')
    run(body())
