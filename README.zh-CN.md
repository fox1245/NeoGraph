<!-- neograph-i18n: source=README.md locale=zh-CN source_sha256=f6016fba6c70ae3d33be9ea2b574a25b9ea984822aa371dd940da43b3b0d19dd -->
<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>C++ 图 agent 引擎——附带 Python 绑定。</strong><br>
    LangGraph 级能力 · 5&nbsp;µs 引擎开销 · 一个可装入树莓派的静态二进制文件。
  </p>
</p>

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
  <a href="#quick-start">快速入门</a> &middot;
  <a href="#use-from-a-cmake-project">CMake</a> &middot;
  <a href="#python">Python</a> &middot;
  <a href="docs/concepts.md">概念</a> &middot;
  <a href="examples/README.md">示例</a> &middot;
  <a href="docs/troubleshooting.md">故障排查</a> &middot;
  <a href="docs/reference-en.md">API 参考</a> &middot;
  <a href="https://fox1245.github.io/NeoGraph/">Doxygen</a> &middot;
  <a href="#vs-langgraph">对比 LangGraph</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo.mp4">
    <img src="docs/images/neograph-promo.gif" alt="NeoGraph 宣传片 — 5µs 引擎开销、5.5MB RSS（10K 并发）、1.2MB 静态二进制、可在树莓派上运行" width="900">
  </a>
</p>

## 什么是 NeoGraph？

NeoGraph 是一个 **C++17/20 基于图的 agent 编排引擎**，将
LangGraph 级能力带入 C++。以 JSON 定义 agent 工作流，通过并行扇出执行，
为时间旅行调试和人类参与设置检查点，并接入任何 LLM provider——全部无需
Python。

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>

auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "sk-...", .default_model = "gpt-4o-mini"
});
auto engine = neograph::graph::create_react_graph(provider, std::move(tools));

neograph::graph::RunConfig config;
config.input = {{"messages", json::array({{{"role","user"},{"content","Hello!"}}})}};
auto result = engine->run(config);
```

上述 agent 实际上只是引擎执行的 JSON——换一个 JSON 就能得到不同的
agent（参见 [`docs/concepts.md`](docs/concepts.md)）：

```json
{
  "channels": { "messages": {"reducer": "append"}, "__route__": {"reducer": "overwrite"} },
  "nodes": {
    "planner":    {"type": "llm_call"},
    "researcher": {"type": "tool_dispatch"},
    "classifier": {"type": "intent_classifier", "routes": ["deep_dive", "summarize"]}
  },
  "edges": [
    {"from": "__start__", "to": "planner"},
    {"from": "planner", "condition": "has_tool_calls",
     "routes": {"true": "researcher", "false": "classifier"}},
    {"from": "researcher", "to": "planner"},
    {"from": "classifier", "condition": "route_channel",
     "routes": {"deep_dive": "__end__", "summarize": "__end__"}}
  ]
}
```

**NeoGraph 是唯一面向 C++ 的图 agent 引擎。** 如果你正在为机器人、
嵌入式系统、游戏、高频交易或任何 Python 不可行的场景构建 agent——
这就是答案。

## 四大维度

每行只需一个命令即可验证——无需设置、无需 API 密钥（实时 LLM 变体除外）。

|   | 维度 | 实测数据 | 详情 |
|---|---|---|---|
| ⚡ | **性能** | 5 µs 引擎开销 · 10 K 并发在 5.5 MB 内 · p99 7 µs @ 10 K（1 CPU 沙箱） | [性能深度解析](docs/performance-deep-dive.md) |
| 🧬 | **自演化** | LLM 裁判 → `graph_def` 热切换 · 5 个客户 → 3 个新涌现的拓扑聚类 | [self_evolving_chatbot](examples/cookbook/self_evolving_chatbot/) |
| 🔌 | **嵌入式就绪** | 1.2 MB 精简静态二进制 · 仅需 `libc.so.6` · 可在 RPi Zero 2W 上运行 | [嵌入式 / 机器人](docs/performance-deep-dive.md#what-the-numbers-mean-for-embedded--robotics) |
| 🪶 | **轻量级** | 2 个直接 wheel 依赖 · 1 K 客户多租户 → 29 MB · 适合 t2.micro | [multi_tenant_chatbot](examples/cookbook/multi_tenant_chatbot/) |

### 基准测试

匹配拓扑、零 I/O 引擎开销——仅节点分发 + 状态写入 + reducer 调用
（µs/迭代，越低越好）：

| 框架 | `seq`（3 节点） | `par`（扇出 5） | 对比 NeoGraph |
|---|--:|--:|--:|
| **NeoGraph master** | **5.0 µs** | **11.8 µs** | 1× |
| Haystack 2.28 | 144 µs | 290 µs | 29× |
| pydantic-graph 1.85 | 236 µs | 286 µs | 47× |
| LangGraph 1.1.9 | 657 µs | 2,349 µs | 131× |
| LlamaIndex 0.14 | 1,780 µs | 4,684 µs | 356× |
| AutoGen 0.7.5 | 3,209 µs | 7,293 µs | 642× |

在 N=10,000 并发（1 CPU / 512 MB 沙箱）下：NeoGraph 52 ms / 7 µs p99 /
5.5 MB · LangGraph 23.4 s / 416 MB · LlamaIndex 与 AutoGen 被 OOM 终止。
完整矩阵 + 方法论：[`docs/performance-deep-dive.md`](docs/performance-deep-dive.md)
· [`benchmarks/README.md`](benchmarks/README.md)。

## 快速入门

**要求** — C++20 编译器（GCC 13.3 核心通过；GCC 14.2+ / Clang 18+ /
MSVC 2022 全覆盖），CMake 3.16+，Python 3（构建时代码生成）。使用默认
选项时，配置步骤还需要 OpenSSL、SQLite3、libpq 和 libcurl 的**开发**
包（仅运行时 `.so` 文件无法满足 `find_package`）：

```bash
# Ubuntu / Debian
sudo apt install libssl-dev libsqlite3-dev libpq-dev libcurl4-openssl-dev
# macOS (SQLite ships with the system)
brew install openssl libpq curl
```

不需要 Postgres / SQLite 检查点或 HTTP/2 后端？跳过这些包并改用
`-DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF
-DNEOGRAPH_USE_LIBCURL=OFF` 配置。

**平台** — Linux x86_64 **GA**（参考，429/429 ctest，消毒器干净）；
macOS arm64、Linux ARM64、Windows MSVC 2022 **beta**。各平台依据见
[`CHANGELOG.md`](CHANGELOG.md)。

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build
cmake --build build -j$(nproc)

# Run an example — no API key needed:
./build/example_custom_graph      # mock ReAct agent
./build/example_parallel_fanout   # parallel fan-out/fan-in
./build/example_send_command      # dynamic Send + Command routing
```

