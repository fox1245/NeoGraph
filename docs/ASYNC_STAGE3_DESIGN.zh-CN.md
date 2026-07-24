<!-- neograph-i18n: source=docs/ASYNC_STAGE3_DESIGN.md locale=zh-CN source_sha256=504932a9848ba52c794d9ac51f4b7b599605bab11e2e679c4a2d11fd7f9d43e2 -->
# 第三阶段 — 基于 asio 的完整异步重构设计

**Languages:** [English](ASYNC_STAGE3_DESIGN.md) | [한국어](ASYNC_STAGE3_DESIGN.ko.md) | [日本語](ASYNC_STAGE3_DESIGN.ja.md) | [简体中文](ASYNC_STAGE3_DESIGN.zh-CN.md)

撰写时间：2026-04-19（feat/async-api）
前置条件：第一阶段定时器 PoC (c356e0f) + 第二阶段 HTTP PoC (b008c11) 已完成。
**决策依据**：`bench_async_fanout` / `bench_async_http` 结果。

---

## 0. 目标与非目标

### 目标（第三阶段结束时将解决的事项）
- 支持承载 1K+ 并发 agent——当前每个 agent 一个线程大约在 ~1K 时失效。
- HTTP / DB / MCP I/O 等待期间不占用线程 → 5–7× 内存节省。
- `run()` 可以在执行器上执行 → 外部事件循环集成（例如 Web 服务器）。

### 非目标（第三阶段不涉及）
- 1K 以下 agent 用例的绝对 µs 改善——目前已足够。
- Python/TS 绑定。保持嵌入式 C++ API。
- 分布式/多进程。仅单进程扩展。
- 用户 Tool 接口变更——Tool::call() 保持同步（最小化用户负担）。

---

## 1. 当前状态快照

### 同步 I/O 依赖点（第三阶段转换目标）

| 层 | 文件 | 行数 | 依赖库 |
|---|---|---|---|
| LLM HTTP | `src/llm/openai_provider.cpp` | 228 | httplib |
| LLM HTTP（通用） | `src/llm/schema_provider.cpp` | 1400+ | httplib |
| MCP HTTP | `src/mcp/client.cpp` | 471 | httplib |
| DB | `src/core/postgres_checkpoint.cpp` | 692 | libpqxx（同步） |
| 并行节点执行 | `src/core/graph_executor.cpp` | 520 | Taskflow（CPU 池） |
| 引擎循环 | `src/core/graph_engine.cpp` | 529 | —（同步运行循环） |

### 现有异步资产
- `deps/asio/` — 已集成独立的 asio 1.30.2。
- `include/neograph/async/http_client.h` — PoC async_post（仅 HTTP/1.1）。
- `src/async/{async_smoke,http_client}.cpp` — 已验证协程行为。
- 2 种基准测试类型——复用为回归测量工具。

### 破坏面
- `Provider::complete()` — 52 个调用点（示例 + 测试合计）。
- `CheckpointStore::save/load/list` — 29 个调用点。
- `GraphEngine::run() / run_stream() / resume()` — 所有 26 个示例使用。
- `MCPClient::rpc_call()` — 7 个示例（03、20–24）。

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

### 核心决策
1. **一个 io_context** — 允许执行器注入，但默认是进程级单例。
2. **Provider / CheckpointStore / MCPClient 是协程原生的** — 同步方法
   仅在 `run_sync()` 包装器内提供。
3. **Tool 保持同步** — 最小化用户负担。内部通过
   `co_await asio::post(thread_pool, ...)` 卸载。
4. **Taskflow 保留**，执行器替换为基于 asio 的实现。保留现有扇出代码路径。
   - 替代方案：完全移除 Taskflow 并以 asio::co_spawn 替代。在第四学期决定。

---

## 3. 学期划分（6–10 周 → 4 个学期）

每个学期 ≈ 2 周。每学期结束时构建通过 + 基准回归测量。

### 第一学期 — 异步 HTTP 基础完成（1.5 周）

目标：完成 HTTP 客户端，使 LLM 调用可被等待。

| # | 任务 | 文件 | 预估 |
|---|---|---|---|
| 1.1 | asio::ssl HTTPS 支持 | `src/async/http_client.cpp` | 2 天 |
| 1.2 | Keep-alive 连接池 | `src/async/conn_pool.{h,cpp}`（新） | 2 天 |
| 1.3 | 可重试传输错误分类 | `include/neograph/async/http_errors.h`（新） | 0.5 天 |
| 1.4 | SSE（流式）解析器 | `src/async/http_client.cpp` | 1 天 |
| 1.5 | 重定向、超时、Retry-After 提取 | 同上 | 1 天 |
| 1.6 | bench_async_http 重新运行（TLS 路径） | `benchmarks/bench_async_http.cpp` | 0.5 天 |

