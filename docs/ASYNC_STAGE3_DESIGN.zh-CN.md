<!-- neograph-i18n: source=docs/ASYNC_STAGE3_DESIGN.md locale=zh-CN source_sha256=1b7adfd905a5df954404b35ec17b04b8e334eaee56e2b5f2c8d042b831c9ffa0 -->
# 第 3 阶段：基于 asio 的完整异步重构设计

> **已归档、已被取代的设计提案。** 本文记录的是 2026-04-19 提出的第 3 阶段重构方案，
> 它不是当前的 API 指南，也不能证明方案里计划的每道闸门都已完成。
> 文中的 `Provider::complete` / `complete_async` 这一对接口以及 `OpenAIProvider`，
> 已在带类型 provider 的切换中移除。现在的 provider 持有带类型的请求，
> 只做一次准备，再分发这个已准备好的调用；
> 请参阅 [API 导览](reference-en.md)、[迁移指南](migration-v0.4-to-v1.0.md)和已安装的头文件。
> SDK 的 `Client::complete` 是另一个接口，仍受支持。
>
> 下文所有的“当前状态”、排期、闸门、版本号和后续动作，指的都是该提案撰写的那个日期。
> 第 3 阶段的范围不含 Python；当前这次切换已经包含带类型的 Python provider 和自有的 outcome。
> 另外，即使禁用了 LLM 节点，当前的 Core 仍然需要外部的 SchemaProvider 运行时。
> 已实现的契约和平台资格认证的限制，见[异步指南](ASYNC_GUIDE.md)和 [Python 绑定](python-binding.md)。

**Languages:** [English](ASYNC_STAGE3_DESIGN.md) | [한국어](ASYNC_STAGE3_DESIGN.ko.md) | [日本語](ASYNC_STAGE3_DESIGN.ja.md) | [简体中文](ASYNC_STAGE3_DESIGN.zh-CN.md)

撰写日期：2026-04-19（feat/async-api）
前置条件：第 1 阶段的定时器 PoC（c356e0f）和第 2 阶段的 HTTP PoC（b008c11）已完成。
**决定推进**，依据是 `bench_async_fanout` / `bench_async_http` 的结果。

---

## 0. 目的与非目的

### 目的（第 3 阶段结束时要解决的问题）
- 支持承载 1K 以上的并发 agent：现在每个 agent 一个线程的做法，大约到 1K 就撑不住了。
- 等待 HTTP / DB / MCP 的 I/O 时不占用线程，内存可节省 5–7 倍。
- `run()` 可以在 executor 上执行，从而接入外部事件循环（例如 Web 服务器）。

### 非目的（第 3 阶段不涉及）
- 1K 以下的 agent 场景里，微秒级的绝对性能提升：目前已经够用。
- Python/TS 绑定。保持嵌入式 C++ API 不变。
- 分布式/多进程。只做单进程内的扩展。
- 用户 Tool 接口的变更：Tool::call() 仍是同步的，以减轻用户负担。

---

## 1. 现状快照

### 同步 I/O 的依赖点（第 3 阶段要改造的对象）

| 层 | 文件 | 行数 | 依赖库 |
|---|---|---|---|
| LLM HTTP | `src/llm/openai_provider.cpp` | 228 | httplib |
| LLM HTTP（通用） | `src/llm/schema_provider.cpp` | 1400+ | httplib |
| MCP HTTP | `src/mcp/client.cpp` | 471 | httplib |
| DB | `src/core/postgres_checkpoint.cpp` | 692 | libpqxx（同步） |
| 并行节点执行 | `src/core/graph_executor.cpp` | 520 | Taskflow（CPU 线程池） |
| 引擎循环 | `src/core/graph_engine.cpp` | 529 | —（同步的 run 循环） |

### 已有的异步基础
- `deps/asio/`：已内置 standalone asio 1.30.2。
- `include/neograph/async/http_client.h`：PoC 版的 async_post（仅支持 HTTP/1.1）。
- `src/async/{async_smoke,http_client}.cpp`：协程行为已验证。
- 2 类基准测试：复用为回归测量的工具。

