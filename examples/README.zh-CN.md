<!-- neograph-i18n: source=examples/README.md locale=zh-CN source_sha256=ce7e094fabd0d6304f5961a14e5ca75acbfa89a345a01c74939bf55595ae91f6 -->
# C++ API 示例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 类型化 C++ 迁移状态

当前 C++ recipe 使用五个类型化 SDK family 的拥有所有权的 `ProviderRequest`、
有序 `sp::Event` 和不可变 `std::shared_ptr<const sp::Outcome>`（`Completion`/`Failure`）。
`ChatMessage`/`ChatTool` 是 portable 投影，不是 native replay 权限。
只 prepare 一次；durable 调用者将 claim/receipt 绑定到
`Provider::request_digest(prepared)`，并 dispatch 同一个 handle。
不要从打印的 JSON 重建 history，也不要把 failure 简化为最终文本。

canonical persistence 使用 `provider-message-v2`/`runtime-history-record-v2`；
portable 摘要不能替代 native record。optional control 由调用者选择，不暗中 clamp。
bounded call 需要真实 model fact；缺失为 `LimitUnknown`。
reservation/charged/held 与 nullable provider usage 独立，不更新 budget，也不是价格、forecast 或 invoice。

header-only `examples/provider_example_support.h` 使用真实 SDK runtime。
所有 Core 构建，包括 `NEOGRAPH_BUILD_LLM=OFF`，都需要 `SchemaProvider::runtime`。
CMake 3.20+ 按显式 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、已安装 runtime package、
固定 public GitHub source archive 的顺序选择 SDK。download fallback 默认启用；
使用已安装 package 或显式 source 的 offline 构建应设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。
安装 include root 是 `include/SchemaProvider`；执行 interface/capability 检查，
SDK package 为 alpha `0.1.1`（interface/shared ABI 4）。

保留的 interface-3 model-free C++ E2E 执行验证了39个编号 target：29个 finite offline target 与实际
MCP/ACP/A2A/Harness 和 gRPC graph/checkpoint/tool 路径。gRPC-vs-JSON-RPC 测量示例也运行了，
但不检查返回值，不计为 behavioral E2E pass。22个 live/外部模型路径与禁用的 Clay GUI 未验证，
没有调用公开 vendor 或授予新 grant。历史测量不是新迁移的 qualification；live 需要密钥、
network、model access 并产生费用。密钥、prompt、artifact 保持私密，不要把敏感 envelope/native
输出发布到公开 log。native archive 是 owner-private 认证 custody，不是加密或 vendor issuer 认证。

当前 interface-4 证据单独记录：Linux x86_64 已执行 native research recovery、
ToT/Forge/rewrite helper 和 A2A 0.3/1.0 peer；11个已安装 Python application 在分批运行中
完成48个无需凭据的 localhost 请求。已跟踪 evolution file 模式与 Plan resume 也按下文执行。
这些是限定范围的 local 运行，不是保留的完整 C++ suite 的重跑。
参见 [SDK interface-4 执行记录](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)。
不声称新的 hosted vendor、Windows/macOS/ARM64、HTTP/3 或 sanitizer 验证；
remote CI 与公开发布仍待完成。



编号示例涵盖 NeoGraph 引擎 API，包括 Core 和 Program quickstart。
大多是此目录中的单个文件；[`26_postgres_react_hitl/`](26_postgres_react_hitl/) 使用 Docker Compose。
将示例复制到项目中，并链接 `neograph::core` 与所需的其他组件。

## 构建

默认 CMake 配置会构建已启用组件所支持的示例。Program quickstart
和基于 Program 的示例需要 `-DNEOGRAPH_BUILD_PROGRAM=ON`；gRPC 和
Python binding 是可选的，未启用对应选项时相关示例会被省略。

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

要包含基于 Program 的示例和 A2A 示例，还需启用以下组件：

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
所有 live 示例使用同一个 key：

