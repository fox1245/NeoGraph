"""The pinned official MCP Python SDK the interop suites need."""

import importlib.metadata

import pytest


def require_official_python_sdk() -> None:
    """Skip the calling module unless the official `mcp` 2.x SDK is installed
    (requirements-mcp-interop.txt pins it); `mcp` carries no __version__."""
    try:
        version = importlib.metadata.version("mcp")
    except importlib.metadata.PackageNotFoundError:
        pytest.skip("the official MCP Python SDK is not installed "
                    "(requirements-mcp-interop.txt)", allow_module_level=True)
    if version.split(".")[0] != "2":
        pytest.skip(f"needs the official MCP Python SDK 2.x, found {version}",
                    allow_module_level=True)