对真实 LLM 运行——每个使用 API 的示例会自动从当前目录加载 `.env`
（内嵌 `cppdotenv`）：

```bash
echo "OPENAI_API_KEY=sk-..." > .env
./build/example_react_agent
```

## 从 CMake 项目中使用

`pip install` 仅适用于 Python（不含 C++ 头文件）。对于 C++，
`FetchContent` 的使用方式类似 CMake 的 `pip install`：

```cmake
include(FetchContent)
FetchContent_Declare(NeoGraph
    GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
    GIT_TAG        master)
# Optional: trim heavy components you don't need.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NEOGRAPH_BUILD_PYBIND   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(NeoGraph)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE neograph::core neograph::llm neograph::a2a)
```

这就是全部集成。初次接触？[**最常见的 5 个前 30 分钟陷阱**](docs/troubleshooting.md)
（通道访问器形式、`neograph::graph::` 子命名空间、`<httplib.h>` OpenSSL 宏、
GCC 13 协程 ICE 等）将为你节省一次调试会话。完整构建选项和 CMake 目标：
[`docs/reference-en.md`](docs/reference-en.md)。

## Python

相同的 C++ 引擎，可通过 `pip` 安装并在 Notebook、Gradio 或
FastAPI 服务中驱动：

```bash
pip install neograph-engine
```

```python
import neograph_engine as ng

definition = {
    "name": "demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"llm": {"type": "llm_call"}},
    "edges":    [{"from": ng.START_NODE, "to": "llm"},
                 {"from": "llm", "to": ng.END_NODE}],
}
engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"messages": [...]}))
```

每次发布 20 个 wheels + sdist（Linux x86_64/aarch64、macOS arm64、
Windows x64 · Python 3.9–3.13）。完整指南 — 使用真实 LLM 的 ReAct、
异步、自定义 reducer、LangGraph 差异列表、可观测性、免 Docker 部署：
[`docs/python-binding.md`](docs/python-binding.md)。

## 功能特性

**核心引擎（`neograph::core`）** — JSON 定义的图（无需重新编译即可更改
工作流）· Pregel 超级步骤执行（带循环）· 并行扇出/扇入 ·
`Send`（动态扇出）+ `Command`（路由+状态覆盖）· 检查点 +
HITL（`interrupt_before/after`、`resume()`、`NodeInterrupt`）·
`get_state` / `update_state` / `fork` / 时间旅行 · 重试策略 ·
流模式 · 子图 · 意图路由 · 跨线程 `Store` · 通过 `NodeFactory` 的自定义
节点 · 原生异步（`run_async` / `run_stream_async`）· 协作式
`CancelToken` · 历史压缩 · 每节点缓存 ·
`NodeFactory::export_schema()`（驱动版本锁定的可视化编辑器）。
内置 **OpenInference 追踪器**，无需额外链接。

