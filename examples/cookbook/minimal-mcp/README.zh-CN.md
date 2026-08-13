<!-- neograph-i18n: source=examples/cookbook/minimal-mcp/README.md locale=zh-CN source_sha256=018efba21b0004352a4b23c8947e0d18299157eb31070d941304799863f60d82 -->
# 最小 MCP — 无 fastmcp、无 SDK、无 API key

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本仓库中的其他 MCP 示例（03 / 20 / 21 / 22）都会把 MCP client 包进使用固定 OpenRouter DeepSeek 模型的 ReAct loop；大多数 MCP 教程也假设 server 侧要 `pip install fastmcp`（会拉入约 60 个包）。这掩盖了一个有用事实：

> **NeoGraph 内置 MCP client 对 peer 侧唯一的要求，是对方进程会说 wire protocol — 对自身侧唯一的要求，是 `libneograph_mcp`（已经在二进制里）。**

这个 cookbook 用尽可能小的设置证明这一点：

- **服务端**：[`min_stdio_server.py`](min_stdio_server.py) — 一个约 60 行、只用 Python 标准库的脚本。没有 `fastmcp`，没有 `mcp` SDK，不需要 pip install。它通过 stdin/stdout 使用 newline-delimited JSON-RPC，并暴露三个工具（`get_current_time`、`calculate`、`get_weather`）。
- **客户端**：[`client_harness.cpp`](client_harness.cpp) — 把 server 作为子进程启动，运行 `initialize` → `tools/list` → `tools/call`，并打印结果。**无 LLM，无 API key。**

## 运行

从 build 目录运行（用 `-DNEOGRAPH_BUILD_MCP=ON` 构建；examples 默认开启）：

```bash
./cookbook_minimal_mcp python3 ../examples/cookbook/minimal-mcp/min_stdio_server.py
```

期望输出：

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

`65537` 证明调用确实到达 server 并在那里求值 — 它不是硬编码字符串。

## 为什么这很重要

- **两边都轻量。** “batteries included” 的说法是真的：NeoGraph 静态链接 MCP，所以没有需要额外安装的包，也没有可能漂移的依赖。*Peer* server 可以小到只受标准库限制 — 这对边缘设备、CI，或只是想不引入框架就暴露几个本地工具的场景很有用。
- **不依赖 peer 实现。** 把 `min_stdio_server.py` 换成任何通过 stdio 说 MCP 的可执行文件（Go binary、Rust server、fastmcp、official SDK）。C++ 侧完全不变。
- **无 key 协议测试。** 因为 loop 中没有 LLM，这也是在接入 agent 之前，最快 smoke-test 你的 MCP server 的 `tools/list` 和 `tools/call` 形状是否正确的方法。

## 接入 agent

一旦 round-trip 正常，把 `client.get_tools()` 交给 graph node（这些 tools 是普通的 `neograph::Tool` 实例），这样 LLM 就能通过 ReAct loop 调用它们 — 这一步参见 [`examples/03_mcp_agent.cpp`](../../03_mcp_agent.cpp)。
