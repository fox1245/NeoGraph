"""A stdio MCP server built with the official MCP Python SDK (FastMCP).

An ACP client passes it to the agent in session/new `mcpServers`; the
interop tests then call its tools through the agent.
"""

import os

from mcp.server.fastmcp import FastMCP

server = FastMCP("neograph-acp-interop")


@server.tool()
def add(a: int, b: int) -> int:
    """Add two integers."""
    return a + b


@server.tool()
def env_probe() -> str:
    """Report ACP_INTEROP_ENV, which the ACP client sets through `env`."""
    return os.environ.get("ACP_INTEROP_ENV", "<unset>")


if __name__ == "__main__":
    server.run()
