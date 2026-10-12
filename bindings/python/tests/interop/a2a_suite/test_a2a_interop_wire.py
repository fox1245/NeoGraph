"""Raw-wire conformance of NeoGraph's native A2AServer against the machine-readable spec.

Every response NeoGraph emits is validated against the 1.0 ProtoJSON descriptor +
JSON Schema (A2A-Version: 1.0) or the 0.3 JSON Schema (no A2A-Version header,
spec 3.6.2). Behavioural MUSTs come from spec sections cited in each test.
"""

from __future__ import annotations

import json
import time

import pytest

from a2a_harness import Transcript, native_server, rpc, rpc_stream, v03_text_message, v1_text_message

import httpx  # noqa: E402  (after the harness's skip gate)
from a2a_oracle import (  # noqa: E402
    A2A_ERROR_CODES,
    JSONRPC_CODES,
    V03_RESPONSE_DEF,
    v1_method_set,
    v1_result_type,
    validate_v03,
    validate_v1,
)

T = Transcript("wire_native_server")


@pytest.fixture(scope="module")
def srv():
    with native_server() as base:
        yield base


def _record(test, ok, **kw):
    T.add(test=test, result="PASS" if ok else "FAIL", **kw)
    print(("PASS " if ok else "FAIL ") + test + ("" if ok else f" :: {kw.get('why', '')}"))


def _expect(test, ok, why, wire):
    _record(test, ok, why=why, wire=wire)
    if not ok:
        pytest.fail(f"{why}\nNeoGraph wire: {json.dumps(wire)[:1500]}")


def _v1_ok(test, type_name, obj, wire=None):
    problems = validate_v1(type_name, obj)
    _expect(test, not problems, f"{type_name} invalid: {problems}", wire if wire is not None else obj)


def _v03_ok(test, def_name, obj, wire=None):
    problems = validate_v03(def_name, obj)
    _expect(test, not problems, f"0.3 {def_name} invalid: {problems}", wire if wire is not None else obj)


def _error_code(env):
    return (env or {}).get("error", {}).get("code") if isinstance(env, dict) else None


# --------------------------------------------------------------------------- card

def test_card_served_at_well_known_path(srv):
    r = httpx.get(srv + "/.well-known/agent-card.json")
    _expect("card.well_known", r.status_code == 200 and r.headers.get("content-type", "").startswith(
        "application/json"), f"status={r.status_code} ct={r.headers.get('content-type')}", r.text)


def test_card_validates_as_v1_agent_card(srv):
    # One card per version (as @a2a-js legacyCompat does): A2A-Version 1.0
    # receives the pure 1.0 card.
    card = httpx.get(srv + "/.well-known/agent-card.json", headers={"A2A-Version": "1.0"}).json()
    _v1_ok("card.v1_schema", "AgentCard", card)


def test_card_validates_as_v03_agent_card(srv):
    # No A2A-Version is 0.3 (spec 3.6.2): the 0.3 card, which also lists the
    # 1.0 interface in supportedInterfaces.
    card = httpx.get(srv + "/.well-known/agent-card.json").json()
    _v03_ok("card.v03_schema", "AgentCard", card)
    versions = [i["protocolVersion"] for i in card.get("supportedInterfaces", [])]
    _expect("card.v03_lists_v1_interface", versions[:1] == ["1.0"], f"interfaces={versions}", card)


def test_card_caching_headers_should(srv):
    r = httpx.get(srv + "/.well-known/agent-card.json")
    ok = "cache-control" in r.headers and "etag" in r.headers
    _expect("card.caching_headers(SHOULD 8.6.1)", ok, f"headers={dict(r.headers)}", dict(r.headers))


# --------------------------------------------------------------------------- 1.0 unary

def test_v1_send_message_response_shape(srv):
    st, _, env, body = rpc(srv, "SendMessage", {"message": v1_text_message("artifact one")}, version="1.0")
    assert st == 200, body
    _v1_ok("v1.SendMessage.result", "SendMessageResponse", env["result"], env)


