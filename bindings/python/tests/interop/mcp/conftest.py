"""Fixtures for the official MCP SDK interop suites (MCP 2025-11-25).

    NEOGRAPH_BUILD_DIR=<build> PYTHONPATH=<build>:bindings/python \
        python -m pytest bindings/python/tests/interop/mcp

The tests need the pinned SDKs (requirements-mcp-interop.txt; package.json for
the TypeScript SDKs) and skip without them, so the regular test run is
unaffected. See mcp_server_interop.py and mcp_interop_support.py for the
environment each side reads.
"""

import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import mcp_server_interop as server_side  # noqa: E402


@pytest.fixture(scope="module")
def probe_http():
    """Probe server on Streamable HTTP; yields its port."""
    with server_side.http_server(server_side.probe_launch(
            {"NG_PROBE_ALLOWED_ORIGINS": "http://allowed.example"})) as server:
        yield server.port


@pytest.fixture(scope="module")
def harness_http(tmp_path_factory):
    cwd = tmp_path_factory.mktemp("harness")
    # The example takes its port from the environment and does not report it.
    launch = lambda port: server_side.server_ports.Launch(  # noqa: E731
        [str(server_side.HARNESS)],
        {"NEOGRAPH_HARNESS_SMOKE": "1", "NEOGRAPH_HARNESS_TRANSPORT": "http",
         "NEOGRAPH_HARNESS_HTTP_PORT": str(port)}, str(cwd))
    with server_side.http_server(launch, reports_port=False) as server:
        yield server.port


@pytest.fixture
def recorded(request):
    """Factory: proxy(port) -> (url, log) recording the HTTP wire for this test."""
    from http_record_proxy import HttpRecordProxy

    proxies = []

    def make(upstream_port: int, suffix: str = "") -> tuple[str, Path]:
        log = server_side.transcript_path(f"py-{request.node.name}{suffix}")
        proxy = HttpRecordProxy(upstream_port, str(log)).start()
        proxies.append(proxy)
        return f"http://127.0.0.1:{proxy.port}/mcp", log

    yield make
    for proxy in proxies:
        proxy.stop()
