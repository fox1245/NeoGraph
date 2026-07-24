# Stage 3 — asio-based Full Async Refactor Design

Author: 2026-04-19 (feat/async-api)
Prerequisite: Stage 1 timer PoC (c356e0f) + Stage 2 HTTP PoC (b008c11) completed.
**Go decision** based on `bench_async_fanout` / `bench_async_http` results.

---

## 0. Purpose and Non-purpose

### Purpose (things that will be resolved when Stage 3 ends)
- Support hosting 1K+ concurrent agents — current thread-per-agent breaks around ~1K.
- No thread occupation during HTTP / DB / MCP I/O wait → 5-7× memory savings.
- `run()` can execute on an executor → external event loop integration (e.g. web server).

### Non-purpose (not touched in Stage 3)
- Absolute µs improvement for agent use cases under 1K — currently sufficient.
- Python/TS bindings. Embedded C++ API maintained.
- Distributed/multi-process. Single-process scaling only.
- User Tool interface changes — Tool::call() remains synchronous (minimize user burden).

---

## 1. Current State Snapshot

### Synchronous I/O Dependency Points (Stage 3 Conversion Targets)

| Layer | File | Line Count | Dependency Library |
|---|---|---|---|
| LLM HTTP | `src/llm/openai_provider.cpp` | 228 | httplib |
| LLM HTTP (generic) | `src/llm/schema_provider.cpp` | 1400+ | httplib |
| MCP HTTP | `src/mcp/client.cpp` | 471 | httplib |
| DB | `src/core/postgres_checkpoint.cpp` | 692 | libpqxx (synchronous) |
| Parallel Node Execution | `src/core/graph_executor.cpp` | 520 | Taskflow (CPU pool) |
| Engine loop | `src/core/graph_engine.cpp` | 529 | — (synchronous run loop) |

### Existing Async Assets
- `deps/asio/` — standalone asio 1.30.2 vendored.
- `include/neograph/async/http_client.h` — PoC async_post (HTTP/1.1 only).
- `src/async/{async_smoke,http_client}.cpp` — coroutine behavior verified.
- 2 benchmark types — reused as regression measurement harness.

### Breaking Surface
- `Provider::complete()` — 52 call sites (examples + tests combined).
- `CheckpointStore::save/load/list` — 29 call sites.
- `GraphEngine::run() / run_stream() / resume()` — all 26 examples used.
- `MCPClient::rpc_call()` — 7 examples (03, 20–24).

---

## 2. Target Architecture

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

### Core Decisions
1. **One io_context** — executor injection possible, but default is process-wide singleton.
2. **Provider / CheckpointStore / MCPClient are coroutine-native** — synchronous methods provided only within `run_sync()` wrapper.
3. **Tool remains synchronous** — minimize user burden. Internally offloads via `co_await asio::post(thread_pool, ...)`.
4. **Taskflow maintained**, executor replaced with asio-based. Preserve existing fan-out code paths.
   - Alternative: Complete Taskflow removal + replacement with asio::co_spawn. Decision in Semester 4.

---

## 3. Semester Split (6-10 weeks → 4 semesters)

Each semester ≈ 2 weeks. Build green + bench regression measurement at semester end.

### Semester 1 — Async HTTP Foundation Completion (1.5 weeks)

Goal: Complete HTTP client enough to make LLM calls awaitable.

| # | Task | File | Estimated |
|---|---|---|---|
| 1.1 | asio::ssl HTTPS support | `src/async/http_client.cpp` | 2 days |
| 1.2 | Keep-alive connection pool | `src/async/conn_pool.{h,cpp}` (new) | 2 days |
| 1.3 | Retryable transport error classification | `include/neograph/async/http_errors.h` (new) | 0.5 day |
| 1.4 | SSE (streaming) parser | `src/async/http_client.cpp` | 1 day |
| 1.5 | Redirect, timeout, Retry-After extraction | Same as above | 1 day |
| 1.6 | bench_async_http re-run (TLS path) | `benchmarks/bench_async_http.cpp` | 0.5 day |

**Completion Criteria**:
- `async_post` / `async_post_stream` two APIs cover all LLM wire requirements.
- bench_async_http results equal or better than Stage 2 at 5K concurrent with keep-alive on.
- Unit tests — TLS handshake / pool reuse / SSE reassembly.