**LLM Provider（`neograph::llm`）** — `OpenAIProvider`（OpenAI/Groq/
Together/vLLM/Ollama——任何兼容 OpenAI 的 API）· `SchemaProvider`
（通过 JSON Schema 支持 Claude、Gemini 或任意自定义供应商）·
ReAct `Agent` 循环（带流式）。

**集成** — MCP 客户端（`neograph::mcp`，HTTP + stdio）· 本地 MCP 服务器
（`neograph::mcp_server`，stdio）· 可选的 Streamable HTTP 服务器
（`neograph::mcp_http_server`）· SQLite Harness 记录
（`neograph::mcp_sqlite`）· 编译器驱动的多工作器
[Harness MCP](docs/HARNESS_MCP.md) · Agent 到 Agent
（`neograph::a2a`，服务器 + 客户端 + 调用者节点）· Agent 客户端协议
（`neograph::acp`，编辑器驱动）· gRPC 服务（`neograph::grpc`，可选）·
异步 HTTP/HTTPS/WS + SSE（`neograph::async`）。

**持久状态** — `PostgresCheckpointStore`、`SqliteCheckpointStore` 和
`InMemoryCheckpointStore` 均在一个 `CheckpointStore` 接口之后（全部支持
Python 绑定），外加用于不可变 Harness 工件和可重启运行记录的
`SqliteHarnessRecordStore`。`neograph::util` 中的无锁 `RequestQueue` +
`AsyncTool`。

`NEOGRAPH_BUILD_MCP` 仍然是两个 MCP 角色的兼容总开关。
使用 `NEOGRAPH_BUILD_MCP_CLIENT` 或 `NEOGRAPH_BUILD_MCP_SERVER` 进行
窄化构建；仅 stdio 服务器的目标不需要 `neograph::async` 或
OpenSSL。通过 `NEOGRAPH_BUILD_MCP_HTTP_SERVER` 显式启用远程 HTTP。

完整能力列表及 55+ 个可运行示例：
[`examples/README.md`](examples/README.md)。

## 架构

`GraphEngine` 是一个轻量级的超级步骤编排器，委托给四个专门构建、
各自独立单元测试的类：

- **`GraphCompiler`** — 纯 `JSON → CompiledGraph` 解析器。
- **`Scheduler`** — 信号分发路由 + 屏障累积。
- **`NodeExecutor`** — 重试循环、并行扇出（`asio::make_parallel_group`）、`Send` 分发。
- **`CheckpointCoordinator`** — 保存 / 恢复 / pending-writes 在 `(store, thread_id)` 门面之后。

`neograph::core` 无网络依赖（`yyjson` + 仅头文件的 `asio`）；
`httplib` 对 `llm`/`mcp` 保持 PRIVATE，从不暴露给用户代码。开箱即用提供
两种并发模型——每个 agent 一个线程（同步）和协程异步（数千个 agent 在
一个 `asio::io_context` 上）。详情：
[`docs/reference-en.md` §7b](docs/reference-en.md#7b-engine-internals) ·
[`docs/concurrency.md`](docs/concurrency.md) · [`docs/ASYNC_GUIDE.md`](docs/ASYNC_GUIDE.md)。

## 对比 LangGraph

| | LangGraph（Python） | NeoGraph（C++） |
|---|---|---|
| 引擎 | StateGraph | GraphEngine |
| 检查点 / HITL / fork / 时间旅行 | 是 | 是（+ `NodeInterrupt`） |
| 并行扇出 | 静态 | `make_parallel_group`（+ 可选 `asio::thread_pool`） |
| Send / Command | 是 | `NodeResult::sends` / `::command` |
| 多 LLM | 需要 LangChain | `SchemaProvider` 内置（3 个供应商） |
| MCP | 独立实现 | 内置 |
| 运行时 / 内存 | Python GIL · ~300 MB+ | C++20 协程 + asio · ~10 MB |
| 边缘 / 嵌入式 | 不可行 | 树莓派、Jetson、IoT |

对于 LangGraph 需要"每个客户一个进程"的相同多租户形态（StateGraph
是一个 Python 对象），NeoGraph 以"图即 JSON"从一个进程提供服务——
[多租户](examples/cookbook/multi_tenant_chatbot/)和
[自演化](examples/cookbook/self_evolving_chatbot/) Cookbook 展示了原因。

## 致谢

灵感来源于 [LangGraph](https://github.com/langchain-ai/langgraph)、
[agent.cpp](https://github.com/mozilla-ai/agent.cpp)、
[asio](https://think-async.com/Asio/)（3.0 引擎运行时）和
[Clay](https://github.com/nicbarker/clay)。

## 许可证

MIT — 见 [LICENSE](LICENSE)。第三方：[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