def test_v1_send_message_rejects_client_task_id_for_new_task(srv):
    # spec 3.4.2: a taskId in a Message MUST reference an existing task, else TaskNotFoundError.
    msg = v1_text_message("echo x", taskId="client-chosen-task-id-1")
    st, _, env, body = rpc(srv, "SendMessage", {"message": msg}, version="1.0")
    _expect("v1.SendMessage.unknown_taskId->TaskNotFound(3.4.2 MUST)",
            _error_code(env) == A2A_ERROR_CODES["TaskNotFoundError"],
            f"expected error -32001, got {env}", env)


def test_v1_send_message_to_terminal_task_is_unsupported(srv):
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("echo first")}, version="1.0")
    task = env["result"]["task"]
    follow = v1_text_message("echo again", taskId=task["id"], contextId=task["contextId"])
    _, _, env2, _ = rpc(srv, "SendMessage", {"message": follow}, version="1.0")
    _expect("v1.SendMessage.terminal_task->UnsupportedOperation(3.1.1 MUST)",
            _error_code(env2) == A2A_ERROR_CODES["UnsupportedOperationError"],
            f"expected error -32004, got {env2}", env2)


def test_v1_send_message_rejects_mismatched_context(srv):
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("interrupt please")},
                       version="1.0")
    task = env["result"]["task"]
    follow = v1_text_message("echo x", taskId=task["id"], contextId="some-other-context")
    _, _, env2, _ = rpc(srv, "SendMessage", {"message": follow}, version="1.0")
    _expect("v1.SendMessage.context_mismatch->error(3.4.3 MUST)", "error" in (env2 or {}),
            f"expected an error, got {env2}", env2)


def test_v1_send_message_return_immediately(srv):
    # spec 3.2.2: returnImmediately=true MUST return right after creating the task.
    t0 = time.monotonic()
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("slow"),
                                            "configuration": {"returnImmediately": True}},
                       version="1.0")
    dt = time.monotonic() - t0
    state = env.get("result", {}).get("task", {}).get("status", {}).get("state")
    _expect("v1.SendMessage.returnImmediately(3.2.2 MUST)",
            dt < 2.0 and state not in ("TASK_STATE_COMPLETED",),
            f"returned after {dt:.2f}s in state {state}", env)


@pytest.mark.parametrize("params,label", [
    ({}, "missing message"),
    ({"message": {"role": "ROLE_USER", "parts": [{"text": "x"}]}}, "missing messageId"),
    ({"message": {"messageId": "m", "role": "ROLE_USER", "parts": []}}, "empty parts"),
    ({"message": {"messageId": "m", "parts": [{"text": "x"}]}}, "missing role"),
    ({"message": "not-an-object"}, "message is a string"),
])
def test_v1_send_message_invalid_params(srv, params, label):
    # spec 3.3.2: servers MUST validate input; validation -> -32602 (9.5).
    _, _, env, _ = rpc(srv, "SendMessage", params, version="1.0")
    _expect(f"v1.SendMessage.invalid_params[{label}]->-32602",
            _error_code(env) == JSONRPC_CODES["InvalidParams"], f"got {env}", env)


def test_v1_history_length_zero_omits_history(srv):
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("echo h"),
                                            "configuration": {"historyLength": 0}}, version="1.0")
    task = env["result"]["task"]
    _expect("v1.SendMessage.historyLength=0(3.2.4 SHOULD)", "history" not in task,
            "history present despite historyLength=0", env)


# --------------------------------------------------------------------------- 1.0 streaming

