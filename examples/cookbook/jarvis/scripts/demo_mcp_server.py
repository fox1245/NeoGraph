"""Demo MCP server for the JARVIS cookbook.

Exposes the same tool set (time/weather/calculator) as NeoGraph's
examples/demo_mcp_server.py, but accepts the port as a CLI argument to avoid
conflicts (typically on port 8000) during JARVIS demos.

Usage:
    python3 demo_mcp_server.py            # default port 8888
    python3 demo_mcp_server.py 8000       # custom port

Requires `pip install fastmcp`. See the JARVIS cookbook README.
"""

import sys
import random
from datetime import datetime
from zoneinfo import ZoneInfo
from fastmcp import FastMCP

mcp = FastMCP("demo-tools")


@mcp.tool()
def get_current_time(timezone: str = "Asia/Seoul") -> str:
    """Get the current date and time. Returns ISO format datetime string."""
    now = datetime.now(ZoneInfo(timezone))
    return f"{now.strftime('%Y-%m-%d %H:%M:%S')} ({timezone})"


@mcp.tool()
def calculate(expression: str) -> str:
    """Evaluate a mathematical expression safely. Supports +, -, *, /, **, %, parentheses.
    Example: calculate("2 + 3 * 4") returns "14"
    """
    allowed = set("0123456789+-*/.()% ")
    if not all(c in allowed for c in expression):
        return "Error: invalid characters in expression"
    try:
        return str(eval(expression))  # noqa: S307 — safe: only digits+operators
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
def get_weather(city: str) -> str:
    """Get current weather for a city (demo: returns simulated data)."""
    temp = random.randint(15, 30)
    condition = random.choice(["맑음", "구름 조금", "흐림", "비", "안개"])
    return f"{city}: {temp}°C, {condition}"


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) >= 2 else 8888
    host = sys.argv[2] if len(sys.argv) >= 3 else "127.0.0.1"  # 0.0.0.0 inside a container
    print(f"[demo_mcp_server] starting on port {port}", file=sys.stderr)
    mcp.run(transport="streamable-http", host=host, port=port)
