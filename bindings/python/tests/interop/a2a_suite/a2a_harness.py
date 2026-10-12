"""Process, port, raw JSON-RPC/SSE and transcript helpers for the A2A interop suite.

The suite runs only when the pinned official SDK (requirements.txt) is
installed; require_suite() skips the calling module otherwise.

Environment:
  NEOGRAPH_A2A_PROBE_SERVER  the neograph_a2a_probe_server binary (default: the
                             build tree's tests/neograph_a2a_probe_server)
  NEOGRAPH_BUILD_DIR         NeoGraph build tree (see interop_support)
  A2A_TRANSCRIPTS            transcript directory (default: a temporary directory)
"""

from __future__ import annotations

import contextlib
import json
import os
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import pytest

from interop_support import (build_binary, installed_pin, neograph_build_dir, require_module,
                              unavailable)

HERE = Path(__file__).resolve().parent
# Every server is started under the port contract of bindings/python/tests/server_ports.py.
sys.path.insert(0, str(HERE.parents[1]))
import server_ports  # noqa: E402

TRANSCRIPTS = Path(os.environ.get("A2A_TRANSCRIPTS",
                                  Path(tempfile.gettempdir()) / "neograph-a2a-interop"))


def require_suite() -> Path:
    """Skip the calling test module unless the pinned SDK and the probe exist."""
    require_module("a2a")
    if installed_pin(HERE / "requirements.txt", "a2a-sdk") is None:
        unavailable("a2a-sdk is not the version pinned in a2a_suite/requirements.txt")
    for module in ("httpx", "jsonschema", "google.protobuf", "google.api"):
        require_module(module)
    probe = build_binary("neograph_a2a_probe_server", "NEOGRAPH_A2A_PROBE_SERVER")
    if probe is None:
        unavailable("neograph_a2a_probe_server is not built")
    return probe


PROBE_SERVER = str(require_suite())

import httpx  # noqa: E402  (after the skip gate)


def import_neograph():
    build = neograph_build_dir()
    if build is not None and str(build) not in sys.path:
        sys.path.insert(0, str(build))
    import neograph_engine  # noqa: PLC0415

    return neograph_engine


class Transcript:
    """Append-only JSONL transcript, one file per probe module."""

    def __init__(self, name: str):
        TRANSCRIPTS.mkdir(parents=True, exist_ok=True)
        self.path = TRANSCRIPTS / f"{name}.jsonl"
        self.path.write_text("")

    def add(self, **entry: Any) -> None:
        with self.path.open("a") as f:
            f.write(json.dumps(entry, default=str) + "\n")


@contextlib.contextmanager
def native_server(*args: str):
    """Run the native NeoGraph A2AServer probe; yields its base URL.

    The probe binds port 0 and prints `READY <port>`."""
    launch = lambda port: server_ports.Launch([PROBE_SERVER, str(port), *args])  # noqa: E731
    with server_ports.start_server(launch, reports_port=True) as server:
        yield server.url()


@contextlib.contextmanager
def subprocess_server(command, env: dict[str, str] | None = None):
    """Run an official SDK server process; yields its `server_ports.ServerProcess`.

    `command(port)` is the argv for a server that binds `port` (0: its own pick) and
    prints `READY <port>` once it serves (sdk_py_server.py and ts/ts_server.cjs do)."""
    launch = lambda port: server_ports.Launch(command(port), env or {})  # noqa: E731
    with server_ports.start_server(launch, reports_port=True) as server:
        yield server


def rpc(base: str, method: str, params: Any = None, *, version: str | None = None,
        rid: Any = 1, raw: str | None = None, headers: dict[str, str] | None = None,
        timeout: float = 30) -> tuple[int, dict[str, str], Any, str]:
    """One JSON-RPC POST. Returns (status, headers, parsed-json-or-None, body)."""
    h = {"Content-Type": "application/json", "Accept": "application/json"}
    if version is not None:
        h["A2A-Version"] = version
    h.update(headers or {})
    if raw is None:
        body: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if rid is not None:
            body["id"] = rid
        if params is not None:
            body["params"] = params
        raw = json.dumps(body)
    r = httpx.post(base.rstrip("/") + "/", content=raw, headers=h, timeout=timeout)
    try:
        parsed = r.json() if r.content else None
    except ValueError:
        parsed = None
    return r.status_code, dict(r.headers), parsed, r.text


def rpc_stream(base: str, method: str, params: Any, *, version: str | None = None,
               rid: Any = 1, timeout: float = 30) -> tuple[int, dict[str, str], list[Any], str]:
    """One streaming JSON-RPC POST; returns every SSE data payload, parsed."""
    h = {"Content-Type": "application/json", "Accept": "text/event-stream"}
    if version is not None:
        h["A2A-Version"] = version
    body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    with httpx.stream("POST", base.rstrip("/") + "/", content=body, headers=h,
                      timeout=timeout) as r:
        text = r.read().decode()
        status, hdrs = r.status_code, dict(r.headers)
    events = []
    for block in text.replace("\r\n", "\n").split("\n\n"):
        data = [ln[5:].lstrip(" ") if ln.startswith("data:") else None for ln in block.split("\n")]
        data = [d for d in data if d is not None]
        if data:
            try:
                events.append(json.loads("\n".join(data)))
            except ValueError:
                events.append({"<unparseable>": "\n".join(data)})
    return status, hdrs, events, text


def v1_text_message(text: str, **extra: Any) -> dict[str, Any]:
    m = {"messageId": f"msg-{time.time_ns()}", "role": "ROLE_USER", "parts": [{"text": text}]}
    m.update(extra)
    return m


def v03_text_message(text: str, **extra: Any) -> dict[str, Any]:
    m = {"kind": "message", "messageId": f"msg-{time.time_ns()}", "role": "user",
         "parts": [{"kind": "text", "text": text}]}
    m.update(extra)
    return m