### Semester 2 — Provider & MCP Async Conversion (2 weeks)

Goal: Async-ify three layers that make HTTP calls (openai, schema, mcp). Preserve existing synchronous API via wrapper.

| # | Task | File | Estimated |
|---|---|---|---|
| 2.1 | Add `Provider::complete_async` (pure virtual) | `include/neograph/provider.h` | 0.5 day |
| 2.2 | Synchronous `complete()` = `run_sync(complete_async())` default implementation | Above | 0.5 day |
| 2.3 | OpenAIProvider async implementation | `src/llm/openai_provider.cpp` | 1 day |
| 2.4 | SchemaProvider async implementation (large workload) | `src/llm/schema_provider.cpp` | 3 days |
| 2.5 | RateLimitedProvider — Retry-After based co_await sleep | `src/llm/rate_limited_provider.cpp` | 1 day |
| 2.6 | MCPClient async HTTP path | `src/mcp/client.cpp` | 1 day |
| 2.7 | MCP stdio async (asio::posix::stream_descriptor) | Above | 2 days |
| 2.8 | All existing provider/mcp tests green | `tests/test_schema_provider_*`, `test_rate_limited_provider.cpp` | Included |

**Completion Criteria**:
- All existing tests green (async internal, sync wrapper calls).
- New test: thousands of provider calls execute concurrently on same io_context.
- Examples still work via synchronous facade — no changes.

### Semester 3 — Async CheckpointStore + Engine Coroutine (2.5 weeks)

Goal: GraphEngine operates via coroutine. Postgres backend utilizes pipeline async mode.

| # | Task | File | Estimated |
|---|---|---|---|
| 3.1 | `CheckpointStore::save_async / load_async` (pure virtual) | `include/neograph/graph/checkpoint.h` | 0.5 day |
| 3.2 | InMemoryStore / SQLite async wrapper | `src/core/graph_checkpoint.cpp`, `src/core/sqlite_checkpoint.cpp` | 1 day |
| 3.3 | libpq pipeline async mode —drop libpqxx | `src/core/postgres_checkpoint.cpp` rewrite | 5 days |
| 3.4 | Add `GraphNode::execute_async` (Task<NodeResult>) | `include/neograph/graph/node.h` | 1 day |
| 3.5 | Built-in node 4 types async implementation (LLMCall, ToolDispatch, IntentClassifier, Subgraph) | `src/core/graph_node.cpp` | 2 days |
| 3.6 | `GraphEngine::run_async` — coroutine conversion | `src/core/graph_engine.cpp`, `graph_executor.cpp` | 3 days |
| 3.7 | Taskflow fan-out → asio::experimental::parallel_group | `graph_executor.cpp` | 2 days |
| 3.8 | bench_neograph re-measurement — regression zero confirmed | `benchmarks/bench_neograph.cpp` | 1 day |

**Completion Criteria**:
- Both `run_async()` and `run()` exist. Synchronous is wrapper.
- Postgres checkpoint 64-thread bench results maintained or improved.
- All 26 examples — nothing changes (thanks to synchronous facade).
- New test: 10K concurrent run() completes within 2GB RAM.

### Semester 4 — Migration, Tool Async, Cleanup (1.5 weeks)

Goal: Establish migration path where users can actually benefit from async.

| # | Task | File | Estimated |
|---|---|---|---|
| 4.1 | Async-ify 1-2 high-concurrency examples from examples (e.g. 05_parallel_fanout, 26_postgres) | `examples/` | 1 day |
| 4.2 | Helper for async offload of Tool — `AsyncTool` adapter | `include/neograph/tool.h` | 1 day |
| 4.3 | Documentation — async guide, migration checklist | `docs/ASYNC_GUIDE.md` new | 1 day |
| 4.4 | NEXT_SESSION.md / README update | — | 0.5 day |
| 4.5 | Final decision on Taskflow dependency removal | — | 0.5 day |
| 4.6 | CI bench_async_* regression gate | `.github/workflows/` | 1 day |
| 4.7 | Major version bump → 2.0.0 | `CMakeLists.txt` etc. | 0.5 day |

