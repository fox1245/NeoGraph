"""Shared helpers for the official-SDK interop suites (a2a_suite, acp_suite).

The suites drive NeoGraph's native protocol implementations with the official
SDKs at pinned versions. They run only when the pinned SDK is installed and
skip cleanly otherwise, so the main test job (which installs other versions)
is unaffected; the protocol-interop CI job installs the pins.

Environment:
  NEOGRAPH_BUILD_DIR        NeoGraph build tree (default: the directory holding
                            the imported neograph_engine package)
  NEOGRAPH_INTEROP_NODE     Node.js >= 20 binary for the TypeScript SDK suites
  NEOGRAPH_INTEROP_REQUIRE  "1" turns every missing prerequisite into a failure
                            instead of a skip (the protocol-interop CI job sets
                            it, so a broken install cannot pass as skipped)
"""

from __future__ import annotations

import importlib.metadata
import os
import re
import subprocess
from pathlib import Path

import pytest


def unavailable(reason: str) -> None:
    """Skip the calling module, or fail it when NEOGRAPH_INTEROP_REQUIRE=1."""
    if os.environ.get("NEOGRAPH_INTEROP_REQUIRE") == "1":
        pytest.fail(f"interop prerequisite missing: {reason}", pytrace=False)
    pytest.skip(reason, allow_module_level=True)


def require_module(name: str) -> None:
    """importorskip that honours NEOGRAPH_INTEROP_REQUIRE."""
    try:
        __import__(name)
    except ImportError:
        unavailable(f"Python module {name!r} is not installed")


def neograph_build_dir() -> Path | None:
    """The NeoGraph build tree whose binaries and Python module are tested."""
    explicit = os.environ.get("NEOGRAPH_BUILD_DIR")
    if explicit:
        return Path(explicit)
    try:
        import neograph_engine  # noqa: PLC0415
    except ImportError:
        return None
    return Path(neograph_engine.__file__).resolve().parent.parent


def build_binary(name: str, env_var: str) -> Path | None:
    """A test fixture executable from the build tree, or the path in env_var."""
    explicit = os.environ.get(env_var)
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    build = neograph_build_dir()
    if build is None:
        return None
    for candidate in (build / "tests" / name, build / "tests" / "Release" / name, build / name):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def installed_pin(requirements: Path, dist: str) -> str | None:
    """The installed version of `dist` when it equals a `dist==X` pin in the file."""
    try:
        installed = importlib.metadata.version(dist)
    except importlib.metadata.PackageNotFoundError:
        return None
    pattern = re.compile(rf"^\s*{re.escape(dist)}(\[[^\]]*\])?\s*==\s*([^\s;#]+)", re.IGNORECASE)
    for line in requirements.read_text().splitlines():
        match = pattern.match(line)
        if match and match.group(2) == installed:
            return installed
    return None


def node_binary() -> str | None:
    """Node.js >= 20 from NEOGRAPH_INTEROP_NODE, or None."""
    node = os.environ.get("NEOGRAPH_INTEROP_NODE")
    if not node:
        return None
    try:
        out = subprocess.run([node, "--version"], capture_output=True, text=True, timeout=10).stdout
        major = int(out.strip().lstrip("v").split(".")[0])
    except (OSError, ValueError, subprocess.SubprocessError):
        return None
    return node if major >= 20 else None
