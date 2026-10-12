"""Support code for NeoGraph's MCP client against the official MCP SDK servers.

Environment:
  * NG_MCP_NODE_DEPS     directory whose node_modules holds the pinned TS SDKs
                         (package.json); TS peers are skipped when unset
  * NG_NODE              node binary (default: "node" on PATH)
  * NG_MCP1_PYTHON       interpreter of a venv with requirements-mcp1-interop.txt
                         (mcp 1.30.0, FastMCP); FastMCP v1 peers are skipped when unset
  * NG_MCP_SCHEMA_DIR    directory with <revision>/schema.json (pinned commit);
                         schema checks are skipped when unset
  * NG_MCP_PORT_BASE     first loopback port to use (default 18200; 100 ports)
  * NG_MCP_TRANSCRIPTS   where wire transcripts are written (default: a temporary
                         directory); this side writes into its "client" subdirectory

The official servers are started as subprocesses.  Every client->server message
is captured (stdio tap or HTTP recorder) so assertions can show the exact wire
message NeoGraph sent next to the official SDK's reaction.
"""

from __future__ import annotations

import contextlib
import dataclasses
import itertools
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Iterator

HERE = Path(__file__).resolve().parent
SERVERS = HERE / "servers"
TOOLS = HERE / "helpers"

import neograph_engine as ng  # noqa: E402

PY = sys.executable
NODE = os.environ.get("NG_NODE") or shutil.which("node") or "node"
NODE_DEPS = os.environ.get("NG_MCP_NODE_DEPS")
MCP1_PYTHON = os.environ.get("NG_MCP1_PYTHON")  # interpreter of a venv with mcp==1.30.0
SCHEMA_PATH = (str(Path(os.environ["NG_MCP_SCHEMA_DIR"]) / "2025-11-25" / "schema.json")
               if os.environ.get("NG_MCP_SCHEMA_DIR") else None)
TRANSCRIPTS = Path(os.environ.get("NG_MCP_TRANSCRIPTS") or
                   tempfile.mkdtemp(prefix="neograph-mcp-client-interop-")) / "client"
TRANSCRIPTS.mkdir(parents=True, exist_ok=True)
_PORT_BASE = int(os.environ.get("NG_MCP_PORT_BASE", "18200"))
_ports = itertools.cycle(range(_PORT_BASE, _PORT_BASE + 100))


def free_port() -> int:
    for _ in range(100):
        port = next(_ports)
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError("no free port in the configured range")


def wait_port(port: int, proc: subprocess.Popen, timeout: float = 20.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early with {proc.returncode}")
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.05)
    raise RuntimeError("server did not start listening")


def read_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            out.append(json.loads(line))
    return out


@dataclasses.dataclass(frozen=True)
class Peer:
    """One official server configuration."""

    id: str
    sdk: str            # "py" (mcp 2.3.0) | "ts" (@modelcontextprotocol/sdk 1.32.1) | "ts2" (@modelcontextprotocol/server 2.3.1)
    transport: str      # "stdio" | "http"
    stateless: bool = False
    json_response: bool = False
    flavor: str = "lowlevel"   # py only: "lowlevel" | "mcpserver"

    @property
    def stateful_http(self) -> bool:
        return self.transport == "http" and not self.stateless


PEERS = [
    Peer("py-stdio", "py", "stdio"),
    Peer("py-http-sse", "py", "http"),
    Peer("py-http-json", "py", "http", json_response=True),
    Peer("py-http-stateless-sse", "py", "http", stateless=True),
    Peer("py-http-stateless-json", "py", "http", stateless=True, json_response=True),
    Peer("ts-stdio", "ts", "stdio"),
    Peer("ts-http-sse", "ts", "http"),
    Peer("ts-http-json", "ts", "http", json_response=True),
    Peer("ts-http-stateless-json", "ts", "http", stateless=True, json_response=True),
]
# @modelcontextprotocol/server 2.3.1 (TS SDK v2, implements 2026-07-28; serves the
# 2025 handshake as stateless "legacy" traffic over HTTP via createMcpHandler).
TS2_PEERS = [
    Peer("ts2-stdio", "ts2", "stdio"),
    Peer("ts2-http-legacy-stateless", "ts2", "http", stateless=True),
]
# mcp 1.30.0 FastMCP (the v1 line most deployed servers still run).
PY1_PEERS = [
    Peer("py1-fastmcp-stdio", "py1", "stdio"),
    Peer("py1-fastmcp-http-sse", "py1", "http"),
    Peer("py1-fastmcp-http-json", "py1", "http", json_response=True),
    Peer("py1-fastmcp-http-stateless-json", "py1", "http", stateless=True, json_response=True),
]
MCPSERVER_PEERS = [
    Peer("py-mcpserver-stdio", "py", "stdio", flavor="mcpserver"),
    Peer("py-mcpserver-http-sse", "py", "http", flavor="mcpserver"),
]


