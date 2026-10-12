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
def env_probe(name: str = "ACP_INTEROP_ENV") -> str:
    """Report an environment variable; the ACP client sets ACP_INTEROP_ENV through `env`."""
    return os.environ.get(name, "<unset>")


if __name__ == "__main__":
    server.run()
