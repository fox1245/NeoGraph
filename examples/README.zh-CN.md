<!-- neograph-i18n: source=examples/README.md locale=zh-CN source_sha256=bdd60f74da6b396e20b442bafa8e8479ebeeced9e8bef17caf8366a89ae4bf7e -->
# C++ API 示例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

P8 切换清单：[`spec/neograph-example-disposition-v1.json`](../spec/neograph-example-disposition-v1.json)。

五十六个可运行的 C++ 程序，覆盖 NeoGraph 引擎表面。
每个示例都是此目录中的单个文件（有一个 Docker-Compose 例外，
[`26_postgres_react_hitl/`](26_postgres_react_hitl/)）— 把其中一个复制到你的项目里，
链接 `neograph::core` + `neograph::llm`，你就有了一个起点。

## 构建

默认 CMake 配置会构建已启用组件所支持的示例。Program quickstart
和基于 Program 的示例需要 `-DNEOGRAPH_BUILD_PROGRAM=ON`；gRPC 和
Python binding 是可选的，未启用对应选项时相关示例会被省略。

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

要构建完整的 C++ 示例集，还要启用 Program 和 A2A：

```bash
cmake -S . -B build \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_A2A=ON
cmake --build build -j$(nproc)
```

传入 `-DNEOGRAPH_BUILD_EXAMPLES=OFF` 可以跳过示例。需要额外依赖的示例
（Crawl4AI Docker、Postgres、MCP servers、Clay+Raylib）会由显式 CMake option
或运行时探测门控控制 — 见下方“设置”列。

## 设置

会调用真实 LLM 的示例会通过 cppdotenv 从 cwd（或任何父目录）自动加载 `.env`。
识别的两个 key 是：

```
OPENAI_API_KEY=sk-...
ANTHROPIC_API_KEY=sk-ant-...
```

下方没有“设置”条目的示例不需要 API key — 它们使用进程内 `MockProvider`
或纯 mock node。

## 从这里开始

如果这是你第一次使用：

| 首选 | 你会学到什么 |
|---|---|
| [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | **Core 快速入门** — 使用已安装的 `neograph::core` 目标、一个严格图和一个类型化通道。不需要可选组件或 API key。 |
| [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | **Program 快速入门** — 使用已安装的 `neograph::program` 目标编译、接纳并运行一个 `call_core` Program。需要 `-DNEOGRAPH_BUILD_PROGRAM=ON`。 |
| [`51_minimal.cpp`](51_minimal.cpp) | 最小可工作程序 — 构建、运行、读取 `result.channel<T>("name")`。不需要 API key。 |
| [`02_custom_graph.cpp`](02_custom_graph.cpp) | 构建 JSON 图定义并运行它。不需要 API key。 |
| [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | 使用 `make_parallel_group` 的异步扇出。不需要 API key。 |
| [`10_send_command.cpp`](10_send_command.cpp) | `Send`（动态扇出）+ `Command`（路由覆盖）。不需要 API key。 |
| [`01_react_agent.cpp`](01_react_agent.cpp) | 使用真实 LLM + calculator tool 的 ReAct 循环。**需要 `OPENAI_API_KEY`。** |
| [`14_plan_executor.cpp`](14_plan_executor.cpp) | Plan → 并行子任务 → solver，并通过 checkpoint store 做崩溃恢复。不需要 API key。 |

理解这些之后，下面其余示例按它们展示的内容分组，而不是按文件编号分组。

## 索引

### 核心引擎 — 图、状态、路由

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | 离线 | 构建 JSON 图 + 运行它。本 repo 中最短的实用程序。 |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | 离线 | 异步扇出 — 三个“researcher” node 在一个 io_context 上共同运行，summarizer 将它们扇入。 |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) | 离线 | 分层组合 — 外层 supervisor 图委托给内层 ReAct 子图。 |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | 离线 | 分类器 → 条件边 → 数学 / 翻译 / 通用专家。 |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) | 离线 | `get_state` / `update_state` / `fork` — 把 LangGraph 的 Checkpointer API 映射到 C++。 |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) | 离线 | 一个 demo 展示六个功能 — `NodeInterrupt`、`RetryPolicy`、`StreamMode`、`Send`、`Command`、`Store`。 |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) | 离线 | Planner→Send→researcher→Command(loop|finish) — 标准 Send+Command 模式。 |
| 42 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | 离线 | 从 C++ 注册自定义 channel 归约器和边条件 — 不改引擎也能扩展 JSON vocabulary。 |
| 43 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | 离线 | 在 node 内通过 `in.ctx.store` 访问跨 thread 的 `Store` — 从共享命名空间记忆得到每用户 node 行为。 |
| 51 | [`51_minimal.cpp`](51_minimal.cpp) | 离线 | 最短可工作程序 — 构建、运行、`result.channel<T>("name")`。新用户模板。 |
| 52 | [`52_export_schema.cpp`](52_export_schema.cpp) | 离线 | `NodeFactory::export_schema()` → topology JSON Schema dump。无代码可视化编辑器构建 palette 时使用的版本锁定真实来源。 |
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | 离线（可选 OpenAI） | 有界 message window — history 超出预算时，被丢弃的 prefix 会替换为 LLM 写出的 summary。默认使用 mock provider。 |