```
OPENROUTER_API_KEY=sk-or-...
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
| [`01_react_agent.cpp`](01_react_agent.cpp) | 使用真实 LLM + calculator tool 的 ReAct 循环。**需要 `OPENROUTER_API_KEY`。** |
| [`14_plan_executor.cpp`](14_plan_executor.cpp) | 计划 → 并行子任务 → 求解器，并通过 checkpoint store 实现崩溃恢复。不需要 API key。 |

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
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | 离线（可选 OpenRouter） | 有界 message window — history 超出预算时，被丢弃的 prefix 会替换为 LLM 写出的 summary。默认使用 mock provider。 |
### 真实 LLM — provider、工具、ReAct

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | OpenRouter | ReAct 循环：`llm_call` ↔ `tool_dispatch`，带 `has_tool_calls` 条件判断。Calculator 工具。 |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | OpenRouter | 使用 OpenRouter 兼容 embedding + 内存余弦搜索的 RAG。 |
| 13 | [`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp) | OpenRouter | 类型化 Responses SSE 请求、有序 `sp::Event` 观察与拥有所有权的 `sp::Outcome`。 |
| 34 | [`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp) | OpenRouter | 保留全部七个类型化 hosted-tool SSE 部分，以及完整 Outcome 和有序 wire 观察。 |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | OpenRouter | 调试辅助：为一次工具调用请求转储原始 `/api/v1/responses` JSON envelope。 |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | OpenRouter | 扫描固定 DeepSeek 模型的 reasoning effort，观察延迟 / reasoning token 取舍。 |

### 推理模式

| # | 文件 | 设置 | 模式 |
|---|------|-------|---------|
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | OpenRouter | Reflexion — generator ↔ critic 循环，直到 critic 说 ACCEPT（Shinn et al. 2023）。俳句约束任务。 |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | OpenRouter | Tree of Thoughts — 每个深度生成 N 个候选 thought，给它们打分，保留 top-K，再展开。24 点游戏。 |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | OpenRouter | Self-Ask — 对多跳推理做显式“是否需要后续问题？”分解（Press et al. 2022）。 |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | OpenRouter | Researcher / Skeptic / Judge — 三个 system prompt，共享转录，由 judge 裁决。 |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | OpenRouter | REWOO — planner 提交带 `#E1 / #E2` 占位符的完整 plan，worker 并行扇出工具，solver 综合结果。 |

### 持久化与 HITL

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | 离线 | 在 payment node 前 `interrupt_before`，持久化检查点，在 operator approval 后 resume。Mock provider。 |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | 离线 | Plan-and-Executor，模拟扇出中途失败 — checkpoint replay 只会重跑失败的同级任务。Pending-writes 机制实战。 |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | OpenRouter + Postgres + Crawl4AI | 进程不连续的深度研究 HITL — PG-backed 检查点能在报告和 resume 之间的 `exit` 后存活。Docker Compose 驱动。 |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | 离线 | LangGraph 风格多轮聊天 — `resume_if_exists` 重新加载先前检查点并追加新轮次。Mock provider。 |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) | 离线 | SQLite `:memory:` checkpoint/resume 与 thread 隔离；不声称文件或 process restart 持久性。 |

### MCP（模型上下文协议）

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | OpenRouter + MCP HTTP server | 从 streamable-http MCP server 发现工具，并驱动 ReAct 循环。 |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | OpenRouter + Python stdio script | 与 03 相同，但 MCP server 是通过 stdin/stdout 通信的子进程 — 没有网络栈。 |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | OpenRouter + 2 servers | 一个 agent、两个 MCP server（HTTP + stdio），工具合并到同一个列表 — LLM 能透明地跨两者选择。 |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | MCP HTTP server（无 LLM） | Planner 为每次 MCP 调用发出一个 Send；`make_parallel_group` 并发运行它们。确定性 — LLM 轴上保持离线，因为 demo 中工具由手写逻辑选择。 |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | OpenRouter + MCP HTTP server | 在任何 MCP 工具调用前 `interrupt_before` — operator 看到待处理工具名 + 参数，批准后 resume。 |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | OpenRouter + MCP HTTP server | Operator 阅读 agent 的草稿答案并输入反馈；第二次运行会把反馈作为新的对话上下文纳入。 |