@pytest.mark.parametrize("prompt,terminal", [
    ("artifact s", "TASK_STATE_COMPLETED"),
    ("fail now", "TASK_STATE_FAILED"),
])
def test_v1_stream_events_are_valid_stream_responses(srv, prompt, terminal):
    st, hdrs, events, text = rpc_stream(srv, "SendStreamingMessage",
                                        {"message": v1_text_message(prompt)}, version="1.0")
    assert st == 200 and hdrs.get("content-type", "").startswith("text/event-stream"), text
    for i, ev in enumerate(events):
        assert ev.get("jsonrpc") == "2.0" and ev.get("id") == 1, ev
        _v1_ok(f"v1.stream[{prompt}].event{i}", "StreamResponse", ev.get("result"), ev)
    first, last = events[0]["result"], events[-1]["result"]
    _expect(f"v1.stream[{prompt}].starts_with_task(3.1.2 MUST)", "task" in first,
            "first event is not a Task", events[0])
    _expect(f"v1.stream[{prompt}].ends_terminal", last.get("statusUpdate", {}).get("status", {})
            .get("state") == terminal, f"last event state != {terminal}", events[-1])


def test_v1_stream_without_streaming_capability(srv):
    # spec 3.3.4: streaming=false -> SendStreamingMessage MUST return UnsupportedOperationError.
    with native_server("--no-streaming") as base:
        st, hdrs, events, text = rpc_stream(base, "SendStreamingMessage",
                                            {"message": v1_text_message("echo")}, version="1.0")
        code = None
        if events and "error" in events[0]:
            code = events[0]["error"].get("code")
        else:
            try:
                code = json.loads(text).get("error", {}).get("code")
            except ValueError:
                pass
        _expect("v1.stream.capability_false->UnsupportedOperation(3.3.4 MUST)",
                code == A2A_ERROR_CODES["UnsupportedOperationError"],
                f"got status={st} code={code}", events or text)


# --------------------------------------------------------------------------- 1.0 get/cancel

def test_v1_get_task_shape_and_not_found(srv):
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("echo g")}, version="1.0")
    tid = env["result"]["task"]["id"]
    _, _, got, _ = rpc(srv, "GetTask", {"id": tid}, version="1.0")
    _v1_ok("v1.GetTask.result", "Task", got.get("result"), got)
    _, _, nf, _ = rpc(srv, "GetTask", {"id": "does-not-exist"}, version="1.0")
    _expect("v1.GetTask.not_found->-32001", _error_code(nf) == -32001, f"got {nf}", nf)
    _, _, bad, _ = rpc(srv, "GetTask", {}, version="1.0")
    _expect("v1.GetTask.missing_id->-32602", _error_code(bad) == -32602, f"got {bad}", bad)


def test_v1_cancel_running_task(srv):
    # Learn the server-generated id from the stream's first frame, then cancel.
    with httpx.stream("POST", srv + "/", headers={"A2A-Version": "1.0", "Accept": "text/event-stream",
                                                  "Content-Type": "application/json"},
                      content=json.dumps({"jsonrpc": "2.0", "id": 9, "method": "SendStreamingMessage",
                                          "params": {"message": v1_text_message("slow c")}}),
                      timeout=30) as r:
        lines = r.iter_lines()
        first = None
        for ln in lines:
            if ln.startswith("data:"):
                first = json.loads(ln[5:])
                break
        tid = first["result"]["task"]["id"]
        _, _, cancel, _ = rpc(srv, "CancelTask", {"id": tid}, version="1.0")
        rest = [json.loads(ln[5:]) for ln in lines if ln.startswith("data:")]
    _v1_ok("v1.CancelTask.result", "Task", cancel.get("result"), cancel)
    _expect("v1.CancelTask.state", cancel["result"]["status"]["state"] == "TASK_STATE_CANCELED",
            "cancel did not report canceled", cancel)
    last = rest[-1]["result"] if rest else {}
    _expect("v1.CancelTask.stream_closes_canceled",
            last.get("statusUpdate", {}).get("status", {}).get("state") == "TASK_STATE_CANCELED",
            "stream did not end canceled", rest)


