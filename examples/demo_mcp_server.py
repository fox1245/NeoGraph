"""NeoGraph 데모용 MCP 서버.

시간/날씨/계산 도구를 제공하는 간단한 MCP 서버.

사용법:
  pip install fastmcp
  python demo_mcp_server.py
"""

from datetime import datetime
from fastmcp import FastMCP

mcp = FastMCP("demo-tools")


@mcp.tool()
def get_current_time(timezone: str = "Asia/Seoul") -> str:
    """Get the current date and time. Returns ISO format datetime string."""
    now = datetime.now()
    return f"{now.strftime('%Y-%m-%d %H:%M:%S')} ({timezone})"


@mcp.tool()
def calculate(expression: str) -> str:
    """Evaluate a mathematical expression safely. Supports +, -, *, /, **, %, parentheses.
    Example: calculate("2 + 3 * 4") returns "14"
    """
    allowed = set("0123456789+-*/.()% ")
    if not all(c in allowed for c in expression):
        return f"Error: invalid characters in expression"
    try:
        result = eval(expression)  # noqa: S307 — safe: only digits+operators
        return str(result)
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
def get_weather(city: str) -> str:
    """Get current weather for a city (demo: returns simulated data)."""
    import random
    temp = random.randint(15, 30)
    conditions = ["맑음", "구름 조금", "흐림", "비", "안개"]
    condition = random.choice(conditions)
    return f"{city}: {temp}°C, {condition}"


if __name__ == "__main__":
    mcp.run(transport="streamable-http", port=8000)
