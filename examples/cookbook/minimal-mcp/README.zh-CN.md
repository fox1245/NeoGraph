<!-- neograph-i18n: source=examples/cookbook/minimal-mcp/README.md locale=zh-CN source_sha256=ad305d0ff700addd7966a8ce6dc03cdceaa6dd0790dd0cdcdb650b98944a5004 -->
# Minimal MCP — 无需 fastmcp 或 API key

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本仓库其他 MCP 示例（03、20 至 24）把 MCP 客户端交给由脚本化模型驱动的 ReAct 循环，而大多数 MCP 教程默认服务端要 `pip install fastmcp`（约 60 个包）。本食谱去掉循环，只展示客户端，因此下面这个事实一目了然：

> 对端只需实现 stdio MCP protocol。C++ client 链接
> `libneograph_mcp` 及其传递 native 依赖；对端不需要 Python MCP
> package。

本食谱用最小的可行设置证明了这一点：

- **服务端**：[`min_stdio_server.py`](min_stdio_server.py) — 一个约60行纯stdlib的Python脚本。没有`fastmcp`，没有`mcp` SDK，没有pip install。它通过stdin/stdout进行换行分隔的JSON-RPC传输，并暴露三个工具（`get_current_time`、`calculate`、`get_weather`）。
- **客户端**: [`client_harness.cpp`](client_harness.cpp) — 以子进程方式启动服务器，运行 `initialize` → `tools/list` → `tools/call`，并打印结果。**无 LLM，无 API 密钥。**

## 运行它

在以 `-DNEOGRAPH_BUILD_EXAMPLES=ON` 和 `-DNEOGRAPH_BUILD_MCP=ON` 配置的 build directory 运行。不调用模型也需要 SchemaProvider。使用 `CMAKE_PREFIX_PATH` 提供安装 prefix，或 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 提供 SDK source：

```bash
./cookbook_minimal_mcp python3 ../examples/cookbook/minimal-mcp/min_stdio_server.py
```

期望输出:

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

`65537` 证明该调用确实到达了服务器并在那里进行了评估——它不是一个预设的字符串。

## 为什么这很重要

- **小巧的对端。** server 只使用 Python 标准库。client 仍有包括 SchemaProvider 的 native build 依赖。将 MCP 放入 binary 不会消除这些依赖。
- **与对端无关。** 将 `min_stdio_server.py` 替换为任何通过 stdio 使用 MCP 协议的可执行程序（Go 二进制程序、Rust 服务器、fastmcp、官方 SDK）。C++ 端无需任何更改。
- **无密钥协议测试。** 由于循环中没有LLM，这也是在将MCP服务器接入智能体之前，对其`tools/list`和`tools/call`形状进行冒烟测试的最快方式。

## 将其接入智能体

一旦往返正常，将`client.get_tools()`交给一个图节点（这些工具是普通的`neograph::Tool`实例），以便LLM可以通过ReAct循环调用它们——参见[`examples/03_mcp_agent.cpp`](../../03_mcp_agent.cpp)了解该步骤。