def test_v1_cancel_terminal_task_not_cancelable(srv):
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("echo done")}, version="1.0")
    tid = env["result"]["task"]["id"]
    _, _, c, _ = rpc(srv, "CancelTask", {"id": tid}, version="1.0")
    _expect("v1.CancelTask.terminal->TaskNotCancelable(3.1.5)",
            _error_code(c) == A2A_ERROR_CODES["TaskNotCancelableError"], f"got {c}", c)
    _, _, after, _ = rpc(srv, "GetTask", {"id": tid}, version="1.0")
    _expect("v1.CancelTask.terminal_state_unchanged",
            after["result"]["status"]["state"] == "TASK_STATE_COMPLETED",
            "completed task was relabelled", after)


# --------------------------------------------------------------------------- method set

CAPABILITY_ERRORS = {
    # spec 3.3.4 (capability validation) for a card with streaming=true,
    # pushNotifications=false, extendedAgentCard absent.
    "CreateTaskPushNotificationConfig": A2A_ERROR_CODES["PushNotificationNotSupportedError"],
    "GetTaskPushNotificationConfig": A2A_ERROR_CODES["PushNotificationNotSupportedError"],
    "ListTaskPushNotificationConfigs": A2A_ERROR_CODES["PushNotificationNotSupportedError"],
    "DeleteTaskPushNotificationConfig": A2A_ERROR_CODES["PushNotificationNotSupportedError"],
    "GetExtendedAgentCard": A2A_ERROR_CODES["UnsupportedOperationError"],
}


@pytest.mark.parametrize("method", [m for m in v1_method_set()
                                    if m not in ("SendMessage", "SendStreamingMessage",
                                                 "GetTask", "CancelTask")])
def test_v1_every_proto_method_is_dispatched(srv, method):
    params = {"id": "nope", "taskId": "nope"} if method != "ListTasks" else {}
    if method == "SubscribeToTask":
        _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("echo s")}, version="1.0")
        params = {"id": env["result"]["task"]["id"]}
    _, _, got, _ = rpc(srv, method, params, version="1.0")
    code = _error_code(got)
    if method in CAPABILITY_ERRORS:
        ok, why = code == CAPABILITY_ERRORS[method], f"expected {CAPABILITY_ERRORS[method]}"
    elif method == "ListTasks":
        ok, why = code is None and "result" in got, "ListTasks is a core operation (3.1.4)"
        if ok:
            ok = not validate_v1("ListTasksResponse", got["result"])
    else:  # SubscribeToTask on a terminal task -> UnsupportedOperationError (9.4.6)
        ok, why = code == A2A_ERROR_CODES["UnsupportedOperationError"], "expected -32004"
    _expect(f"v1.method[{method}]", ok, f"{why}; got {got}", got)


# --------------------------------------------------------------------------- versioning / JSON-RPC

def test_version_not_supported(srv):
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("echo")}, version="9.9")
    _expect("version.unsupported->-32009(3.6.2 MUST)", _error_code(env) == -32009, f"got {env}", env)


def test_version_patch_is_ignored(srv):
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("echo")}, version="1.0.1")
    _expect("version.patch_ignored(3.6)", "task" in env.get("result", {}), f"got {env}", env)


def test_version_query_parameter(srv):
    # spec 3.6.1: clients MAY send A2A-Version as a request parameter.
    r = httpx.post(srv + "/?A2A-Version=1.0", json={"jsonrpc": "2.0", "id": 1, "method": "SendMessage",
                                                     "params": {"message": v1_text_message("echo")}})
    env = r.json()
    _expect("version.query_parameter(3.6.1 MAY)", "task" in env.get("result", {}), f"got {env}", env)


def test_jsonrpc_parse_error(srv):
    st, _, env, body = rpc(srv, "", raw="{not json")
    ok = _error_code(env) == -32700 and "id" in env and env["id"] is None
    _expect("jsonrpc.parse_error", ok, f"got {body}", body)


