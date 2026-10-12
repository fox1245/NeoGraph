"""NeoGraph's native A2A client (C++, through the Python binding) -> official SDK servers.

Servers: a2a-sdk (Python) 1.0-only, a2a-sdk with v0.3 compat listing 0.3 first,
@a2a-js/sdk 1.x 1.0-only and with legacy compat. Every request NeoGraph puts on
the wire is recorded by the server and validated against the A2A schemas.

Environment: NEOGRAPH_BUILD_DIR, A2A_JS_SDK_DIST (…/@a2a-js/sdk/dist),
A2A_JS_EXPRESS (…/node_modules/express), NEOGRAPH_INTEROP_NODE (Node.js >= 20).
The @a2a-js servers are skipped without them.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from a2a_harness import HERE, Transcript, import_neograph, subprocess_server
from a2a_oracle import V03_REQUEST_DEF, v1_method_set, v1_request_type, validate_v03, validate_v1

from interop_support import neograph_build_dir, node_binary  # noqa: E402

ng = import_neograph()
a2a = ng.a2a
T = Transcript("native_client_sdk_servers")
NODE = node_binary()
JS_DIST = os.environ.get("A2A_JS_SDK_DIST", "")
JS_EXPRESS = os.environ.get("A2A_JS_EXPRESS", "")
HAVE_TS = bool(NODE and JS_DIST and JS_EXPRESS and Path(JS_DIST).is_dir()
               and Path(JS_EXPRESS).is_dir())

SERVERS = {
    "py-1.0": ("py", []),
    "py-compat-0.3-first": ("py", ["--compat03", "--v03-first"]),
    "ts-1.0": ("ts", []),
    "ts-compat": ("ts", ["--compat03"]),
}


class Peer:
    def __init__(self, url, record):
        self.url, self.record = url, record

    def requests(self):
        if not self.record.exists():
            return []
        return [json.loads(line) for line in self.record.read_text().splitlines() if line.strip()]


@pytest.fixture(scope="module", params=list(SERVERS))
def peer(request, tmp_path_factory):
    kind, args = SERVERS[request.param]
    if kind == "ts" and not HAVE_TS:
        if os.environ.get("NEOGRAPH_INTEROP_REQUIRE") == "1":
            pytest.fail("NEOGRAPH_INTEROP_NODE, A2A_JS_SDK_DIST or A2A_JS_EXPRESS is not set")
        pytest.skip("set NEOGRAPH_INTEROP_NODE, A2A_JS_SDK_DIST and A2A_JS_EXPRESS for @a2a-js")
    record = tmp_path_factory.mktemp("rec") / f"{request.param}.jsonl"
    if kind == "py":
        command = lambda port: [sys.executable, "-I", str(HERE / "sdk_py_server.py"), str(port),  # noqa: E731
                                *args, "--record", str(record)]
    else:
        command = lambda port: [NODE, str(HERE / "ts" / "ts_server.cjs"), str(port),  # noqa: E731
                                JS_DIST, JS_EXPRESS, *args, "--record", str(record)]
    with subprocess_server(command) as server:
        p = Peer(server.url(), record)
        p.name = request.param
        yield p
        T.add(test=f"[{request.param}] recorded_requests", requests=p.requests())


def _client(peer, card=True):
    c = a2a.A2AClient(peer.url)
    c.set_timeout(15)
    if card:
        c.fetch_agent_card()
    return c


def _check(test, fn):
    try:
        out = fn()
    except Exception as e:  # noqa: BLE001 - the client's rejection is the finding
        T.add(test=test, result="FAIL", error=f"{type(e).__name__}: {e}", code=getattr(e, "code", None))
        print(f"FAIL {test} :: {type(e).__name__}: {e}")
        raise
    T.add(test=test, result="PASS", outcome=str(out)[:1500])
    print(f"PASS {test}")
    return out


def test_probe_without_card(peer):
    # The caller never fetched the card: the client fetches it before its first
    # call (owner decision D5) instead of probing 0.3 first, which a 1.0-only
    # agent answers with VersionNotSupportedError (-32009, spec 3.6.2).
    c = _client(peer, card=False)
    task = _check(f"[{peer.name}] send.no_card_probe", lambda: c.send_message("echo probe"))
    assert task.status.state == a2a.TaskState.Completed


def test_card_then_send(peer):
    c = _client(peer)
    task = _check(f"[{peer.name}] send", lambda: c.send_message("echo hi"))
    assert task.status.state == a2a.TaskState.Completed and task.artifacts


def test_stream(peer):
    c = _client(peer)
    seen = []
    task = _check(f"[{peer.name}] stream", lambda: c.send_message_stream(
        "echo stream", lambda ev: (seen.append(str(ev.type)), True)[1]))
    T.add(test=f"[{peer.name}] stream.events", events=seen)
    assert task.status.state == a2a.TaskState.Completed and task.artifacts, seen


def test_direct_message_reply(peer):
    c = _client(peer)
    task = _check(f"[{peer.name}] send.message_reply", lambda: c.send_message("message hi"))
    # The binding only returns Task: a direct Message becomes a Completed Task with an empty id.
    T.add(test=f"[{peer.name}] send.message_reply.shape", id=task.id, state=str(task.status.state))
    assert task.status.state == a2a.TaskState.Completed


@pytest.mark.parametrize("word,state", [("fail", "Failed"), ("interrupt", "InputRequired")])
def test_states(peer, word, state):
    c = _client(peer)
    task = _check(f"[{peer.name}] send.{word}", lambda: c.send_message(f"{word} x"))
    assert task.status.state == getattr(a2a.TaskState, state)


def test_get_and_cancel_errors(peer):
    c = _client(peer)
    t = c.send_message("echo t")
    _check(f"[{peer.name}] get", lambda: c.get_task(t.id))
    with pytest.raises(a2a.A2ARpcError) as nf:
        c.get_task("no-such-task")
    with pytest.raises(a2a.A2ARpcError) as nc:
        c.cancel_task(t.id)
    T.add(test=f"[{peer.name}] errors", task_not_found=nf.value.code, not_cancelable=nc.value.code)
    if peer.name == "py-compat-0.3-first":
        # Upstream a2a-sdk 1.2.2 quirk: its 0.3 compat layer reports every A2A error
        # as -32603 "Internal error"; not a NeoGraph finding.
        assert nf.value.code in (-32001, -32603) and nc.value.code in (-32002, -32603)
    else:
        assert nf.value.code == -32001 and nc.value.code == -32002


def test_cancel_running(peer):
    c = _client(peer)
    first = threading.Event()
    box = {}

    def on_event(ev):
        if not first.is_set():
            box["id"] = ev.task.id if ev.task else (ev.status_update.task_id if ev.status_update else "")
            first.set()
        return True

    th = threading.Thread(target=lambda: box.setdefault(
        "final", c.send_message_stream("slow c", on_event)), daemon=True)
    th.start()
    assert first.wait(10)
    canceled = _check(f"[{peer.name}] cancel.running", lambda: c.cancel_task(box["id"]))
    th.join(15)
    assert canceled.status.state == a2a.TaskState.Canceled


def test_requests_validate_against_schema(peer):
    _client(peer).send_message("echo validate")
    problems = []
    methods_1_0 = set(v1_method_set())
    for r in peer.requests():
        if r["method"] != "POST":
            continue
        env = json.loads(r["body"])
        version = {k.lower(): v for k, v in r["headers"].items()}.get("a2a-version")
        m = env.get("method")
        if version and version.startswith("1"):
            if m not in methods_1_0:
                problems.append(f"1.0 request uses unknown method {m!r}")
                continue
            problems += [f"{m}: {p}" for p in validate_v1(v1_request_type(m), env.get("params", {}))]
        else:
            if m in V03_REQUEST_DEF:
                problems += [f"{m}: {p}" for p in validate_v03(V03_REQUEST_DEF[m], env)]
            else:
                problems.append(f"0.3 request uses unknown method {m!r} (A2A-Version={version!r})")
    T.add(test=f"[{peer.name}] request_schema", result="PASS" if not problems else "FAIL",
          problems=problems)
    assert not problems, problems


def test_message_ids_unique_across_processes(peer):
    # messageId is "the unique identifier (e.g. UUID) of the message" and agents may
    # de-duplicate on it (spec 3.3.1). Two fresh client processes must not collide.
    code = (
        "import sys; sys.path.insert(0, %r); import neograph_engine as ng; "
        "c = ng.a2a.A2AClient(%r); c.set_timeout(15); c.fetch_agent_card(); c.send_message('echo dup')"
    ) % (str(neograph_build_dir()), peer.url)
    before = len(peer.requests())
    for _ in range(2):
        subprocess.run([sys.executable, "-I", "-c", code], check=True, timeout=60)
    time.sleep(0.3)
    ids = []
    for r in peer.requests()[before:]:
        if r["method"] == "POST":
            p = json.loads(r["body"]).get("params", {})
            if "message" in p:
                ids.append(p["message"]["messageId"])
    T.add(test=f"[{peer.name}] message_id_uniqueness", ids=ids,
          result="PASS" if len(set(ids)) == len(ids) else "FAIL")
    assert len(set(ids)) == len(ids), f"two client processes sent the same messageId: {ids}"


def test_interface_url_on_a_sub_path(tmp_path):
    # spec 8.3.2: clients MUST use the URL of the selected interface. The card is at
    # the well-known root; the JSON-RPC endpoint is /a2a/v1.
    command = lambda port: [sys.executable, "-I", str(HERE / "sdk_py_server.py"), str(port),  # noqa: E731
                            "--rpc-path", "/a2a/v1"]
    with subprocess_server(command) as server:
        url = server.url()
        c = a2a.A2AClient(url)
        c.set_timeout(15)
        card = c.fetch_agent_card()
        T.add(test="[py-1.0 rpc=/a2a/v1] card", interfaces=[i.url for i in card.supported_interfaces])
        task = _check("[py-1.0 rpc=/a2a/v1] send uses the interface URL", lambda: c.send_message("echo sub"))
        assert task.status.state == a2a.TaskState.Completed