### 真实 LLM — provider、工具、ReAct

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | OpenAI | ReAct 循环：`llm_call` ↔ `tool_dispatch`，带 `has_tool_calls` 条件判断。Calculator 工具。 |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | OpenAI | 使用真实 `text-embedding-3-small` embedding + 内存余弦搜索的 RAG。 |
| 13 | [`13_openai_responses.cpp`](13_openai_responses.cpp) | OpenAI | 同一个 ReAct 循环，但通过 `SchemaProvider("openai_responses")` 接到 `/v1/responses`。一个配置开关，无需 provider 子类。 |
| 33 | [`33_openai_responses_ws.cpp`](33_openai_responses_ws.cpp) | OpenAI | 通过 WebSocket 使用 Responses API — `wss://api.openai.com/v1/responses`。在多工具 agentic 循环上延迟约低 40%。 |
| 34 | [`34_openai_responses_ws_tools.cpp`](34_openai_responses_ws_tools.cpp) | OpenAI | 遍览每个 Responses API 内置工具 — web_search、image_generation、file_search、tool_search、skills、shell。传输层级（不用 SchemaProvider）。 |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | OpenAI | 调试辅助：为一次工具调用请求转储原始 `/v1/responses` JSON envelope。故意绕过 SchemaProvider。 |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | OpenAI | 在同一个 prompt 上扫描 `reasoning_effort` 的 `{minimal, low, medium, high}` — 观察延迟 / 隐藏 CoT token / 回答质量的取舍。 |

### 推理模式

| # | 文件 | 设置 | 模式 |
|---|------|-------|---------|
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | Anthropic | Reflexion — generator ↔ critic 循环，直到 critic 说 ACCEPT（Shinn et al. 2023）。俳句约束任务。 |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | Anthropic | Tree of Thoughts — 每个深度生成 N 个候选 thought，给它们打分，保留 top-K，再展开。24 点游戏。 |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | Anthropic | Self-Ask — 对多跳推理做显式“是否需要后续问题？”分解（Press et al. 2022）。 |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | Anthropic | Researcher / Skeptic / Judge — 三个 system prompt，共享转录，由 judge 裁决。 |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | Anthropic | REWOO — planner 提交带 `#E1 / #E2` 占位符的完整 plan，worker 并行扇出工具，solver 综合结果。 |

### 持久化与 HITL

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | 离线 | 在 payment node 前 `interrupt_before`，持久化检查点，在 operator approval 后 resume。Mock provider。 |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | 离线 | Plan-and-Executor，模拟扇出中途失败 — checkpoint replay 只会重跑失败的同级任务。Pending-writes 机制实战。 |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | OpenAI WS + Postgres + Crawl4AI | 进程不连续的深度研究 HITL — PG-backed 检查点能在报告和 resume 之间的 `exit` 后存活。Docker Compose 驱动。 |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | 离线 | LangGraph 风格多轮聊天 — `resume_if_exists` 重新加载先前检查点并追加新轮次。Mock provider。 |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) | 离线 | `SqliteCheckpointStore` — 单文件持久运行，无需 server。与 InMemory/Postgres 相同的 `CheckpointStore` interface。 |

