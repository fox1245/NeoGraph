"""자비스 cookbook 데모용 MCP 서버.

NeoGraph 의 examples/demo_mcp_server.py 와 동일한 도구 셋(시간/날씨/계산)을 노출하되
port 를 CLI 인자로 받아 자비스 시연 시 충돌(보통 port 8000 점유) 회피.

사용:
    python3 demo_mcp_server.py            # 기본 port 8888
    python3 demo_mcp_server.py 8000       # 다른 port 지정

pip install fastmcp 필요. 자비스 cookbook README 참고.
"""

import sys
import random
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
    host = sys.argv[2] if len(sys.argv) >= 3 else "127.0.0.1"  # 컨테이너에선 0.0.0.0
    print(f"[demo_mcp_server] starting on port {port}", file=sys.stderr)
    mcp.run(transport="streamable-http", host=host, port=port)