class Run:
    """One server instance plus its transcript files, owned by one test."""

    def __init__(self, peer: Peer, name: str, extra: list[str] | None = None,
                 path: str = "/mcp", tap_args: list[str] | None = None):
        self.peer = peer
        self.name = name
        self.dir = TRANSCRIPTS / name
        if self.dir.exists():
            shutil.rmtree(self.dir)
        self.dir.mkdir(parents=True)
        self.wire = self.dir / "wire.jsonl"
        self.events = self.dir / "events.jsonl"
        self.extra = list(extra or [])
        self.path = path
        self.tap_args = list(tap_args or [])
        self.proc: subprocess.Popen | None = None
        self.port: int | None = None
        self.stderr = self.dir / "server.stderr"

    # -- server command ------------------------------------------------------
    def server_argv(self) -> list[str]:
        p = self.peer
        if p.sdk == "py":
            argv = [PY, "-I", str(SERVERS / "py_official_server.py"), "--flavor", p.flavor,
                    "--transport", p.transport, "--events", str(self.events)]
        elif p.sdk == "py1":
            if not MCP1_PYTHON:
                raise RuntimeError("NG_MCP1_PYTHON is not set")
            argv = [MCP1_PYTHON, "-I", str(SERVERS / "py1_official_server.py"),
                    "--transport", p.transport, "--events", str(self.events)]
        elif p.sdk in ("ts", "ts2"):
            if not NODE_DEPS:
                raise RuntimeError("NG_MCP_NODE_DEPS is not set")
            script = "ts_official_server.cjs" if p.sdk == "ts" else "ts2_official_server.cjs"
            argv = [NODE, str(SERVERS / script),
                    "--transport", p.transport, "--events", str(self.events)]
        else:
            raise ValueError(p.sdk)
        if p.transport == "http":
            argv += ["--port", str(self.port), "--path", self.path, "--record", str(self.wire)]
            if p.stateless and p.sdk != "ts2":
                argv.append("--stateless")
            if p.json_response:
                argv.append("--json-response")
        return argv + self.extra

    def env(self) -> dict[str, str]:
        env = dict(os.environ)
        if NODE_DEPS:
            env["NG_MCP_NODE_DEPS"] = NODE_DEPS
        return env

    def start(self) -> "Run":
        if self.peer.transport == "http":
            self.port = free_port()
            self.proc = subprocess.Popen(self.server_argv(), env=self.env(),
                                         stdout=subprocess.DEVNULL,
                                         stderr=open(self.stderr, "wb"))
            wait_port(self.port, self.proc)
        return self

    def restart(self) -> None:
        """Restart an HTTP server on the same port (all sessions are lost)."""
        assert self.peer.transport == "http"
        self.stop()
        self.proc = subprocess.Popen(self.server_argv(), env=self.env(),
                                     stdout=subprocess.DEVNULL, stderr=open(self.stderr, "ab"))
        wait_port(self.port, self.proc)

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None

    # -- client --------------------------------------------------------------
    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}{self.path}"

    def stdio_argv(self) -> list[str]:
        return [PY, "-I", str(TOOLS / "stdio_tap.py"), "--log", str(self.wire), *self.tap_args,
                "--", *self.server_argv()]

    def client(self, request_timeout: float | None = None, url: str | None = None):
        if self.peer.transport == "stdio":
            return ng.mcp.MCPClient(self.stdio_argv())
        if request_timeout is None:
            return ng.mcp.MCPClient(url or self.url)
        cfg = ng.mcp.MCPClientConfig()
        import datetime
        cfg.request_timeout = datetime.timedelta(seconds=request_timeout)
        return ng.mcp.MCPClient(url or self.url, cfg)

    # -- transcript ----------------------------------------------------------
    def settle(self, quiet: float = 0.25, limit: float = 3.0) -> None:
        """Wait until the recorder stopped appending (HTTP recorders write after the response)."""
        deadline = time.time() + limit
        last = -1
        stable_since = time.time()
        while time.time() < deadline:
            size = self.wire.stat().st_size if self.wire.exists() else 0
            if size != last:
                last, stable_since = size, time.time()
            elif time.time() - stable_since >= quiet:
                return
            time.sleep(0.05)

    def wire_entries(self) -> list[dict]:
        self.settle()
        return read_jsonl(self.wire)

    def server_events(self) -> list[dict]:
        return read_jsonl(self.events)

    def client_messages(self) -> list[dict]:
        """Every JSON message the NeoGraph client sent, with transport metadata."""
        out = []
        for e in self.wire_entries():
            if self.peer.transport == "stdio":
                if e.get("dir") == "c2s":
                    out.append({"message": e.get("json"), "raw": e.get("raw")})
            else:
                body = e.get("request_body") or ""
                msg = None
                with contextlib.suppress(ValueError):
                    msg = json.loads(body) if body else None
                out.append({"message": msg, "raw": body, "http": {
                    "method": e["method"], "path": e["path"], "status": e["status"],
                    "headers": {k.lower(): v for k, v in e["request_headers"]},
                    "response_headers": {k.lower(): v for k, v in e["response_headers"]},
                    "response_body": e["response_body"]}})
        return out

    def http_exchanges(self) -> list[dict]:
        return [m for m in self.client_messages() if "http" in m]


