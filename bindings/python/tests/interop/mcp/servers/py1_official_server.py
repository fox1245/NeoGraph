"""Official Python MCP SDK v1 server (mcp==1.30.0, FastMCP) - the most widely deployed line.

Run with the interpreter from NG_MCP1_PYTHON (a venv with requirements-mcp1.txt).

  py1_official_server.py --transport stdio|http [--port N] [--path /mcp]
                         [--stateless] [--json-response] [--record FILE] [--events FILE]
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _recorder import Recorder  # noqa: E402

import anyio  # noqa: E402
from pydantic import BaseModel  # noqa: E402

from mcp.server.fastmcp import Context, FastMCP  # noqa: E402
from mcp.server.fastmcp.exceptions import ToolError  # noqa: E402

ARGS: argparse.Namespace
_lock = threading.Lock()


def event(kind, **fields):
    if ARGS.events:
        with _lock, open(ARGS.events, "a", encoding="utf-8") as fh:
            fh.write(json.dumps({"t": time.time(), "event": kind, **fields}, default=str) + "\n")


class Weather(BaseModel):
    city: str
    celsius: float


def build(stateless: bool, json_response: bool, path: str) -> FastMCP:
    mcp = FastMCP("ng-probe-fastmcp-v1", instructions="probe server", stateless_http=stateless,
                  json_response=json_response, streamable_http_path=path)

    @mcp.tool()
    def echo(text: str) -> str:
        """Echo text"""
        return text

    @mcp.tool()
    def add(a: int, b: int) -> int:
        """Add two integers (wrapped structured output)"""
        return a + b

    @mcp.tool()
    def weather(city: str) -> Weather:
        """Structured output from a pydantic model"""
        return Weather(city=city, celsius=21.5)

    @mcp.tool()
    def fails(reason: str = "nope") -> str:
        """Raises ToolError -> isError result"""
        raise ToolError(f"tool refused: {reason}")

    @mcp.tool()
    async def chatty(ctx: Context) -> str:
        """Logs through the context before answering"""
        await ctx.info("first log")
        await ctx.info("second log")
        return "chatty done"

    @mcp.tool()
    async def ping_client(ctx: Context) -> str:
        """Server->client ping inside a tool call"""
        try:
            with anyio.fail_after(4):
                await ctx.session.send_ping()
            event("ping_client", ok=True)
            return "client answered ping"
        except TimeoutError:
            event("ping_client", ok=False, error="timeout")
            raise ToolError("client never answered ping")

    return mcp


def main():
    global ARGS
    p = argparse.ArgumentParser()
    p.add_argument("--transport", choices=["stdio", "http"], default="stdio")
    p.add_argument("--port", type=int, default=18270)
    p.add_argument("--path", default="/mcp")
    p.add_argument("--stateless", action="store_true")
    p.add_argument("--json-response", action="store_true")
    p.add_argument("--record")
    p.add_argument("--events")
    p.add_argument("--flavor", default="fastmcp")  # accepted for a uniform command line
    ARGS, _ = p.parse_known_args()
    mcp = build(ARGS.stateless, ARGS.json_response, ARGS.path)
    if ARGS.transport == "stdio":
        mcp.run("stdio")
        return
    import uvicorn

    uvicorn.run(Recorder(mcp.streamable_http_app(), ARGS.record), host="127.0.0.1", port=ARGS.port,
                log_level="warning", lifespan="on")


if __name__ == "__main__":
    main()