**完成标准**：
- `async_post` / `async_post_stream` 两个 API 覆盖所有 LLM 线路需求。
- bench_async_http 结果在启用 keep-alive 且 5K 并发时等于或优于第二阶段。
- 单元测试 — TLS 握手 / 连接池复用 / SSE 重组。

### 第二学期 — Provider 与 MCP 异步转换（2 周）

目标：将三个发起 HTTP 调用的层（openai、schema、mcp）异步化。通过包装器
保留现有同步 API。

| # | 任务 | 文件 | 预估 |
|---|---|---|---|
| 2.1 | 添加 `Provider::complete_async`（纯虚函数） | `include/neograph/provider.h` | 0.5 天 |
| 2.2 | 同步 `complete()` = `run_sync(complete_async())` 默认实现 | 同上 | 0.5 天 |
| 2.3 | OpenAIProvider 异步实现 | `src/llm/openai_provider.cpp` | 1 天 |
| 2.4 | SchemaProvider 异步实现（工作量大） | `src/llm/schema_provider.cpp` | 3 天 |
| 2.5 | RateLimitedProvider — 基于 Retry-After 的 co_await sleep | `src/llm/rate_limited_provider.cpp` | 1 天 |
| 2.6 | MCPClient 异步 HTTP 路径 | `src/mcp/client.cpp` | 1 天 |
| 2.7 | MCP stdio 异步（asio::posix::stream_descriptor） | 同上 | 2 天 |
| 2.8 | 所有现有 provider/mcp 测试通过 | `tests/test_schema_provider_*`、`test_rate_limited_provider.cpp` | 包含在内 |

**完成标准**：
- 所有现有测试通过（异步内部，同步包装器调用）。
- 新测试：数千个 provider 调用在同一 io_context 上并发执行。
- 示例通过同步门面仍可工作——无变更。

### 第三学期 — 异步 CheckpointStore + 引擎协程（2.5 周）

目标：GraphEngine 通过协程运作。Postgres 后端利用管线异步模式。

| # | 任务 | 文件 | 预估 |
|---|---|---|---|
| 3.1 | `CheckpointStore::save_async / load_async`（纯虚函数） | `include/neograph/graph/checkpoint.h` | 0.5 天 |
| 3.2 | InMemoryStore / SQLite 异步包装器 | `src/core/graph_checkpoint.cpp`、`src/core/sqlite_checkpoint.cpp` | 1 天 |
| 3.3 | libpq 管线异步模式 — 移除 libpqxx | `src/core/postgres_checkpoint.cpp` 重写 | 5 天 |
| 3.4 | 添加 `GraphNode::execute_async`（Task<NodeResult>） | `include/neograph/graph/node.h` | 1 天 |
| 3.5 | 4 种内置节点类型的异步实现（LLMCall、ToolDispatch、IntentClassifier、Subgraph） | `src/core/graph_node.cpp` | 2 天 |
| 3.6 | `GraphEngine::run_async` — 协程转换 | `src/core/graph_engine.cpp`、`graph_executor.cpp` | 3 天 |
| 3.7 | Taskflow 扇出 → asio::experimental::parallel_group | `graph_executor.cpp` | 2 天 |
| 3.8 | bench_neograph 重新测量 — 确认零回归 | `benchmarks/bench_neograph.cpp` | 1 天 |

**完成标准**：
- `run_async()` 和 `run()` 两者并存。同步是包装器。
- Postgres 检查点 64 线程基准测试结果保持或改善。
- 所有 26 个示例 — 无变更（得益于同步门面）。
- 新测试：10K 并发 run() 在 2GB 内存内完成。

### 第四学期 — 迁移、Tool 异步、清理（1.5 周）

目标：建立使用户真正能从异步中受益的迁移路径。

| # | 任务 | 文件 | 预估 |
|---|---|---|---|
| 4.1 | 选择 1–2 个高并发候选示例进行异步转换（例如 05_parallel_fanout、26_postgres） | `examples/` | 1 天 |
| 4.2 | Tool 异步卸载辅助类 — `AsyncTool` 适配器 | `include/neograph/tool.h` | 1 天 |
| 4.3 | 文档 — 异步指南、迁移清单 | `docs/ASYNC_GUIDE.md` 新 | 1 天 |
| 4.4 | NEXT_SESSION.md / README 更新 | — | 0.5 天 |
| 4.5 | 关于 Taskflow 依赖是否移除的最终决定 | — | 0.5 天 |
| 4.6 | CI bench_async_* 回归门禁 | `.github/workflows/` | 1 天 |
| 4.7 | 主版本号升级 → 2.0.0 | `CMakeLists.txt` 等 | 0.5 天 |