@contextlib.contextmanager
def server(peer: Peer, name: str, **kw) -> Iterator[Run]:
    run = Run(peer, name, **kw)
    try:
        yield run.start()
    finally:
        run.stop()


def short(obj: Any, limit: int = 600) -> str:
    text = obj if isinstance(obj, str) else json.dumps(obj, sort_keys=True)
    return text if len(text) <= limit else text[:limit] + "...(truncated)"


# --------------------------------------------------------------------------- schema

class SchemaOracle:
    """Validates client-originated messages against the official MCP schema.json."""

    def __init__(self, path: str):
        import jsonschema
        from referencing import Registry, Resource

        self.schema = json.loads(Path(path).read_text(encoding="utf-8"))
        self.defs = self.schema.get("$defs") or self.schema.get("definitions")
        self.resource = Resource.from_contents(self.schema)
        self.registry = Registry().with_resource("urn:mcp", self.resource)
        self.cls = jsonschema.validators.validator_for(self.schema)
        self.defs_key = "$defs" if "$defs" in self.schema else "definitions"
        self.client_requests = self._methods("ClientRequest")
        self.client_notifications = self._methods("ClientNotification")
        self.server_requests = self._methods("ServerRequest")

    def _methods(self, union: str) -> dict[str, str]:
        out = {}
        node = self.defs.get(union)
        if node is None:  # e.g. 2026-07-28 removed server-initiated requests (MRTR)
            return {}
        members = node.get("anyOf") or node.get("oneOf")
        if members is None:  # a single-member "union" (e.g. 2026-07-28 ClientNotification)
            const = node.get("properties", {}).get("method", {}).get("const")
            return {const: union} if const else {}
        for ref in members:
            name = ref["$ref"].split("/")[-1]
            const = self.defs[name]["properties"]["method"].get("const")
            out[const] = name
        return out

    def errors(self, value: Any, definition: str) -> list[str]:
        ref = {"$ref": f"urn:mcp#/{self.defs_key}/{definition}"}
        validator = self.cls(ref, registry=self.registry)
        return [f"{'/'.join(map(str, e.absolute_path)) or '$'}: {e.message}"
                for e in validator.iter_errors(value)]

    def validate_client_message(self, msg: Any, server_request_methods: dict) -> list[str]:
        """Return a list of schema violations (empty = valid)."""
        if not isinstance(msg, dict):
            return ["not a JSON object"]
        errs = self.errors(msg, "JSONRPCMessage")
        if "method" in msg and "id" in msg:
            name = self.client_requests.get(msg["method"])
            if name is None:
                return errs + [f"method {msg['method']!r} is not a ClientRequest in this revision"]
            return errs + self.errors(msg, name)
        if "method" in msg:
            name = self.client_notifications.get(msg["method"])
            if name is None:
                return errs + [f"notification {msg['method']!r} is not a ClientNotification"]
            return errs + self.errors(msg, name)
        # A response to a server->client request.
        method = server_request_methods.get(json.dumps(msg.get("id")))
        if "error" in msg:
            return errs + self.errors(msg, "JSONRPCErrorResponse")
        result_def = {"ping": "EmptyResult", "roots/list": "ListRootsResult",
                      "sampling/createMessage": "CreateMessageResult",
                      "elicitation/create": "ElicitResult"}.get(method, "Result")
        return errs + self.errors(msg, "JSONRPCResultResponse") + self.errors(msg.get("result"), result_def)


_oracle: SchemaOracle | None = None


def oracle() -> SchemaOracle | None:
    global _oracle
    if _oracle is None and SCHEMA_PATH:
        _oracle = SchemaOracle(SCHEMA_PATH)
    return _oracle


def record_result(name: str, payload: dict) -> None:
    """Append a machine-readable verdict line kept with the transcripts."""
    with open(TRANSCRIPTS / "results.jsonl", "a", encoding="utf-8") as fh:
        fh.write(json.dumps({"test": name, **payload}) + "\n")
