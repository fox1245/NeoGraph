<!-- neograph-i18n: source=bindings/python/examples/README.md locale=zh-CN source_sha256=9e936b9adcdabe02b5173ddcdbea7246c3329915e68958acd7c3726c8e1ad55e -->
# Python API 示例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

二十八个脚本，端到端覆盖 binding surface。

## 设置

```bash
pip install neograph-engine python-dotenv
cp .env.example .env  # edit OPENAI_API_KEY for examples that hit a real LLM
```

`_common.py` 会从本目录或任意父目录自动加载 `.env`。需要 API key 的 examples 在缺少 key 时会干净跳过（不会崩溃）。

## 索引

| 编号 | 文件 | 网络 | 模式 |
|---|------|---------|---------|
| 01 | [`01_minimal.py`](01_minimal.py) | offline | `GraphNode` 子类 + `engine.run()`。最小有用图。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | offline | `Tool` 子类 + 内置 `tool_dispatch`。手写 tool_call（无真实 LLM）。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | offline | `run(input)` 返回带 `Send` list 的 `NodeResult` + `set_worker_count(4)`。Map-reduce。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | offline | `engine.run_async` + 8 个并发 runs 的 `asyncio.gather` + `run_stream_async`。 |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | **OpenAI** | `OpenAIProvider` + 内置 `llm_call` node。One-shot completion。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | **OpenAI** | ReAct loop：`llm_call` ↔ `tool_dispatch`，带 `has_tool_calls` conditional。 |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | 离线 | 带 mock LLM emitter 的两阶段 propose/approve workflow。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | **OpenAI** | 分类器节点 + 条件边 → 数学 / 翻译 / 通用专家。 |
| 09 | [`09_state_management.py`](09_state_management.py) | offline | `set_checkpoint_store(InMemoryCheckpointStore())` + `get_state` + `fork`。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) | offline | `run(input)` 返回 `Command(goto_node=…, updates=[…])`。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | **OpenAI** | Actor + critic loop，带反思 prompt（Shinn et al. 2023）。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | **OpenAI** | Self-Ask 后续问题分解（Press et al. 2022）。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | **OpenAI** | 两个 debaters + judge。Debaters 通过 `Send` fan out。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | offline | 将 graph definition 序列化到 `.json` 文件。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | offline | 加载 `.json` graph 并运行（14 的 companion）。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | **OpenAI WS** | Multi-turn Gradio chat，在 `조사해줘 / research / investigate` 时切换到并行 deep-research subgraph。使用 `SchemaProvider("openai_responses", use_websocket=True)`。需要 `pip install gradio`。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | **OpenAI WS + Crawl4AI + Postgres** | 与 16 相同的 chat shape，但 researchers 会通过本地 Crawl4AI container（`docker run unclecode/crawl4ai`）实际搜索 web，状态持久化在 Postgres（`PostgresCheckpointStore`）。二者都可通过 env vars 选配；缺失时会优雅 fallback。Postgres 路径需用 `-DNEOGRAPH_BUILD_POSTGRES=ON` source-build。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | **OpenAI** | `engine.set_node_cache_enabled("ask", True)` — 同一输入的第二次运行会在 0 ms 内 replay cached `NodeResult`，不调用 LLM。Stats 通过 `engine.node_cache_stats()` 查看。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | offline | `from neograph_engine import message_stream` — 包装 callback，使 `LLM_TOKEN` events 以 LangChain-shape message dicts（`{role, content, content_so_far, node, metadata}`）到达。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | offline | `from neograph_engine.tracing import otel_tracer` — 将 engine events bridge 到 OpenTelemetry spans。自带 ConsoleSpanExporter；可替换为 OTLP 发送到 Jaeger / Tempo / Honeycomb / Datadog。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | **OpenAI** | `SchemaProvider(..., prefer_libcurl=True)` — opt-in HTTP/2（libcurl）transport，对比默认 ConnPool（HTTP/1.1 keep-alive）。在 5-way parallel burst 上 A/B 两者，并打印哪个在 YOUR endpoint 上更快。默认 ConnPool 在 api.openai.com 上更快；当需要 CF-WAF compatibility、经 corporate proxies 降低 TCP fan-out，或需要 HTTP/3 时再切换。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | **OpenAI** | Goal-driven self-evolution：agent 运行，对照 JSON-shape goal 评分其输出，并请求 LLM 提出修订后的 graph definition。当 score ≥ 1.0 或达到 max_iters 时 loop 结束。演示 JSON-as-program，其中 modifier 的唯一输出是新的 graph spec。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | offline (mock) / **OpenAI** | Per-thread evolving chat agent：持久 multi-turn conversation；每轮之间，agent 的 JSON definition 会基于累计 history 重写。演示 evolution 期间的 checkpoint-resume（prior messages 保留）、`__graph_meta__` audit channel pattern，以及 validator boundary（whitelist node types、required channels、edge connectivity）。通过 deterministic mock provider + heuristic mock evolver，无 API key 也可端到端运行。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | offline | The tool gate (#89)：每次 tool call 在 **任何 tool 运行前** 都会咨询 `engine.set_tool_gate(...)`，返回 Allow / Allow-with-rewritten-args / Deny / Interrupt。展示 canonical approval prompt — *"the agent wants to run `rm -rf build/`. Allow?"* — 以及关键点：当 human 决策时，无害的 sibling call **尚未** 运行，因此拒绝确实意味着什么都没发生，批准也不会重新运行它。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | offline | Concurrent tools (#96)：使用 `ng.AsyncTool` 而不是 `ng.Tool`，三个 300 ms tools 用 0.30 s 而不是 0.90 s。同一次运行中也测量边界 — 三个 *CPU-bound* tools 花费单个 tool 的 3.2x 时间，因为 Python function 运行时持有 GIL，线程再多也改变不了。Concurrency 是 opt-in，因此现有 stateful tool 不会突然与自己 race。 |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | offline | MCP (#95)：`ng.mcp.MCPClient(url).get_tools()` 拉取 remote tool catalogue，并直接交给 `NodeContext`。它会启动自己的 MCP server，因此无网络也能运行。重复的同名调用默认串行；线程化 demo 仅通过 `ToolExecutionPolicyRegistry` 显式将 `fetch` 标为 Reentrant，然后测量三个 0.4 s HTTP calls 在 0.41 s 内完成。stdio 也会多路复用 JSON-RPC ID，但重叠同时需要该 host policy 和并发 server。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | localhost | A2A hosting (#120)：官方 `a2a-sdk` 拥有 JSON-RPC、task state、agent card 和 cancellation。`ProtocolHostAdapter.stream()` 将 engine token events 映射为 chunked A2A artifacts，同时保留 checkpoint context。需要 Python 3.10+ 和 `pip install "neograph-engine[a2a]"`。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | stdio | ACP hosting (#120)：stream token updates，为 graph 保留 text/image/audio/resource content blocks，并在设置 `NEOGRAPH_ACP_POSTGRES_URL` 或 `NEOGRAPH_ACP_SQLITE_PATH` 时支持 durable `session/load`。需要 Python 3.10+ 和 `pip install "neograph-engine[acp]"`。 |

## 为什么 hosting 使用官方 SDKs

C++ library 有自己的 `A2AServer` 和 `ACPServer`，但直接暴露这些类会给 Python 用户第二套 protocol implementation，其集成弱于 Python 官方 SDKs。尤其是，官方 SDKs 已经拥有当前 wire-format compatibility、server transports、task 或 session lifecycle，以及 asyncio cancellation。NeoGraph 只提供这些 SDKs 无法提供的部分：一次带 checkpoint 感知的 C++ graph engine 调用。

| Item | Decision |
|------|----------|
| C++ feature that appears missing | `A2AServer`、`ACPServer` 及其 lifecycle methods 没有镜像为 Python classes。 |
| Python alternative | 官方 `a2a-sdk` 1.x 和 `agent-client-protocol` 0.11.x server runtimes。 |
| NeoGraph integration | `ProtocolHostAdapter` 将 protocol conversation IDs 映射到 `RunConfig.thread_id`，启用 `resume_if_exists`，stream `LLM_TOKEN` events，接受自定义 JSON-safe input payloads，并取消 active asyncio task。 |
| Dependency policy | 两个 SDKs 都是 optional，因为它们需要 Python 3.10+，而 `neograph-engine` 支持 Python 3.9。安装 `neograph-engine[a2a]`、`neograph-engine[acp]` 或 `neograph-engine[protocols]`。 |
| Durable ACP sessions | 为 wheel-supported durable backend 设置 `NEOGRAPH_ACP_POSTGRES_URL`。使用 `NEOGRAPH_BUILD_SQLITE=ON` 的 source builds 可设置 `NEOGRAPH_ACP_SQLITE_PATH`。agent 只有在配置其中之一时才宣告 `session/load`；新 session 在第一个 completed prompt 创建 checkpoint 后才可 load。Session IDs 是 server-generated capabilities，checkpoints 使用私有 `acp:` thread namespace。每个 session 保持一个 active agent process；checkpoint stores 不会跨进程序列化 concurrent writers。 |
| Current limit | ACP editor callbacks（`fs/read_text_file`、terminal calls、permission prompts）目前还不能从共享 NeoGraph Python tool 安全调用：当前 `AsyncTool` 在 worker thread 上运行同步函数，并不携带当前 protocol session ID。伪造 bridge 会冒着调用错误 editor session 的风险。 |
| Revisit direct bindings when | 用户必须在 Python 中嵌入确切的 C++ server，或者官方 SDK 路径无法保留所需的 NeoGraph cancellation、checkpoint、tracing 或 tool-call behavior。 |

`ProtocolHostAdapter.run_payload()` 会把任意 JSON-safe value 传给配置的 `input_builder`。默认 `message_input` 会把 rich content blocks 保持为 user message 的 `content`；如果 graph 的 provider 期望其他形状，应传入自定义 builder。`ProtocolHostAdapter.stream()` 先 yield `ProtocolStreamEvent(kind="token", ...)` values，最后恰好 yield 一个 final event。除非 `stream_node` 命名了其 tokens 正好构成最终答案的 graph node，否则 live tokens 会被禁用。这防止 planner/tool-node output 泄漏到 protocol response。asyncio consumer queue 有界（默认 1,024 chunks）；溢出会取消 engine run。Native stream events 会先被调度到 asyncio loop，因此这个 queue 是慢 protocol transport 的 backpressure，而不是对 unbounded native producer 的硬性进程级 memory cap。

运行任意一个：

```bash
python 01_minimal.py
```

## 心智模型

从 Python 看，NeoGraph 像从 Python 看 LangGraph：由 nodes、带 reducers 的 channels、通过 `Send` 的 dynamic fan-out、通过 `Command` 的 routing overrides、通过命名 conditions（`route_channel`、`has_tool_calls` 等）的 conditional edges 组成的 graph。相同 primitives，相同 JSON-shaped graph definition。差异在于运行它的东西 — 一个 C++ engine，以每 step 微秒级完成 super-step loop、scheduling 和 checkpointing，而不是 LangGraph 的约 ~600 µs。

examples 中反复出现三种模式：

1. **Python custom nodes**（01, 03, 04, 07, 09, 10, 11, 12, 13）继承 `neograph_engine.GraphNode` 并实现 `run(input)`。从 `input.state` 读取 channels，在存在时使用 `input.stream_cb`，并返回 writes、`Command`、`Send` 或 `NodeResult`。engine 在 GIL handling 下 dispatch 到 Python，因此并发 custom nodes 不会 deadlock。

2. **Python tools**（02, 06, 07）继承 `neograph_engine.Tool`，并把实例传入 `NodeContext(tools=[…])`。engine 在 compile time 接管所有权；之后可以丢弃 Python references。

3. **Async**（04）— 每个 `*_async` binding 都返回绑定到调用线程 running loop 的 `asyncio.Future`。Stream callbacks 通过 `loop.call_soon_threadsafe` 跳到 loop thread，因此你的 `cb(ev)` 会在 asyncio 期望的位置运行。

## Graph 定义是 JSON

`GraphEngine.compile(definition, ctx)` 接受你在代码中构造的 Python `dict`，或从文件 `json.loads()` 得到的 `dict` — 形状相同。Examples 14 + 15 展示 round-trip。Custom node *types* 仍需在代码中注册（Python classes 不能编码成 JSON），但 wiring — channels、nodes by type、edges、conditional edges — 是数据。

## 发行版名称 vs. import 名称

PyPI package 是 **`neograph-engine`**（裸 `neograph` 名称已被 PyPI 上无关项目占用）。Python import name 是 `neograph_engine`：

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider
```
