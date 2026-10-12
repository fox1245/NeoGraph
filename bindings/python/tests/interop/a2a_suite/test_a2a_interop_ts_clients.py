"""Official @a2a-js/sdk clients (1.3.0 with v0.3 legacy compat, and 0.3.14) -> NeoGraph native server.

Each .cjs script prints one JSON line per check; this wrapper records them and
fails when any check fails. Environment: NODE, A2A_JS_SDK_DIST, A2A_JS_SDK_V03_DIST.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest

from a2a_harness import HERE, Transcript, native_server
from interop_support import node_binary, unavailable

# The pinned @a2a-js/sdk installs (ts/package.json, ts/package-v03.json).
NODE = node_binary()
DISTS = {
    "ts_client_native_server.cjs": os.environ.get("A2A_JS_SDK_DIST", ""),
    "ts_client_v03_native_server.cjs": os.environ.get("A2A_JS_SDK_V03_DIST", ""),
}
if NODE is None:
    unavailable("set NEOGRAPH_INTEROP_NODE to a Node.js >= 20 binary")

T = Transcript("ts_clients_native_server")


@pytest.mark.parametrize("script", list(DISTS))
def test_ts_client(script):
    if not DISTS[script] or not Path(DISTS[script]).is_dir():
        if os.environ.get("NEOGRAPH_INTEROP_REQUIRE") == "1":
            pytest.fail("A2A_JS_SDK_DIST / A2A_JS_SDK_V03_DIST is not set")
        pytest.skip("set A2A_JS_SDK_DIST / A2A_JS_SDK_V03_DIST to the pinned @a2a-js/sdk dist")
    with native_server() as base:
        proc = subprocess.run([NODE, str(HERE / "ts" / script), base, DISTS[script]],
                              capture_output=True, text=True, timeout=180)
    checks = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith("{")]
    for c in checks:
        T.add(script=script, **c)
        print(f"{c['result']} {c['check']}" + (f" :: {c.get('error')}" if c["result"] == "FAIL" else ""))
    failed = [c for c in checks if c["result"] == "FAIL"]
    assert checks, proc.stderr[-2000:]
    assert not failed, json.dumps(failed, indent=1)