### 会受影响的调用面
- `Provider::complete()`：52 处调用（示例和测试合计）。
- `CheckpointStore::save/load/list`：29 处调用。
- `GraphEngine::run() / run_stream() / resume()`：26 个示例全部用到。
- `MCPClient::rpc_call()`：7 个示例用到（03、20–24）。

---

## 2. 目标架构

```
┌─────────────────────────────────────────────────────┐
│  User code (examples / user apps)                    │
│  - sync facade (default)   - async facade (opt-in)   │
└──────────────┬──────────────────────┬────────────────┘
               │                      │
      run_sync()|                     │run_async() → Task<RunResult>
               │                      │
┌──────────────▼──────────────────────▼────────────────┐
│  GraphEngine (coroutine-native core)                 │
│    Task<RunResult> run(RunConfig)                    │
│    ├─ NodeExecutor    : co_await node bodies         │
│    ├─ Scheduler       : pure (unchanged)             │
│    ├─ Coordinator     : co_await ckpt_store->save    │
│    └─ io_context ref  : injected or owned            │
└──┬────────────────┬─────────────────┬────────────────┘
   │                │                 │
┌──▼─────┐   ┌──────▼──────┐   ┌──────▼─────────┐
│ Async  │   │ Async       │   │ Async MCP      │
│ HTTP   │   │ Postgres    │   │ Client         │
│ client │   │ (libpq      │   │ (HTTP/stdio)   │
│ + TLS  │   │  pipeline)  │   │                │
└────────┘   └─────────────┘   └────────────────┘
   ↑                ↑                 ↑
   └─── shared asio::io_context (one, N worker threads) ──┘
```

### 核心决定
1. **只有一个 io_context**：可以注入 executor，但默认是进程级的单例。
2. **Provider / CheckpointStore / MCPClient 原生就是协程**：同步方法只通过 `run_sync()` 包装器提供。
3. **Tool 保持同步**，以减轻用户负担；内部用 `co_await asio::post(thread_pool, ...)` 转到线程池上执行。
4. **保留 Taskflow**，只把 executor 换成基于 asio 的实现，现有的 fan-out 代码路径不动。
   - 备选方案：彻底移除 Taskflow，改用 asio::co_spawn。到迭代 4 再定。

---

## 3. 迭代划分（6–10 周 → 4 个迭代）

每个迭代约 2 周。每个迭代结束时，要求构建通过，并完成基准回归测量。

### 迭代 1：补全异步 HTTP 基础（1.5 周）

目标：把 HTTP 客户端补全到足以让 LLM 调用可以 await。

| # | 任务 | 文件 | 预估 |
|---|---|---|---|
| 1.1 | 支持 asio::ssl 的 HTTPS | `src/async/http_client.cpp` | 2 天 |
| 1.2 | Keep-alive 连接池 | `src/async/conn_pool.{h,cpp}`（新增） | 2 天 |
| 1.3 | 可重试的传输错误分类 | `include/neograph/async/http_errors.h`（新增） | 0.5 天 |
| 1.4 | SSE（流式）解析器 | `src/async/http_client.cpp` | 1 天 |
| 1.5 | 重定向、超时、提取 Retry-After | 同上 | 1 天 |
| 1.6 | 重跑 bench_async_http（TLS 路径） | `benchmarks/bench_async_http.cpp` | 0.5 天 |

**完成标准**：
- `async_post` / `async_post_stream` 这两个 API 覆盖 LLM 所需的全部线上请求。
- 开启 keep-alive、5K 并发时，bench_async_http 的结果不低于第 2 阶段。
- 单元测试：TLS 握手、连接池复用、SSE 重组。

### 迭代 2：Provider 与 MCP 的异步改造（2 周）

目标：把发起 HTTP 调用的三层（openai、schema、mcp）改成异步，同时通过包装器保留现有的同步 API。