@pytest.mark.parametrize("raw,label", [
    ('{"jsonrpc":"1.0","id":1,"method":"GetTask","params":{"id":"x"}}', "wrong jsonrpc version"),
    ('{"id":1,"method":"GetTask","params":{"id":"x"}}', "missing jsonrpc"),
    ('{"jsonrpc":"2.0","id":1,"method":5}', "non-string method"),
    ('{"jsonrpc":"2.0","id":1}', "missing method"),
    ('{"jsonrpc":"2.0","id":{"a":1},"method":"GetTask","params":{"id":"x"}}', "object id"),
])
def test_jsonrpc_invalid_request(srv, raw, label):
    _, _, env, body = rpc(srv, "", raw=raw, version="1.0")
    _expect(f"jsonrpc.invalid_request[{label}]->-32600", _error_code(env) == -32600, f"got {body}", body)


def test_jsonrpc_batch(srv):
    raw = json.dumps([{"jsonrpc": "2.0", "id": 1, "method": "GetTask", "params": {"id": "x"}}])
    st, _, env, body = rpc(srv, "", raw=raw, version="1.0")
    ok = st == 200 and (isinstance(env, list) or _error_code(env) == -32600)
    _expect("jsonrpc.batch->array or -32600", ok, f"status={st} body={body!r}", body)


def test_jsonrpc_string_id_echo(srv):
    _, _, env, _ = rpc(srv, "GetTask", {"id": "nope"}, version="1.0", rid="abc-1")
    _expect("jsonrpc.string_id_echo", env.get("id") == "abc-1", f"got {env}", env)


def test_jsonrpc_unknown_method(srv):
    _, _, env, _ = rpc(srv, "NoSuchMethod", {}, version="1.0")
    _expect("jsonrpc.method_not_found", _error_code(env) == -32601, f"got {env}", env)


# --------------------------------------------------------------------------- 0.3 dialect

def test_v03_send_message(srv):
    _, _, env, _ = rpc(srv, "message/send", {"message": v03_text_message("artifact a")})
    _v03_ok("v03.message/send", V03_RESPONSE_DEF["message/send"], env)


def test_v03_stream(srv):
    st, _, events, text = rpc_stream(srv, "message/stream", {"message": v03_text_message("artifact b")})
    for i, ev in enumerate(events):
        _v03_ok(f"v03.message/stream.event{i}", V03_RESPONSE_DEF["message/stream"], ev)
    finals = [i for i, ev in enumerate(events) if ev.get("result", {}).get("final") is True]
    _expect("v03.stream.nothing_after_final", finals and finals[0] == len(events) - 1,
            f"final=true at {finals} of {len(events)} events", events)
    kinds = [ev.get("result", {}).get("kind") for ev in events]
    _expect("v03.stream.artifacts_emitted", "artifact-update" in kinds or any(
        ev.get("result", {}).get("artifacts") for ev in events), f"kinds={kinds}", events)


def test_v03_get_and_cancel(srv):
    _, _, env, _ = rpc(srv, "message/send", {"message": v03_text_message("echo c")})
    tid = env["result"]["id"]
    _, _, g, _ = rpc(srv, "tasks/get", {"id": tid})
    _v03_ok("v03.tasks/get", "GetTaskResponse", g)
    _, _, c, _ = rpc(srv, "tasks/cancel", {"id": tid})
    _expect("v03.tasks/cancel.terminal->-32002", _error_code(c) == -32002, f"got {c}", c)
    _v03_ok("v03.tasks/cancel.envelope", "CancelTaskResponse", c)


@pytest.mark.parametrize("method,code", [
    # tasks/resubscribe is implemented: an unknown task is TaskNotFound, sent
    # as an SSE event because 0.3 clients read resubscribe only as a stream
    # (as the @a2a-js server does).
    ("tasks/resubscribe", -32001),
    ("tasks/pushNotificationConfig/set", -32003),
    ("tasks/pushNotificationConfig/get", -32003),
    ("tasks/pushNotificationConfig/list", -32003),
    ("tasks/pushNotificationConfig/delete", -32003),
    ("agent/getAuthenticatedExtendedCard", -32004),
])
def test_v03_method_set(srv, method, code):
    params = {"id": "x", "taskId": "x", "pushNotificationConfigId": "x"}
    if method == "tasks/resubscribe":
        _, _, events, text = rpc_stream(srv, method, params)
        env = events[0] if events else None
    else:
        _, _, env, _ = rpc(srv, method, params)
    _expect(f"v03.method[{method}]", _error_code(env) == code, f"expected {code}, got {env}", env)