### 异步、并发、性能

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | 离线 | 三个 agent 运行通过 `engine->run_async()` 在一个 `io_context` 线程上交错运行 — 墙钟时间 ≈ 50 ms，而不是 3×50 ms。Stage-4 端到端异步。 |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | OpenRouter | 使用类型化 provider 事件的异步 ReAct。text delta 是显示投影，不是 native history。 |
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
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | OpenRouter DeepSeek + Crawl4AI Docker | `langchain-ai/open_deep_research` 的 C++ port。Supervisor 规划，扇出并行 sub-researcher（每个都有自己的 ReAct 循环），综合成 markdown 报告。 |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | OpenRouter | CRAG（Yan et al. 2024）。Retrieve → grade → 根据相关性路由到 refine(KB) / refine+web / web-only。Web search 通过 `/api/v1/responses` 内置工具。 |

### 本地 / 混合 LLM 后端

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | llama.cpp / vLLM | 位于 `http://localhost:8090` 的类型化 Chat 客户端；模型权重位于代理进程之外。 |

### 展示

| # | 文件 | 设置 | 展示内容 |
|---|------|-------|---------------|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | Clay + Raylib (`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`) | 带 Clay/Raylib UI 的多轮聊天。纯 C++ 桌面应用，NeoGraph 后端。Mock 或 `--live`。 |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | OpenRouter + Ghidra + ghidra-mcp | 逆向工程 agent，通过 Ghidra 从去除符号的二进制恢复函数名和摘要。历史端到端结果为：6 个函数的 crackme，matched_score 0.92。完整流水线在独立的私有仓库 `fox1245/re-agent` 中维护。 |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | 离线 | 五个小“classifier”（情感 / 毒性 / 语言 / 主题 / 意图）通过 Send 扇出并并行运行。墙钟时间 ≈ max(per-classifier)，不是求和 — 小模型边缘故事。Mock 5 ms 延迟作为 DistilBERT/MiniLM pass 的替身；inline `[ONNX SWAP-IN]` block 展示使用 `Ort::Session` 的 30 行替换。没有推理运行时依赖。 |

示例 35 要求通过 `GHIDRA_MCP_BRIDGE` 指定 `bridge_mcp_ghidra.py` 脚本路径。`GHIDRA_MCP_PYTHON` 选择解释器，默认值为 `python3`；`GHIDRA_SERVER_URL` 选择插件端点，默认值为 `http://127.0.0.1:18080/`。运行前须启动 Ghidra 及其 MCP 插件。此示例还需要 `OPENROUTER_API_KEY`，并调用付费模型。上面的分数是历史观测值，不是新的运行结果，也不是普遍的准确率保证。

## 保留的示例契约

这些源码契约使用 SDK interface 4。上面的 C++ 运行记录是历史证据，不是 interface-4
验证或新的 live 调用。各 family 控制是 `ProviderControls` 的封闭类型化字段；
不支持的 family/origin/model 组合在 I/O 前拒绝。reasoning/sampling/tool 控制、
Responses 服务端保存 cursor、deployment header 和显式 portable Gemini history 见
[provider reference](../docs/reference-en.md)。cursor 与 portable history 不授予 native replay 权限。

### 研究与慢推理路径

Deep Research (25 / 26) 对 supervisor、researcher、compression、final-report 的每个请求，
仅在已完成的空 `MaxTokens` outcome 没有 visible text，也没有有效或无效 client tool call 时，
允许最多两次额外 semantic call。输出 cap 加倍，但不超过16,384。research-brief 调用不属于此
ladder。每次额外调用使用新 ordinal，通过原 bank 的 admission，保留 outcome 和 usage；
不会更新 grant、hold 或 deadline。没有显式 deadline 时，通过一次无 effect 的 preparation
取得已配置 deadline，在 mediated invoke 前释放 preparation，然后固定 deadline。
显式 deadline 跳过这一步。Failure、observer/settlement 错误和已交付 streaming part 不触发额外调用。