| # | 任务 | 文件 | 预估 |
|---|---|---|---|
| 2.1 | 增加 `Provider::complete_async`（纯虚函数） | `include/neograph/provider.h` | 0.5 天 |
| 2.2 | 同步的 `complete()` 默认实现为 `run_sync(complete_async())` | 同上 | 0.5 天 |
| 2.3 | OpenAIProvider 的异步实现 | `src/llm/openai_provider.cpp` | 1 天 |
| 2.4 | SchemaProvider 的异步实现（工作量大） | `src/llm/schema_provider.cpp` | 3 天 |
| 2.5 | RateLimitedProvider：基于 Retry-After 的 co_await sleep | `src/llm/rate_limited_provider.cpp` | 1 天 |
| 2.6 | MCPClient 的异步 HTTP 路径 | `src/mcp/client.cpp` | 1 天 |
| 2.7 | MCP stdio 异步化（asio::posix::stream_descriptor） | 同上 | 2 天 |
| 2.8 | 现有的 provider/mcp 测试全部通过 | `tests/test_schema_provider_*`、`test_rate_limited_provider.cpp` | 已含在内 |

**完成标准**：
- 所有现有测试通过（内部走异步，调用走同步包装器）。
- 新增测试：在同一个 io_context 上，同时执行数千次 provider 调用。
- 示例仍通过同步门面运行，无需改动。

### 迭代 3：异步 CheckpointStore 与引擎协程化（2.5 周）

目标：让 GraphEngine 以协程方式运行，Postgres 后端使用 pipeline 异步模式。

| # | 任务 | 文件 | 预估 |
|---|---|---|---|
| 3.1 | `CheckpointStore::save_async / load_async`（纯虚函数） | `include/neograph/graph/checkpoint.h` | 0.5 天 |
| 3.2 | InMemoryStore / SQLite 的异步包装 | `src/core/graph_checkpoint.cpp`、`src/core/sqlite_checkpoint.cpp` | 1 天 |
| 3.3 | libpq 的 pipeline 异步模式，放弃 libpqxx | 重写 `src/core/postgres_checkpoint.cpp` | 5 天 |
| 3.4 | 增加 `GraphNode::execute_async`（Task<NodeResult>） | `include/neograph/graph/node.h` | 1 天 |
| 3.5 | 4 种内置节点的异步实现（LLMCall、ToolDispatch、IntentClassifier、Subgraph） | `src/core/graph_node.cpp` | 2 天 |
| 3.6 | `GraphEngine::run_async`：改造成协程 | `src/core/graph_engine.cpp`、`graph_executor.cpp` | 3 天 |
| 3.7 | Taskflow 的 fan-out 改为 asio::experimental::parallel_group | `graph_executor.cpp` | 2 天 |
| 3.8 | 重新测量 bench_neograph，确认零回归 | `benchmarks/bench_neograph.cpp` | 1 天 |

**完成标准**：
- `run_async()` 与 `run()` 并存，同步版本是包装器。
- Postgres 检查点在 64 线程基准下的结果持平或更好。
- 26 个示例都不需要改动（得益于同步门面）。
- 新增测试：10K 个并发 run() 在 2GB 内存内完成。

### 迭代 4：迁移、Tool 异步化与收尾（1.5 周）

目标：铺好迁移路径，让用户真正能从异步中获益。

| # | 任务 | 文件 | 预估 |
|---|---|---|---|
| 4.1 | 选 1–2 个并发较高的示例改成异步（例如 05_parallel_fanout、26_postgres） | `examples/` | 1 天 |
| 4.2 | 为 Tool 提供异步卸载的辅助工具：`AsyncTool` 适配器 | `include/neograph/tool.h` | 1 天 |
| 4.3 | 文档：异步指南、迁移检查清单 | 新增 `docs/ASYNC_GUIDE.md` | 1 天 |
| 4.4 | 更新 NEXT_SESSION.md / README | — | 0.5 天 |
| 4.5 | 最终决定是否移除对 Taskflow 的依赖 | — | 0.5 天 |
| 4.6 | CI 中加入 bench_async_* 回归闸门 | `.github/workflows/` | 1 天 |
| 4.7 | 大版本升级到 2.0.0 | `CMakeLists.txt` 等 | 0.5 天 |

**完成标准**：
- 合并到 master，分支结束。
- 发布 `NeoGraph 2.0`，附带不兼容变更清单的发布说明。
- 26 个同步示例全部通过，并新增 1–2 个异步示例。

---

## 4. 不兼容变更矩阵