### MCP（模型上下文协议）

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | OpenAI + MCP HTTP server | 从 streamable-http MCP server 发现工具，并驱动 ReAct 循环。 |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | OpenAI + Python stdio script | 与 03 相同，但 MCP server 是通过 stdin/stdout 通信的子进程 — 没有网络栈。 |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | OpenAI + 2 servers | 一个 agent、两个 MCP server（HTTP + stdio），工具合并到同一个列表 — LLM 能透明地跨两者选择。 |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | MCP HTTP server（无 LLM） | Planner 为每次 MCP 调用发出一个 Send；`make_parallel_group` 并发运行它们。确定性 — LLM 轴上保持离线，因为 demo 中工具由手写逻辑选择。 |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | OpenAI + MCP HTTP server | 在任何 MCP 工具调用前 `interrupt_before` — operator 看到待处理工具名 + 参数，批准后 resume。 |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | OpenAI + MCP HTTP server | Operator 阅读 agent 的草稿答案并输入反馈；第二次运行会把反馈作为新的对话上下文纳入。 |

### 异步、并发、性能

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | 离线 | 三个 agent 运行通过 `engine->run_async()` 在一个 `io_context` 线程上交错运行 — 墙钟时间 ≈ 50 ms，而不是 3×50 ms。Stage-4 端到端异步。 |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | OpenAI | 外层 `asio::io_context` + `co_spawn` + `co_await engine->run_stream_async(...)` 驱动 ReAct 循环，LLM node 的 token 通过 `co_await provider->complete_stream_async(...)` 针对 `SchemaProvider("openai_responses")` streaming 到 stdout。**这正是 pre-PR-#10 会段错误的形状** — 修复后干净运行；工具往返 + 最终答案约 4s。 |
| 44 | [`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp) | 离线 | 带背压的固定 worker 池（`neograph::util::RequestQueue`）— 有界在途工作，负载下不会无界增长。 |
| 46 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | 离线 | 协作式取消 — 每个 child 使用 `CancelToken::fork()`，parent `cancel()` 会级联到所有在途 child。 |
| 47 | [`47_node_cache.cpp`](47_node_cache.cpp) | 离线 | 每 node 结果缓存，key 为 node + input — 跨运行遇到相同输入时跳过重新计算。 |
| 50 | [`50_async_tool.cpp`](50_async_tool.cpp) | 离线 | `AsyncTool` — coroutine-shaped 工具执行适配器，让工具可以 `co_await` 而不阻塞 io_context。 |

### 代理互操作 — A2A 与 ACP

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 38 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | 离线 | 把编译好的 NeoGraph 暴露为 Agent-to-Agent endpoint（HTTP、streaming SSE）。先运行这个。 |
| 37 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | 离线（需要示例 38 正在运行） | 驱动一个*远程* A2A agent — `A2ACallerNode` 让远程 agent 看起来像本地 node。 |
| 39 | [`39_acp_server.cpp`](39_acp_server.cpp) | 离线 | 通过 Agent Client Protocol 暴露 NeoGraph — stdio 上的双向 JSON-RPC，这是编辑器（Zed 风格）驱动的形状。 |

### 分布式 — gRPC 服务与远程检查点/工具

只有传入 `-DNEOGRAPH_BUILD_GRPC=ON` 才会构建（需要 `grpc++` / `protoc`）。

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 52 | [`52_grpc_server.cpp`](52_grpc_server.cpp) | 离线（grpc++） | 通过 gRPC 暴露 `GraphEngine` — 每个不同图的 engine 懒编译并缓存。 |
| 53 | [`53_grpc_client.cpp`](53_grpc_client.cpp) | 离线（grpc++） | 从 C++ 客户端调用 NeoGraph gRPC `GraphService`。 |
| 54 | [`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp) | 离线（grpc++） | `GrpcCheckpointStore` — 跨网络边界的远程 `CheckpointStore`，带诚实的延迟测量。 |
| 55 | [`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp) | 离线（grpc++） | 正面对比：JSON-RPC vs gRPC 上的工具调用 — “70× 是 Nagle artifact”背后的微基准。 |
| 57 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | 离线（grpc++） | 位于另一个进程中的工具，作为本地 `neograph::Tool` 暴露。 |

### 可观测性

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 49 | [`49_openinference.cpp`](49_openinference.cpp) | 离线 | OpenInference tracer adapter — `graph.run > node.* > llm.complete` 落成一个 trace tree（12 个 attribute）。Phoenix 已验证。Mock provider。 |

### 深度研究 / RAG 变体

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | Anthropic + Crawl4AI Docker | `langchain-ai/open_deep_research` 的 C++ port。Supervisor 规划，扇出并行 sub-researcher（每个都有自己的 ReAct 循环），综合成 markdown 报告。 |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | OpenAI | CRAG（Yan et al. 2024）。Retrieve → grade → 根据相关性路由到 refine(KB) / refine+web / web-only。Web search 通过 `/v1/responses` 内置工具。 |

### 本地 / 混合 LLM 后端

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | 本地 server（llama.cpp / vLLM） | 把 `OpenAIProvider` 指向 `http://localhost:8090`。两进程拆分让模型权重留在 agent 地址空间之外。 |

### 展示

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | Clay + Raylib (`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`) | 带 Clay/Raylib UI 的多轮聊天。纯 C++ 桌面应用，NeoGraph 后端。Mock 或 `--live`。 |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | OpenAI + Ghidra + ghidra-mcp | 逆向工程 agent — 通过 Ghidra 从 stripped binary 恢复函数名 + 摘要。端到端验证（matched_score 0.92，6-fn crackme）。完整流水线在 [`fox1245/re-agent`](https://github.com/fox1245/re-agent)。 |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | 离线 | 五个小“classifier”（情感 / 毒性 / 语言 / 主题 / 意图）通过 Send 扇出并并行运行。墙钟时间 ≈ max(per-classifier)，不是求和 — 小模型边缘故事。Mock 5 ms 延迟作为 DistilBERT/MiniLM pass 的替身；inline `[ONNX SWAP-IN]` block 展示使用 `Ort::Session` 的 30 行替换。没有推理运行时依赖。 |

## 心智模型 — 三层，中间是 JSON

每个示例都属于三种设置之一：

1. **只用内置 node**（02, 04, 07, 14）：`llm_call` / `tool_dispatch`
   / mock-provider node — 图完全由 JSON wiring，不需要 subclassing。最接近 `create_react_graph()` 产生的内容。
2. **自定义 `GraphNode` subclass**（05, 09, 10, 25）：你控制精确的
   `run(NodeInput)` body — 通过 `NodeOutput` 发出 `ChannelWrite`、`Send` 或 `Command`。
   Send 扇出与 Command 路由覆盖就在这里。
3. **面向非 OpenAI 形状的 `SchemaProvider`**（13, 15, 16, 17, 33）：
   一个 JSON schema 描述传输形状（Anthropic、Gemini、OpenAI Responses API、raw WebSocket），
   因此同一个 `Agent` / `llm_call` node 无需 subclassing 就能命中不同 endpoint。

无论哪种方式，图定义都是 JSON 形状（`std::map<std::string, json>`）
— [Python examples](../bindings/python/examples/) 中的示例 14 和 15 展示了同一个定义
如何通过 `json.dumps` 往返后再回来。

## API key 节省策略

| Provider | 示例 |
|---|---|
| `OPENAI_API_KEY` | 01, 03, 12, 13, 20, 22, 23, 24, 28, 29, 30, 33, 34, 35, 40 |
| `ANTHROPIC_API_KEY` | 15, 16, 17, 18, 19, 25 |
| local server (no key) | 31 |
| **none** | 02, 04, 05, 06, 07, 08, 09, 10, 14, 21, 27, 36, 37, 38, 39, 41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57 |

三十一个示例不需要 API key 就能运行 — 这是“试跑一下”的最低门槛。
尤其是示例 21（MCP 扇出，确定性 planner）和 27（异步并发，
用 `steady_timer` 代替 LLM 延迟）展示了不花 token 的引擎管线。
gRPC 套件（52–55, 57）也不需要 key，但需要 `-DNEOGRAPH_BUILD_GRPC=ON`
（`grpc++` / `protoc`）；56（`history_compaction`）默认使用 mock provider，
只有存在 key 时才会触碰 OpenAI。

## CMake 配置后重新运行

构建出的二进制文件位于构建目录根部，命名为 `example_<short_name>`
（例如 `example_react_agent`、`example_custom_graph`）。准确名称在每个 `.cpp`
顶部注释的 `Usage:` 下。
