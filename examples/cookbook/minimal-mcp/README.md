# Minimal MCP — no fastmcp, no SDK, no API key

Every other MCP example in this repo (03 / 20 / 21 / 22) wraps the MCP
client inside a ReAct loop that needs `OPENAI_API_KEY`, and most MCP
tutorials assume you `pip install fastmcp` (which pulls ~60 packages)
on the server side. That hides a useful fact:

> **NeoGraph's built-in MCP client needs nothing on the peer side except
> a process that speaks the wire protocol — and nothing on its own side
> except `libneograph_mcp` (already in the binary).**

This cookbook proves it with the smallest possible setup:

- **Server**: [`min_stdio_server.py`](min_stdio_server.py) — a ~60-line
  pure-stdlib Python script. No `fastmcp`, no `mcp` SDK, no pip install.
  It speaks newline-delimited JSON-RPC over stdin/stdout and exposes
  three tools (`get_current_time`, `calculate`, `get_weather`).
- **Client**: [`client_harness.cpp`](client_harness.cpp) — spawns the
  server as a subprocess, runs `initialize` → `tools/list` →
  `tools/call`, and prints results. **No LLM, no API key.**

## Run it

From the build directory (built with `-DNEOGRAPH_BUILD_MCP=ON`, which is
on by default for examples):

```bash
./cookbook_minimal_mcp python3 ../examples/cookbook/minimal-mcp/min_stdio_server.py
```

Expected output:

```
[*] Spawning stdio MCP server: python3 .../min_stdio_server.py
[*] initialize OK
[*] tools/list -> 3 tools:
    - get_current_time: Get the current UTC date and time (ISO format).
    - calculate: Evaluate a simple math expression (+ - * / ** % and parens).
    - get_weather: Return deterministic demo weather for a city.

[*] tools/call round-trips:
    get_current_time({"timezone":"UTC"}) -> 2026-05-31 12:00:00 (UTC)
    calculate({"expression":"2 ** 16 + 1"}) -> 65537
    get_weather({"city":"Tokyo"}) -> Tokyo: 22C, clear (demo)

[*] 3/3 MCP tool calls succeeded (no LLM, no fastmcp)
```

The `65537` proves the call really reached the server and evaluated
there — it isn't a canned string.

## Why this matters

- **Lightweight, both sides.** The "batteries included" claim is real:
  NeoGraph links MCP statically, so there is no separate package to
  install and no dependency that can drift. The *peer* server can be as
  small as the stdlib allows — useful on edge devices, in CI, or when
  you just want to expose a couple of local tools without a framework.
- **Peer-agnostic.** Replace `min_stdio_server.py` with any executable
  that speaks MCP over stdio (a Go binary, a Rust server, fastmcp, the
  official SDK). The C++ side never changes.
- **Key-free protocol test.** Because there's no LLM in the loop, this
  is also the fastest way to smoke-test that your MCP server's
  `tools/list` and `tools/call` shapes are correct before wiring it
  into an agent.

## Wiring it into an agent

Once the round-trip works, hand `client.get_tools()` to a graph node
(the tools are ordinary `neograph::Tool` instances) so an LLM can call
them through a ReAct loop — see [`examples/03_mcp_agent.cpp`](../../03_mcp_agent.cpp)
for that step.
