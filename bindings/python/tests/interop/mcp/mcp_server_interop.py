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

Every server is started under the port contract of bindings/python/tests/server_ports.py.
"""

import contextlib
import json
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
HELPERS = HERE / "helpers"
if str(HELPERS) not in sys.path:
    sys.path.insert(0, str(HELPERS))
TESTS = HERE.parents[1]  # bindings/python/tests
if str(TESTS) not in sys.path:
    sys.path.insert(0, str(TESTS))
import server_ports  # noqa: E402

_build = os.environ.get("NEOGRAPH_BUILD_DIR")
BUILD = Path(_build) if _build else None
PROBE = Path(os.environ.get("NG_MCP_PROBE_SERVER") or
             (BUILD / "tests" / "neograph_mcp_interop_probe" if BUILD else "neograph_mcp_interop_probe"))
HARNESS = BUILD / "example_harness_mcp_server" if BUILD else Path("example_harness_mcp_server")
EXAMPLE59 = BUILD / "example_mcp_server" if BUILD else Path("example_mcp_server")
TRANSCRIPTS = Path(os.environ.get("NG_MCP_TRANSCRIPTS") or
                   tempfile.mkdtemp(prefix="neograph-mcp-interop-"))
TEE = HELPERS / "stdio_tee.py"


def missing_binaries() -> list[str]:
    return [str(p) for p in (PROBE, HARNESS, EXAMPLE59) if not p.exists()]


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


@contextlib.contextmanager
def http_server(launch_for_port, *, reports_port: bool = True):
    """Start an HTTP server under the port contract (server_ports) and yield it.

    The probe binds port 0 and reports its port (reports_port=True); a server that
    has to be told a number (the Harness example) says so with reports_port=False.
    `launch_for_port(n)` gives its `server_ports.Launch`."""
    with server_ports.start_server(launch_for_port, reports_port=reports_port) as server:
        yield server


def probe_launch(env: dict | None = None):
    """The probe on Streamable HTTP, for http_server."""
    return lambda port: server_ports.Launch([str(PROBE), "http", str(port)], env or {})


def wire(log: Path, direction: str | None = None) -> list[dict]:
    recs = read_transcript(log)
    return [r for r in recs if direction is None or r["dir"] == direction]
