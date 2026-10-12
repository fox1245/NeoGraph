"""Locate the ACP probe agent, the pinned SDKs and the transcript directory.

Environment:
  NEOGRAPH_ACP_PROBE_AGENT  the neograph_acp_probe_agent binary (default:
                            <build>/tests/neograph_acp_probe_agent)
  NEOGRAPH_BUILD_DIR        the NeoGraph build tree (default: the directory
                            that holds the imported neograph_engine package)
  ACP_TRANSCRIPT_DIR        where per-test NDJSON transcripts go (default: a
                            temporary directory)
  NEOGRAPH_INTEROP_NODE     a Node.js >= 20 binary, for the TypeScript suite
  ACP_TS_SDK_DIR            .../node_modules/@agentclientprotocol/sdk
  NEOGRAPH_INTEROP_REQUIRE  "1" fails, instead of skipping, a module whose
                            prerequisite (pinned SDK, probe agent, Node) is missing
"""

from __future__ import annotations

import os
import tempfile
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path

HERE = Path(__file__).resolve().parent
TAP = HERE / "acp_stdio_tap.py"
PINS = (HERE / "requirements.txt", HERE / "requirements-next.txt")

try:  # shared helpers for every interop suite (bindings/python/tests/interop/)
    from interop_support import (installed_pin, neograph_build_dir, node_binary,
                                 require_module, unavailable)
except ImportError:  # pragma: no cover - the suite run on its own
    import subprocess

    import pytest

    def unavailable(reason):
        if os.environ.get("NEOGRAPH_INTEROP_REQUIRE") == "1":
            pytest.fail(f"interop prerequisite missing: {reason}", pytrace=False)
        pytest.skip(reason, allow_module_level=True)

    def require_module(name):
        try:
            __import__(name)
        except ImportError:
            unavailable(f"Python module {name!r} is not installed")

    def neograph_build_dir():
        env = os.environ.get("NEOGRAPH_BUILD_DIR")
        if env:
            return Path(env)
        try:
            import neograph_engine
        except ImportError:
            return None
        return Path(neograph_engine.__file__).resolve().parent.parent

    def node_binary():
        node = os.environ.get("NEOGRAPH_INTEROP_NODE")
        if not node:
            return None
        try:
            out = subprocess.run([node, "--version"], capture_output=True, text=True, timeout=10)
            major = int(out.stdout.strip().lstrip("v").split(".")[0])
        except (OSError, ValueError, subprocess.SubprocessError):
            return None
        return node if major >= 20 else None

    def installed_pin(requirements_path, dist_name):
        try:
            installed = version(dist_name)
        except PackageNotFoundError:
            return None
        for line in Path(requirements_path).read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            name, sep, pinned = line.partition("==")
            if sep and name.strip().lower() == dist_name.lower() and pinned.strip() == installed:
                return installed
        return None


def pinned_acp_sdk() -> str | None:
    """The installed agent-client-protocol version when it is one we pin."""
    for pins in PINS:
        found = installed_pin(pins, "agent-client-protocol")
        if found:
            return found
    return None


def probe_agent() -> Path | None:
    env = os.environ.get("NEOGRAPH_ACP_PROBE_AGENT")
    if env:
        return Path(env) if Path(env).is_file() else None
    build = neograph_build_dir()
    if build is None:
        return None
    for candidate in (Path(build) / "tests" / "neograph_acp_probe_agent",
                      Path(build) / "tests" / "Release" / "neograph_acp_probe_agent.exe",
                      Path(build) / "tests" / "neograph_acp_probe_agent.exe"):
        if candidate.is_file():
            return candidate
    return None


_transcripts: Path | None = None


def transcript_dir(label: str) -> Path:
    global _transcripts
    root = os.environ.get("ACP_TRANSCRIPT_DIR")
    if root:
        base = Path(root)
    else:
        if _transcripts is None:
            _transcripts = Path(tempfile.mkdtemp(prefix="neograph-acp-interop-"))
        base = _transcripts
    path = base / label
    path.mkdir(parents=True, exist_ok=True)
    return path


def require_probe_agent() -> Path:
    agent = probe_agent()
    if agent is None:
        unavailable("neograph_acp_probe_agent is not built (set NEOGRAPH_ACP_PROBE_AGENT "
                    "or NEOGRAPH_BUILD_DIR)")
    return agent


def require_pinned_acp_sdk() -> str:
    require_module("acp")
    found = pinned_acp_sdk()
    if found is None:
        unavailable("agent-client-protocol is not one of the versions pinned in "
                    "requirements.txt / requirements-next.txt")
    return found