| API | 变更 | 迁移成本 | 处理策略 |
|---|---|---|---|
| `Provider::complete` | 保留同步版本，新增 `complete_async` | 无 | 声明两个纯虚函数，用默认实现让它们互相调用 |
| `GraphNode::execute` | 保留同步版本，新增 `execute_async` | 自定义节点不受影响 | 同上 |
| `GraphEngine::run` | 保留同步版本，新增 `run_async` | 无 | 同步门面 |
| `CheckpointStore::save` | 保留同步版本，新增 `save_async` | 自定义存储不受影响 | 同上 |
| `PostgresCheckpointStore` | libpqxx 改为直接用 libpq | 用户侧 API 不变 | 内部替换 |
| `MCPClient::rpc_call` | 保留同步版本，新增 `rpc_call_async` | 无 | 同步门面 |

**结论：迁移之后，所有同步 API 都保留。** 升到 2.0 的原因，是内部依赖变了（放弃 libpqxx）以及需要 C++20 协程。

---

## 5. 风险登记

| 风险 | 影响 | 缓解措施 |
|---|---|---|
| libpq 的 pipeline 模式不适合检查点的写入模式 | 迭代 3 延期 | 迭代 3 初期用 2 天 spike 验证。如果不适合，就回退到 libpq 同步调用加 `asio::post(thread_pool)`：性能收益只有一半，但仍能完成 |
| asio::ssl 与 Anthropic/OpenAI endpoint 的 ALPN 问题 | 迭代 1 延期 | 第 2 阶段的基准只用了 HTTP。迭代 1 初期就对真实 endpoint 做冒烟测试 |
| Taskflow 与 asio executor 难以整合 | 迭代 3 延长 | 保留 Taskflow，只把并行节点内部改成协程；彻底移除放到迭代 4，作为可选项 |
| 同步门面 `run_sync(coro)` 死锁（单线程 io_context 环境） | 运行时 bug | 用户门面始终加一层保护，确保 io_context 至少有 1 个工作线程 |
| 同步、异步两条路径都要测，测试数量翻倍 | 维护成本 | 只定义一次参数化测试，自动在两条路径上运行 |
| 示例 26 出现回归 | 发布延期 | 示例不做异步改造（只有迭代 4 里的可选项）。同步门面通过即可 |

---

## 6. 验证闸门（每个迭代通用）

每个迭代完成时，必须满足：

1. `cmake --build build -j`：警告为 0。
2. `ctest -j`：172 个以上的测试全部通过。
3. `benchmarks/bench_neograph`：现有的三项指标（单次运行微秒数、1 线程 PG、64 线程 PG）的回归不超过 5%。
4. `benchmarks/bench_async_http`：不低于第 2 阶段。
5. `benchmarks/bench_async_fanout`：50K 个定时器仍保持 6 倍。
6. ASan / TSan 构建通过（`build-asan`、`build-tsan` 已存在）。
7. 提交使用 `feat(async)` 前缀，并附 Co-Authored-By。

---

## 7. 排期汇总

| 周 | 迭代 | 主要交付 |
|---|---|---|
| W1 | 1 | TLS + keep-alive + SSE |
| W2 | 1→2 | 重新测量基准，开始 Provider 异步化 |
| W3 | 2 | SchemaProvider、MCP 异步化 |
| W4 | 2→3 | CheckpointStore 异步化，开始重写 libpq |
| W5 | 3 | libpq pipeline、内置节点异步化 |
| W6 | 3 | 引擎协程化、executor 改造 |
| W7 | 3→4 | 基准回归闸门 |
| W8 | 4 | 1–2 个示例、文档、CI 闸门 |

合计 **8 周**，处在 6–10 周范围的中间。如需落实风险缓解措施，W5/W6 可以再延长 1 周。

---

## 8. 开工前的最后确认

下次会话开始时确认：

- [ ] 继续在 `feat/async-api` 分支上做，还是另开 `feat/async-stage3`？→ **建议继续用这个分支**，保持从 PoC 到第 3 阶段的连续性。
- [ ] 确定迭代 1 的 spike 目标主机（比较 api.openai.com 与 api.anthropic.com 在 TLS 上的行为差异）。
- [ ] 提前阅读 libpq pipeline 模式的文档。
- [ ] 对移除还是保留 Taskflow，先定个初步立场（默认：保留，迭代 4 再评估）。

---

**下一步**：开始迭代 1.1，在 `src/async/http_client.cpp` 中加入 asio::ssl 层。
