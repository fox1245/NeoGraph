"""Shared setup for the official MCP SDK clients against NeoGraph's MCP server.

NeoGraph servers under test (all built from this repository):
  - the interop probe, tests/fixtures/mcp_interop_probe_server.cpp
    (CMake target neograph_mcp_interop_probe), over stdio and Streamable HTTP;
  - examples/59_mcp_server.cpp (example_mcp_server);
  - the Harness MCP server with its smoke provider (example_harness_mcp_server).

Environment:
  NEOGRAPH_BUILD_DIR    the build that holds those binaries (required)
  NG_MCP_PROBE_SERVER   the probe binary, if not <build>/tests/neograph_mcp_interop_probe
  NG_MCP_TRANSCRIPTS    where wire transcripts are written (default: a temporary directory)
  MCP_PORT_RANGE        "lo-hi" loopback ports for these tests (default: the OS picks)
"""

import contextlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
HELPERS = HERE / "helpers"
if str(HELPERS) not in sys.path:
    sys.path.insert(0, str(HELPERS))

_build = os.environ.get("NEOGRAPH_BUILD_DIR")
BUILD = Path(_build) if _build else None
PROBE = Path(os.environ.get("NG_MCP_PROBE_SERVER") or
             (BUILD / "tests" / "neograph_mcp_interop_probe" if BUILD else "neograph_mcp_interop_probe"))
HARNESS = BUILD / "example_harness_mcp_server" if BUILD else Path("example_harness_mcp_server")
EXAMPLE59 = BUILD / "example_mcp_server" if BUILD else Path("example_mcp_server")
TRANSCRIPTS = Path(os.environ.get("NG_MCP_TRANSCRIPTS") or
                   tempfile.mkdtemp(prefix="neograph-mcp-interop-"))
TEE = HELPERS / "stdio_tee.py"
# Ports come from the OS unless MCP_PORT_RANGE pins a range: a fixed range
# collides when two test processes run this suite on one machine at once.
_PORT_RANGE = os.environ.get("MCP_PORT_RANGE")
_lo, _hi = (int(x) for x in (_PORT_RANGE or "0-0").split("-"))
_next_port = [_lo]


def missing_binaries() -> list[str]:
    return [str(p) for p in (PROBE, HARNESS, EXAMPLE59) if not p.exists()]


def free_port() -> int:
    if not _PORT_RANGE:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]
    while _next_port[0] <= _hi:
        port = _next_port[0]
        _next_port[0] += 1
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError("no free port in MCP_PORT_RANGE")


def transcript_path(name: str) -> Path:
    TRANSCRIPTS.mkdir(parents=True, exist_ok=True)
    safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in name)
    path = TRANSCRIPTS / f"{safe}.jsonl"
    path.write_text("")
    return path


def read_transcript(path: Path) -> list[dict]:
    out = []
    for line in path.read_text().splitlines():
        rec = json.loads(line)
        raw = rec.get("raw", "")
        try:
            rec["msg"] = json.loads(raw) if raw else None
        except json.JSONDecodeError:
            rec["msg"] = None
        out.append(rec)
    return out


def stdio_command(log: Path, argv: list[str]) -> tuple[str, list[str]]:
    """Command/args that run `argv` behind the recording tee."""
    return sys.executable, ["-I", "-B", str(TEE), str(log), "--", *argv]


def wait_port(port: int, timeout: float = 10.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        with contextlib.suppress(OSError), socket.create_connection(("127.0.0.1", port), 0.2):
            return
        time.sleep(0.05)
    raise RuntimeError(f"server on {port} did not start")


@contextlib.contextmanager
def http_server(argv: list[str], port: int, env: dict | None = None, cwd: str | None = None):
    proc = subprocess.Popen(argv, env={**os.environ, **(env or {})}, cwd=cwd,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_port(port)
        yield proc
    finally:
        proc.terminate()
        with contextlib.suppress(subprocess.TimeoutExpired):
            proc.wait(5)
        if proc.poll() is None:
            proc.kill()


def wire(log: Path, direction: str | None = None) -> list[dict]:
    recs = read_transcript(log)
    return [r for r in recs if direction is None or r["dir"] == direction]