**完成标准**：
- 合并到 master。分支结束。
- `NeoGraph 2.0` 发布说明，包含破坏性变更列表。
- 所有 26 个同步示例通过，1–2 个新异步示例新增。

---

## 4. 破坏性变更矩阵

| API | 变更 | 迁移成本 | 解决策略 |
|---|---|---|---|
| `Provider::complete` | 同步保持，`complete_async` 新增 | 无 | 声明两个纯虚函数，通过默认实现互相连接 |
| `GraphNode::execute` | 同步保持，`execute_async` 新增 | 自定义节点不受影响 | 同上 |
| `GraphEngine::run` | 同步保持，`run_async` 新增 | 无 | 同步门面 |
| `CheckpointStore::save` | 同步保持，`save_async` 新增 | 自定义存储不受影响 | 同上 |
| `PostgresCheckpointStore` | libpqxx → 直接 libpq | 对用户无 API 变更 | 内部替换 |
| `MCPClient::rpc_call` | 同步保持，`rpc_call_async` 新增 | 无 | 同步门面 |

**结论：迁移后所有同步 API 仍然保留**。2.0 版本升级是由于内部依赖
（移除 libpqxx）+ C++20 协程要求。

---

## 5. 风险登记

| 风险 | 影响 | 缓解措施 |
|---|---|---|
| libpq 管线模式不适合检查点写入模式 | 第三学期延迟 | 在第三学期早期用 2 天 spike 验证。如不适合，回退到 libpq 同步调用 + `asio::post(thread_pool)` — 获得一半的性能收益但仍可完成 |
| asio::ssl + Anthropic/OpenAI 端点 ALPN 问题 | 第一学期延迟 | 第二阶段基准仅使用了 HTTP。在第一学期早期运行真实端点冒烟测试 |
| Taskflow ↔ asio 执行器集成困难 | 第三学期延长 | 保留 Taskflow，仅使并行节点内部协程化 — 完全移除是第四学期的可选任务 |
| 同步门面 `run_sync(coro)` 死锁（单线程 io_context 环境） | 运行时错误 | 用户门面始终以确保 io_context 中至少有一个工作线程的守卫包裹 |
| 同时测试同步/异步路径 → 测试数量翻倍 | 维护成本 | 定义参数化测试一次，自动运行两条路径 |
| 示例 26 回归 | 发布延迟 | 示例不进行异步转换（第四学期才可选）。同步门面通过即可 |

---

## 6. 验证门禁（所有学期通用）

每学期结束时，必须：

1. `cmake --build build -j` — 零警告。
2. `ctest -j` — 所有 172+ 测试通过。
3. `benchmarks/bench_neograph` — 现有三个指标（单次运行 µs、1 线程 PG、
   64 线程 PG）在 5% 回归范围内。
4. `benchmarks/bench_async_http` — 等于或优于第二阶段。
5. `benchmarks/bench_async_fanout` — 50K 定时器保持 6×。
6. ASan / TSan 构建通过（`build-asan`、`build-tsan` 已存在）。
7. 提交使用 `feat(async)` 前缀 + Co-Authored-By。

---

## 7. 时间表摘要

| 周 | 学期 | 关键交付物 |
|---|---|---|
| W1 | 1 | TLS + keep-alive + SSE |
| W2 | 1→2 | 基准重新测量，Provider 异步开始 |
| W3 | 2 | SchemaProvider、MCP 异步 |
| W4 | 2→3 | CheckpointStore 异步，libpq 重写开始 |
| W5 | 3 | libpq 管线，内置节点异步 |
| W6 | 3 | 引擎协程，执行器转换 |
| W7 | 3→4 | 基准回归门禁 |
| W8 | 4 | 示例 1-2、文档、CI 门禁 |

总计 **8 周**——6–10 周范围的中点。如需风险缓解，第 5/6 周有 1 周扩展空间。

---

## 8. 开始前的最终检查

在下次会话开始时确认：

- [ ] 在 `feat/async-api` 分支上继续？还是分支到 `feat/async-stage3`？
  → **推荐在当前分支上继续**。保持 PoC → 第三阶段的连续性。
- [ ] 确定第一学期 spike 目标主机（api.openai.com 与
  api.anthropic.com TLS 行为差异）。
- [ ] 预先审查 libpq 管线模式文档。
- [ ] 对 Taskflow 移除 vs 保留的初步立场（默认：保留，第四学期重新评估）。

---

**下一步行动**：第一学期 1.1 开始 — 为 `src/async/http_client.cpp` 添加 asio::ssl 层。
