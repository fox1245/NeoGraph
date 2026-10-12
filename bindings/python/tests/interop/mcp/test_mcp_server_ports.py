"""A server that must be told a port (the Harness MCP example) never ends up talking to a decoy.

The decoy takes the number the example was going to get, between choosing it and starting
the example. The suite's start (server_ports.py through mcp_server_interop.http_server)
must start the example again on a fresh number, and nothing may talk to the decoy.
"""

import sys

import pytest

httpx = pytest.importorskip("httpx")

import port_decoy  # noqa: E402
import server_ports  # noqa: E402
from mcp_server_interop import HARNESS, http_server, missing_binaries  # noqa: E402

if missing_binaries():
    pytest.skip(f"NEOGRAPH_BUILD_DIR lacks {missing_binaries()}", allow_module_level=True)

pytestmark = pytest.mark.skipif(
    sys.platform != "linux", reason="who listens on a port is read from /proc, which only Linux has")

INIT = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                   "clientInfo": {"name": "raw", "version": "1"}}}


def test_a_decoy_on_the_harness_port_is_never_talked_to(monkeypatch, tmp_path):
    decoys = port_decoy.take_the_first_number(monkeypatch)
    launch = lambda port: server_ports.Launch(  # noqa: E731
        [str(HARNESS)],
        {"NEOGRAPH_HARNESS_SMOKE": "1", "NEOGRAPH_HARNESS_TRANSPORT": "http",
         "NEOGRAPH_HARNESS_HTTP_PORT": str(port)}, str(tmp_path))
    try:
        with http_server(launch, reports_port=False) as server:
            decoy = decoys[0]
            assert server.port != decoy.port and server.attempts == 2
            answer = httpx.post(
                f"http://127.0.0.1:{server.port}/mcp", json=INIT,
                headers={"Accept": "application/json, text/event-stream"}, timeout=30)
            assert answer.status_code == 200 and "protocolVersion" in answer.text
            assert (decoy.connections, decoy.requests) == (0, 0)
    finally:
        for decoy in decoys:
            decoy.close()
