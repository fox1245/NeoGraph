"""A stdio MCP server built with the official MCP Python SDK.

An ACP client passes it to the agent in session/new `mcpServers`; the
interop tests then call its tools through the agent. It runs on both SDK
majors: mcp 2 renamed FastMCP to MCPServer.
"""

import os

try:
    from mcp.server.mcpserver import MCPServer as Server
except ImportError:
    from mcp.server.fastmcp import FastMCP as Server

server = Server("neograph-acp-interop")


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
