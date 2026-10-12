"""Official ACP TypeScript SDK -> native NeoGraph ACPServer (node:test suite).

Runs acp_ts_sdk_interop.test.mjs with the pinned @agentclientprotocol/sdk and
validates every message the agent sent against the vendored ACP schema.

Environment: NEOGRAPH_INTEROP_NODE (Node.js >= 20) and ACP_TS_SDK_DIR
(.../node_modules/@agentclientprotocol/sdk, installed from package.json).
"""

import json
import os
import subprocess
import sys
from pathlib import Path

from acp_interop_support import (  # noqa: E402
    HERE,
    node_binary,
    require_module,
    require_probe_agent,
    transcript_dir,
    unavailable,
)

require_module("jsonschema")
AGENT = require_probe_agent()
NODE = node_binary()
if NODE is None:
    unavailable("set NEOGRAPH_INTEROP_NODE to a Node.js >= 20 binary")
SDK_DIR = os.environ.get("ACP_TS_SDK_DIR", "")
PINNED = json.loads((HERE / "package.json").read_text())["dependencies"]["@agentclientprotocol/sdk"]


def _installed_sdk_version():
    try:
        return json.loads((Path(SDK_DIR) / "package.json").read_text())["version"]
    except (OSError, ValueError, KeyError):
        return None


if not SDK_DIR or _installed_sdk_version() != PINNED:
    unavailable(f"set ACP_TS_SDK_DIR to @agentclientprotocol/sdk {PINNED} "
                "(npm install from acp_suite/package.json)")

from acp_schema_check import agent_problems  # noqa: E402


def test_official_typescript_sdk_against_native_agent():
    transcripts = transcript_dir(f"ts-{PINNED}")
    env = {**os.environ, "ACP_TS_SDK_DIR": SDK_DIR, "NEOGRAPH_ACP_AGENT": str(AGENT),
           "PYTHON": sys.executable, "ACP_TRANSCRIPT_DIR": str(transcripts)}
    proc = subprocess.run([NODE, "--test", str(HERE / "acp_ts_sdk_interop.test.mjs")],
                          env=env, capture_output=True, text=True, timeout=600)
    assert proc.returncode == 0, proc.stdout[-8000:] + proc.stderr[-4000:]
    problems = [p for path in sorted(transcripts.glob("*.jsonl")) for p in agent_problems(path)]
    assert not problems, "agent messages fail the ACP schema:\n" + "\n".join(problems)
    wire = "".join(path.read_text() for path in transcripts.glob("*.jsonl"))
    assert "hunter2" not in wire and "/srv/secret" not in wire