**Completion Criteria**:
- Merge to master. Branch ends.
- `NeoGraph 2.0` release notes with breaking change list.
- All 26 synchronous examples green, 1-2 new async examples added.

---

## 4. Breaking Change Matrix

| API | Change | Migration Cost | Resolution Strategy |
|---|---|---|---|
| `Provider::complete` | Synchronous maintained, `complete_async` added | None | Declare two pure virtuals, connect mutually via default implementation |
| `GraphNode::execute` | Synchronous maintained, `execute_async` added | Custom nodes unaffected | Same |
| `GraphEngine::run` | Synchronous maintained, `run_async` added | None | Sync facade |
| `CheckpointStore::save` | Synchronous maintained, `save_async` added | Custom stores unaffected | Same |
| `PostgresCheckpointStore` | libpqxx → direct libpq | No API change for users | Internal replacement |
| `MCPClient::rpc_call` | Synchronous maintained, `rpc_call_async` added | None | Sync facade |

**Conclusion: All synchronous APIs remain even after migration**. 2.0 bump is due to internal dependency (libpqxx drop) + C++20 coroutine requirement.

---

## 5. Risk Register

| Risk | Impact | Mitigation |
|---|---|---|
| libpq pipeline mode unsuitable for checkpoint write pattern | Semester 3 delay | Verify with 2-day spike early in Semester 3. If unsuitable, fallback to libpq synchronous calls + `asio::post(thread_pool)` — achieves half performance gain but completion possible |
| asio::ssl + Anthropic/OpenAI endpoint ALPN issue | Semester 1 delay | Stage 2 bench used HTTP only. Run real endpoint smoke test early in Semester 1 |
| Taskflow ↔ asio executor integration difficulty | Semester 3 extension | Keep Taskflow, make only parallel node internals coroutine — complete removal is Semester 4 optional |
| Sync facade `run_sync(coro)` deadlock (single-thread io_context environment) | Runtime bug | User facade always wrapped with guard ensuring at least 1 worker thread in io_context |
| Testing both sync/async paths → test count doubles | Maintenance cost | Define parameterized test once, automatically run both paths |
| Example 26 regressions | Release delay | Examples not async-converted (Semester 4 only optional). OK if sync facade passes |
| Shared diagram annotation | Documentation consistency | Translate to English in diagram annotation |

---

## 6. Validation Gates (Common to All Semesters)

At each semester completion, must:

1. `cmake --build build -j` — warnings 0.
2. `ctest -j` — all 172+ tests green.
3. `benchmarks/bench_neograph` — existing three metrics (single run µs, 1-thread PG, 64-thread PG) within 5% regression.
4. `benchmarks/bench_async_http` — equal or better than Stage 2.
5. `benchmarks/bench_async_fanout` — 50K timers maintained 6×.
6. ASan / TSan builds green (`build-asan`, `build-tsan` already exist).
7. Commits use `feat(async)` prefix + Co-Authored-By.

---

## 7. Schedule Summary

| Week | Semester | Key Deliverable |
|---|---|---|
| W1 | 1 | TLS + keep-alive + SSE |
| W2 | 1→2 | Bench re-measurement, Provider async start |
| W3 | 2 | SchemaProvider, MCP async |
| W4 | 2→3 | CheckpointStore async, libpq rewrite start |
| W5 | 3 | libpq pipeline, built-in node async |
| W6 | 3 | Engine coroutine, executor conversion |
| W7 | 3→4 | Bench regression gate |
| W8 | 4 | Examples 1-2, documentation, CI gate |

Total **8 weeks** — midpoint of 6-10 week range. 1-week expansion available in W5/W6 if risk mitigation needed.

---

## 8. Final Check Before Starting

Confirm at next session entry:

- [ ] Continue on `feat/async-api` branch? Or branch to `feat/async-stage3`? → **Recommend continuous this branch**. Preserve PoC → Stage 3 continuity.
- [ ] Determine Semester 1 spike target host (api.openai.com vs. api.anthropic.com TLS behavior difference).
- [ ] Pre-review libpq pipeline mode documentation.
- [ ] Initial stance on Taskflow removal vs. maintenance (default: maintain, re-evaluate Semester 4).

---

**Next Action**: Semester 1.1 start — Add asio::ssl layer to `src/async/http_client.cpp`.