空最终报告是错误。有内容的 `MaxTokens` 报告只在 public 投影中加 `Incomplete`，
不修改不可变 outcome，不重试部分文本。空 compression 耗尽 ladder 后返回 diagnostic，
不会伪造成功的 provider result。

示例16保持默认8,192 cap 和每次 ask 的300秒 deadline，只对已完成的空响应最多调用三次；
不加倍 cap，也不重试 failure。空中间响应或无效 evaluator 分数不会被替换成零；截断的最终回答作为不完整结果以1退出。
示例28的 rewrite 请求 low effort 和512输出 token，
空/空白响应时原样返回原问题，provider timeout 为180秒。
这些路径设置不改变共享 factory 的默认值。

多客户 self-evolving chatbot 在有效 UTF-8 边界内将 judge 历史前缀限制为200字节，
并在异常时报告错误且以1退出。The Beast 的 `baldwin_llm --llm` 必须提供密钥，
不会改用 offline oracle。类型化请求使用 low effort 和默认1,824 cap；只有已完成的空
`MaxTokens` 响应会以默认3,648 cap 重新请求一次。原始 Outcome 和每次实际调用的 usage 均保留。
Provider failure、仍为空的响应或无法解析的运算名以1退出，而不会伪装成已完成的 learner。这些源码修复不是实时验证或解题成功声明。

### issue #190 / #317 的有限诊断

`NG_EXAMPLE_MAX_TOKENS` 是 ToT、Baldwin、Jarvis、`server_multi` 以及 inspection 示例 29/30 的显式正数单次调用输出上限。它包含隐藏的 reasoning，显式提供时永不加倍。未设置时，原始配方默认值保持不变。`NG_EXAMPLE_EMPTY_REASKS=0` 禁用现有的已完成空 `MaxTokens` 重新请求；最大/默认值对 ToT 为 2，对 Baldwin/Jarvis 为 1。这些是新的语义调用，绝不是对 failure 或已交付 tool-call 输出的重试。Jarvis 保留独立的 broker ordinal、原始 admission bank/deadline 以及每个实际 Outcome/report。不会为了通过而禁用任何 thinking 设置，不会虚构模型上限信息，也不会续期金钱 grant。

以下控制项只会减少原始示例的工作量：

| 配方 | 控制项与原始默认值 | 禁用重新请求时的最大调用数 |
|---|---|---|
| ToT | `NG_TOT_DEPTH=3` (1..3), `NG_TOT_BRANCHING=3` (1..3), `NG_TOT_BEAM_WIDTH=5` (1..5) | 原始 37；1/1/1 时 3 |
| Baldwin（两种模式） | `NG_BALDWIN_POPULATION=6` (2/4/6), `NG_BALDWIN_GENERATIONS=4` (1..4) | 原始 48；2/1 时 4 |
| 多客户服务器 | `NG_MULTI_CUSTOMERS=5`, `NG_MULTI_TURNS=5` (均为 1..5) | 原始 90；1 个客户/4 轮时 14 |

在**另行预留主机消费额度**后，从仓库 root 运行：

```sh
# Representative canaries, not substitutes for the unchanged original workload.
NG_EXAMPLE_MAX_TOKENS=8192 NG_EXAMPLE_EMPTY_REASKS=0 \
  NG_TOT_DEPTH=1 NG_TOT_BRANCHING=1 NG_TOT_BEAM_WIDTH=1 \
  ./build/example_tree_of_thoughts
NG_EXAMPLE_MAX_TOKENS=8192 NG_EXAMPLE_EMPTY_REASKS=0 \
  NG_BALDWIN_POPULATION=2 NG_BALDWIN_GENERATIONS=1 \
  ./build/cookbook_the_beast_baldwin_llm --llm
NG_EXAMPLE_MAX_TOKENS=8192 NG_MULTI_CUSTOMERS=1 NG_MULTI_TURNS=4 \
  ./build/cookbook_self_evolving_chatbot_multi
# These inspection commands retain sensitive native/provider payloads.
NG_EXAMPLE_MAX_TOKENS=8192 ./build/example_responses_envelope "Reply briefly to hello."
NG_EXAMPLE_MAX_TOKENS=8192 ./build/example_reasoning_effort
```