# --------------------------------------------------------------------------- failure classes

# Failure mapping: hook block -> REJECTED before any node finished (FAILED
# after); interrupt -> INPUT_REQUIRED; every other failure -> FAILED with the
# typed code in metadata and no exception text.
@pytest.mark.parametrize("prompt,expected_state,code", [
    ("fail", "TASK_STATE_FAILED", "NG.TaskFailed"),
    ("coded", "TASK_STATE_FAILED", "app.tool_error"),
    ("hook", "TASK_STATE_REJECTED", "NG.HookBlocked"),
    ("provider", "TASK_STATE_FAILED", "NG.ProviderFailed"),
    ("interrupt", "TASK_STATE_INPUT_REQUIRED", None),
])
@pytest.mark.parametrize("method", ["SendMessage", "SendStreamingMessage"])
def test_failure_class_mapping(srv, prompt, expected_state, code, method):
    if method == "SendMessage":
        _, _, env, body = rpc(srv, method, {"message": v1_text_message(prompt)}, version="1.0")
        task = env["result"]["task"]
    else:
        _, _, events, body = rpc_stream(srv, method, {"message": v1_text_message(prompt)},
                                        version="1.0")
        tid = events[0]["result"]["task"]["id"]
        _, _, got, _ = rpc(srv, "GetTask", {"id": tid}, version="1.0")
        task, env = got["result"], events
        _expect(f"failure_map[{prompt}].{method}.stream_state",
                events[-1]["result"]["statusUpdate"]["status"]["state"] == expected_state,
                f"stream ended {events[-1]}", events)
    state = task["status"]["state"]
    leaked = any(secret in body for secret in ("probe-internal-detail", "/srv/secret", "hunter2"))
    T.add(test=f"failure_map[{prompt}].{method}", state=state,
          status_message=task["status"].get("message"), metadata=task.get("metadata"))
    _expect(f"failure_map[{prompt}].{method}.state", state == expected_state, f"state={state}", env)
    if code:
        got_code = (task.get("metadata") or {}).get("neograph/failure", {}).get("code")
        _expect(f"failure_map[{prompt}].{method}.code", got_code == code, f"code={got_code}", env)
    _expect(f"failure_map[{prompt}].{method}.no_internal_text_leak", not leaked,
            "the wire carries the raw exception text", body)


def test_input_required_resume_with_same_task(srv):
    _, _, env, _ = rpc(srv, "SendMessage", {"message": v1_text_message("interrupt me")}, version="1.0")
    task = env["result"]["task"]
    parts = task["status"]["message"]["parts"]
    _expect("input_required.reason_and_payload(4.1.3)",
            parts[0].get("text") == "probe needs approval" and parts[1].get("data") == {"tool": "probe"},
            f"parts={parts}", env)
    follow = v1_text_message("echo approved", taskId=task["id"], contextId=task["contextId"])
    _, _, env2, _ = rpc(srv, "SendMessage", {"message": follow}, version="1.0")
    T.add(test="input_required.resume", first=env, second=env2)
    resumed = env2.get("result", {}).get("task", {})
    _expect("input_required.resume_same_task(3.4.3)", resumed.get("id") == task["id"],
            f"got {env2}", env2)
    reply = resumed.get("status", {}).get("message", {}).get("parts", [{}])[0].get("text")
    _expect("input_required.resumes_from_checkpoint",
            resumed.get("status", {}).get("state") == "TASK_STATE_COMPLETED"
            and reply == "[probe] resumed: echo approved", f"got {env2}", env2)
