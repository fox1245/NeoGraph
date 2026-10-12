"""Every message NeoGraph's MCP server emitted in this session, and every
message the official clients sent it, validated against the official MCP
schema.json of the revision each session negotiated: 2025-11-25, 2025-06-18,
2025-03-26 or 2024-11-05 (pinned commit, see the CI job).

The module name sorts it after the other server suites: it reads the wire
transcripts they wrote (and the TypeScript suite's, when that ran first with the
same NG_MCP_TRANSCRIPTS). Responses are matched to their requests so each is
checked against its exact Result definition, and every tool's inputSchema and
outputSchema against the JSON Schema 2020-12 meta-schema.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("jsonschema")

from mcp_server_interop import HELPERS, TRANSCRIPTS  # noqa: E402

SCHEMA_DIR = os.environ.get("NG_MCP_SCHEMA_DIR")


def test_every_captured_message_matches_its_revision_schema():
    if not SCHEMA_DIR:
        pytest.skip("NG_MCP_SCHEMA_DIR not set")
    if not (Path(SCHEMA_DIR) / "2025-11-25" / "schema.json").exists():
        pytest.skip("NG_MCP_SCHEMA_DIR has no 2025-11-25/schema.json")
    if not any(TRANSCRIPTS.glob("*.jsonl")):
        pytest.skip("no transcripts were captured in this session")
    result = subprocess.run(
        [sys.executable, "-I", str(HELPERS / "validate_mcp_transcripts.py"), SCHEMA_DIR,
         str(TRANSCRIPTS), str(TRANSCRIPTS / "schema-validation.json")],
        capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stdout + result.stderr