这五条命令最多 3 + 4 + 14 + 1 + 4 = 26 次模型调用和 212,992 个配置输出 token，输入 token 另计。它们既不证明美元价格，也不授权消费。请使用所有者批准的模型/路由，并保持日志私密。原始工作量 qualification 只去掉 shape override，仍记录显式 cap/重新请求选择；在这些显式设置下，完整的 ToT + Baldwin + 服务器最多 175 次调用 / 1,433,600 个配置输出 token。

ToT 必须输出非空的最终 `EXPR`/`CHECK`；请独立计算实际表达式，确认恰好使用一个 4、一个 7 和两个 8，且结果为 24。模型的评分不是算术 oracle。Baldwin 必须在两种模式下显示实际的 pipeline fitness 和命中数；不要强制特定的进化轨迹，也不要用 offline oracle 替代其 live learner。服务器必须以非空的模型/judge 输出到达所选的第四轮，且没有序列化/provider failure。30 是观察四种 effort 的 sweep：不承诺 reasoning token、耗时和正确性单调；空回答以 exit 2 退出并保留其 Outcome。29 可能合理地只返回 tool call 而没有回答文本；它是 envelope inspection，不是天气回答 benchmark。Provider failure 仍为非零退出。

### 检查点与演化

示例08保留 terminal checkpoint fork 后启动新 user turn 的流程，不演示暂停 reviewer 的 resume。
示例14根据实际 executor 次数计算节省数：首次五次调用后只重跑一个失败 sibling，节省四次。

示例54在选择 smoke/file 模式前注册 `pnoop`。从 repository root 使用已跟踪 seed/task 文件：

```bash
./build/example_evolution --smoke
./build/example_evolution examples/54_evolution_seed.json examples/54_evolution_task.json
```

检查实际 JSON 字段 `best.compiled`、`best.validated`、`best.executed`、`best.correct`；
仅有 `compile_passed` 不能证明正确执行。file 模式提供 built-in node type 和此 demo 的 `pnoop`，
custom type 仍需 host 注册。当前 Linux x86_64 file 模式使用此 seed/task pair，
四个 `best` flag 均为 true；未注册 node type 则失败，没有成功的 compile/execute/correct 结果。

### A2A dialect 与 task snapshot

示例37打印 card interface 和首次 RPC 选择的 dialect。client 选择兼容的 JSON-RPC 0.x/1.0
card interface；card URL 不会重定向已配置 RPC endpoint。未 fetch card 时，仅数值 `-32601`
允许初始 dialect probe，交付 SSE 后不会重发。示例38广告两个 dialect，并标注初始/更新
task snapshot；初始 task 不是已完成回答。server response encoding 由 `A2A-Version` header
选择，与 method 拼写无关。1.0 使用 PascalCase method、flat part、`returnImmediately`，
stream 累积 status/artifact update。caller 按完成/中断的 agent status text、首个 artifact text、
最后 agent history text 的顺序选择答案。

## 心智模型 — 三层，中间是 JSON

每个示例都属于三种设置之一：

1. **只用内置 node**（02, 04, 07, 14）：`llm_call` / `tool_dispatch`
   / mock-provider node — 图完全由 JSON wiring，不需要 subclassing。最接近 `create_react_graph()` 产生的内容。
2. **自定义 `GraphNode` subclass**（05, 09, 10, 25）：你控制精确的
   `run(NodeInput)` body — 通过 `NodeOutput` 发出 `ChannelWrite`、`Send` 或 `Command`。
   Send 扇出与 Command 路由覆盖就在这里。
3. **类型化 provider 请求与 Outcome** (13, 15, 16, 17)：SDK admission、有序事件与不可变 Outcome；没有 descriptor interpreter 或 WebSocket adapter。
图定义采用 JSON 形式（`std::map<std::string, json>`）。
[Python examples](../bindings/python/examples/) 使用相同的 topology 格式；
provider 请求与 Outcome 是独立类型化对象，不是 topology JSON。

## API key 节省策略

| Provider | 示例 |
|---|---|
| `OPENROUTER_API_KEY` | 01, 03, 12, 13, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 28, 29, 30, 34, 35, 40 |
| local server (no key) | 31 |
| **none** | 02, 04, 05, 06, 07, 08, 09, 10, 14, 21, 27, 36, 37, 38, 39, 41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57 |

示例25与26也使用 local Crawl4AI。当前 secure Docker image 要求非空的
`CRAWL4AI_API_TOKEN`，见示例26的 `.env.example`。

三十一个示例不需要 API key 就能运行 — 这是“试跑一下”的最低门槛。
尤其是示例 21（MCP 扇出，确定性 planner）和 27（异步并发，
用 `steady_timer` 代替 LLM 延迟）展示了不花 token 的引擎管线。
gRPC 套件（52–55, 57）也不需要 key，但需要 `-DNEOGRAPH_BUILD_GRPC=ON`
（`grpc++` / `protoc`）；56（`history_compaction`）默认使用 mock provider，
只有存在 key 时才会触碰 OpenRouter。
## CMake 配置后重新运行

构建出的二进制文件位于构建目录根部，命名为 `example_<short_name>`
（例如 `example_react_agent`、`example_custom_graph`）。准确名称在每个 `.cpp`
顶部注释的 `Usage:` 下。

## Responses inspection 契约 (13 / 29 / 30 / 34)

13 提交类型化 Responses streaming request；事件不是字符串 callback。
29 保留成功/失败的完整 Outcome、`wire_envelope`、所有有序 `wire_output` item 和类型化 part、
function argument、citation、reasoning、opaque hosted output 与 artifact。
raw inspection 输出含敏感内容，不是安全的 telemetry/export 格式。
30 扫描 `none`、`low`、`medium`、`high` 并保留每个完整 Outcome。
reasoning/input/output/total/provider-reported-total 及 extra count 是 nullable 64-bit evidence，
包含 stage、quality、conflict。缺失不等于 zero；visible text 与预留不是 usage。
34 保留全部七部分：function(calculator)、web search、image generation、file search、
tool search、`shell.environment` 的 skills 和 shell(`container_auto`)。
`OPENROUTER_VECTOR_STORE_ID` 是 file search 的前提；`OPENROUTER_SKILL_ID` 覆盖默认
`openai-spreadsheets` skill。本 inspection demo 只声明 function tool，不在本地执行。
保留全部有序类型化/raw event 和完整 terminal。hosted tool 可能不被 route 支持或产生额外费用；
类型化 admission 不是 live 兼容保证。不保留 WebSocket/primitive 可运行示例。
Images/Veo/Decisions 是独立类型化 NeoGraph client，不继承 chat spending grant。

Decisions 的 choice criteria 是以 choice 为键的对象，而不是字符串数组。
当前独立的 typed client 无需 raw `SchemaProvider::request_json()` body 即可表达有效结构：

```cpp
#include <neograph/llm/decisions_client.h>
neograph::llm::DecisionsChoiceQuestion question;
question.instructions = "Choose whether the supplied answer addresses the question.";
question.criteria = {{"accept", "The answer addresses the question."},
                     {"reject", "The answer does not address the question."}};
neograph::llm::DecisionsRequest request;
request.state = {{"question", "What is 2 + 2?"}, {"answer", "4"}};
request.questions.emplace("decision", std::move(question));
```

这只是请求构造，不是新的付费请求，也不证明 vendor 准确性。
