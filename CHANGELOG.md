# Changelog

All notable changes to NeoGraph are documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Fixed

- `openinference_tracer`: silence the `Failed to detach context`
  stderr traceback that OTel's SDK emitted on every shutdown when
  the tracer was used with `engine.run_stream_async` +
  `StreamMode.ALL`. The OTel contextvars token created at NODE_START
  was being detached from a different `asyncio.Task` (NODE_END
  callback fires from the engine's continuation, not the caller's
  task), so `Context.reset(token)` raised `ValueError`; the SDK
  swallowed the raise but still routed the full traceback through
  `logger.exception`, polluting production logs without affecting
  semantics. Fix records the (thread, task) at attach and skips
  detach on mismatch, plus installs a narrow `logging.Filter` on
  `opentelemetry.context` that drops the message only while our
  `_safe_detach` is on the stack. Sync callers and same-task async
  callers still get proper LLM-span nesting under the node span.
  (Issue #2)

---

## [0.6.0] — 2026-05-07 — OpenInference observability layer

Closes the LangSmith UX gap. NeoGraph already emitted OTel-shape
spans (so traces flowed to any OTel backend); this release adds the
LLM-specific attribute layer that Phoenix / Arize / Langfuse use to
render the trace as a chat-bubble + token-counts UI instead of a
flat generic-application span list. Verified end-to-end against a
local Phoenix container — writer→critic graph produces a 6-span
hierarchy (CHAIN root → node spans → LLM spans) with model name,
prompt/response, and token counts visible in the Phoenix UI.

### Added

- `neograph_engine.openinference` module:
  - `openinference_tracer(tracer)` — context manager that mirrors
    `otel_tracer` but tags root + node spans with
    `openinference.span.kind = "CHAIN"` and stuffs node payload
    into `input.value` / `output.value` JSON blobs.
  - `OpenInferenceProvider(inner, tracer)` — wraps any `Provider`.
    On every `complete()` opens an `llm.complete` child span tagged
    `span.kind = "LLM"`, capturing `llm.model_name`,
    `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`,
    `llm.output_messages.0.message.{role,content}`,
    `llm.token_count.{prompt,completion,total}`, and the Langfuse-
    compatible `input.value` / `output.value` blobs.
- 4 tests in `bindings/python/tests/test_openinference.py` —
  InMemorySpanExporter assertions on attribute presence, span
  hierarchy, exception path, and node-input/output JSON blobs.

### Fixed

- `openinference_tracer` now attaches each node span as the OTel
  *current* context (via `otel_context.attach`) so child LLM spans
  opened inside the node body nest under their node span. Without
  this, contextvar propagation across the C++→Python pybind callback
  boundary produced 3+ unrelated trace_ids per run instead of the
  expected single trace tree. The token is detached on NODE_END /
  ERROR / INTERRUPT to restore the prior current span. Same pattern
  the existing `otel_tracer` documents — explicit attach/detach
  rather than `trace.use_span(...).__enter__()` which is unsafe to
  use without a matching `__exit__`.

### Notes

- OpenTelemetry remains an opt-in dependency. Importing
  `neograph_engine.openinference` raises a clear ImportError on
  first use only if `opentelemetry-api` isn't installed; not at
  import time.
- For a Phoenix end-to-end run::

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

  Configure the OTLP gRPC exporter to `http://localhost:4317` and
  open `http://localhost:6006` to view traces. The module docstring
  has the full snippet.

---

## [0.5.0] — 2026-05-07 — Binding ergonomics: live-mutation list properties

Closes a silent-no-op trap on the most-natural Python idiom for
mutating message / writes / sends lists exposed via the binding.
Previously `params.messages.append(msg)` mutated a copy and the
underlying C++ vector never saw the new item — graceful failure (no
crash, no warning) that produced degraded LLM replies. Now `.append()`
pushes through to the live std::vector.

### Added

- `bindings/python/src/opaque_types.h` — `PYBIND11_MAKE_OPAQUE` for
  five vector types: `std::vector<ChatMessage>`, `<ChatTool>`,
  `<ToolCall>`, `<graph::ChannelWrite>`, `<graph::Send>`.
- `module.cpp` `init_opaque_vectors` — `py::bind_vector` registers
  each as a Python class (`ChatMessageList`, `ChatToolList`,
  `ToolCallList`, `ChannelWriteList`, `SendList`) supporting the
  full mutable-sequence protocol against the live C++ vector.
- `py::implicitly_convertible<py::list, …>` for each — the legacy
  build-then-assign pattern (`params.messages = [m1, m2]`) keeps
  working unchanged; assignment auto-converts a Python list into
  the bound class.
- `bindings/python/examples/23_evolving_chat_agent.py` — per-thread
  evolving chat agent (live LLM): the agent's JSON definition is
  rewritten between turns based on accumulated conversation history.
  Demonstrates checkpoint-resume across evolution (prior messages
  survive), the `__graph_meta__` audit channel pattern, and a
  validator boundary (whitelist node types, required channels).

### Changed

- `params.messages` / `.tools` / `chat_message.tool_calls` /
  `node_result.writes` / `.sends` now return their bound class
  instead of a plain `list`. `len()`, iteration, `__getitem__`,
  `__setitem__`, `.append()`, `.extend()`, slicing — all behave
  like a Python list. Only `isinstance(x, list)` returns False.
  Repo + downstream grep confirms zero such isinstance call sites.
- `.github/workflows/nightly.yml` — drop the `ops/s ≥ 600K` gate.
  After 4 consecutive failures with `err=0` and `leak=false`, the
  threshold (calibrated against local hardware at 969K ops/s) was
  unreachable on shared GitHub-hosted runners (measured 233~273K
  ops/s, 3-4× below local). Throughput regression detection lives
  in the PR-time `bench-regression` job (stable hardware, single-
  shot dispatch in µs). The nightly soak's actual value is
  `err==0` + `leak_suspect==false` over 5 minutes — both kept as
  hard gates.

### Notes

- `ChatMessage.image_urls` (`std::vector<std::string>`) intentionally
  not migrated — `vector<string>` is used too widely across the
  binding for a global OPAQUE without sweeping every callsite.
  Documented as a remaining limitation; v0.6+ candidate.

---

## [0.4.0] — 2026-05-05 — v1.0 prep: unified `run(NodeInput)` dispatch

The opening release of the v1.0 sharpening track (ROADMAP_v1.md).
The 8-virtual `GraphNode` cross-product (`execute` / `execute_async` /
`execute_full` / … / `execute_full_stream_async`) collapses to a
single canonical method: `run(NodeInput) -> awaitable<NodeOutput>`.
Per-run metadata (cancel token, deadline, trace_id) moves from a
non-channel-set `GraphState` member + a thread-local smuggling
channel into an explicit `RunContext` argument. `CancelToken` gains
hierarchical `fork()` so multi-Send fan-out workers each own a
private signal that the parent's `cancel()` cascades to.

### Added

- `RunContext` (`include/neograph/graph/engine.h`) — explicit
  per-run metadata: `cancel_token`, `deadline`, `trace_id`,
  `thread_id`, `step`, `stream_mode`. Engine threads through every
  `NodeExecutor::run` call. **PR 1, commit `a473f0e`.**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — single
  canonical dispatch entry point. `NodeInput { state, ctx,
  stream_cb }`; `NodeOutput { writes, command, sends }`. Default
  body forwards to the legacy 8 virtuals so existing subclasses
  keep compiling. **PR 2, commit `607ce66`.**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — child token
  with its own `cancellation_signal`. Parent `cancel()` cascades
  to all live children (and to grandchildren recursively).
  `run_sync(aw, parent_token)` switches to `parent_token->fork()`
  so each nested op binds its own slot — closes the v0.3.x emit-
  vs-bind race and the multi-Send single-handler overwrite. The
  v0.3.x `add_cancel_hook` list keeps working through deprecation.
  **PR 3, commit `897645c`.**
- `[[deprecated]]` on the 8 legacy `GraphNode` virtuals + `add_cancel_hook`.
  Internal call sites (graph_node.cpp default chain, default
  `run()` forwarder) bracketed by new
  `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` macros (`api.h` — GCC /
  clang / MSVC portable). User code overriding deprecated virtuals
  sees migration warnings; engine internals stay clean.
  **PR 4, commit `35a4517`.**
- `engine.get_state_view(thread_id) -> StateView` is now the
  canonical state read; raw-dict `engine.get_state(...)` soft-
  deprecated in the docstring (no warning emitted — raw dict
  remains a valid escape hatch). **PR 5, commit `f31aa53`.**
- 7 C++ + 19 Python examples migrated to `run(NodeInput)`. Smoke
  runs match v0.3.2 outputs bit-for-bit. **PR 6a/6b, commits
  `a2a24ef` / `0a76e3a`.**
- Pybind `PyGraphNodeOwner` overrides `run(NodeInput)` and
  dispatches to a Python user's `run` method (when defined),
  falling through to the legacy chain otherwise. `RunContext` /
  `NodeInput` / `CancelToken` exposed to Python; `cancel_token`
  reachable as `input.ctx.cancel_token` without the thread-local
  smuggle. **PR 7, commit `4e186a5`.**
- `docs/reference-en.md` §6 GraphNode collapsed to a single `run()`.
  RunContext + `fork()` example subsections added under §7.
  README "Differences from LangGraph" picked up a "One node method"
  entry. **PR 8, commit `519a00b`.**
- Built-in C++ nodes (`LLMCallNode`, `ToolDispatchNode`,
  `RouteToNode`) migrated to `run(NodeInput)` overrides.
  **PR 9a, commit `d1070dc`.**
- Newcomer-mode trap fixes: README CMake snippet documents
  `graph::` sub-namespace, cppdotenv path, `OpenAIProvider::create()`
  vs `create_shared()`, `neograph::json` as nlohmann subset,
  3-arg vs 2-arg `compile()`. Python `compile(def, ctx, store=None)`
  keyword-arg added (additive, non-breaking). **commit `ee11ed6`.**

### Changed

- README: "10K-worker measured stress test" section — RTX 4070 Ti +
  Gemma 4 E2B Q4 on neoclaw, N=10000 done @ 0 err / 424s / 2572 MB
  peak / ~1 KB marginal worker cost / p99 648 ms (`7840b81`).
- README: "Production economics" section — fleet safety + RAM delta
  framing (`b82b15a`).
- README: "No Docker required" + "Dependency-drift immunity"
  bullets in the LangGraph delta list (`333b482`, `a6061d7`).

### Deprecated

- `GraphNode::execute / execute_async / execute_full /
  execute_full_async / execute_stream / execute_stream_async /
  execute_full_stream / execute_full_stream_async` — kept working
  with `[[deprecated]]` annotation through v0.5.x, removed in v1.0.
- `CancelToken::add_cancel_hook` — replaced by `fork()`. Same
  deprecation window.

### Notes

- Validation: 442 → 452 ctest (3 NodeRunDispatch + 7 CancelTokenFork
  added) + 96 pytest + 5 live LLM/WS green at the v0.4.0 tag.
- A sub-PR (`run(const NodeInput&)` reference param) tripped the
  v0.2.0 RunConfig coroutine-reference UAF crash shape under the
  pybind async path. Fix landed before merge: `NodeInput in` by
  value. Documented in `node.h`.

---

## [0.3.2] — 2026-05-05 — Cancel propagation hardening (5 rounds)

Five-round patch series closing the gaps the v0.3.0 single-shot
cancel uncovered: Send fan-out propagation, in-process polling,
hooks for Python, C++ scope, exception typing. Also lands the
TODO_v0.3.md feedback batch from the FastAPI SSE chat-demo
evaluation — `resume_if_exists`, dict-or-list `update_state`,
StateView for typed state reads.

### Added

- `RunConfig::resume_if_exists` — opt-in resume of a prior
  thread's checkpoint without explicit `resume()` call. Standard
  multi-turn chat semantics: `engine.run(cfg)` continues the
  conversation if `thread_id` exists.
- `engine.update_state(thread_id, dict | list[ChannelWrite],
  as_node="")` — accepts both shapes. Pre-fix only `dict` worked;
  passing a list silently no-op'd. List form is symmetric with
  every node body's emit shape.
- `StateView` (`bindings/python/neograph_engine/state_view.py`) —
  Pydantic-typed state read. `engine.get_state_view(thread_id) ->
  StateView` returns flat dot-access (`view.messages` /
  `view.foo`) plus `view.raw` for the dict escape hatch.
  Subclass for typed channel definitions:
  `class ChatState(ng.StateView): messages: list[dict] = []`.
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` —
  asserts mid-flight cancel really aborts every Send-spawned
  sibling at the socket layer (was the v0.3.1 root-cause patch).
- `examples/22_self_evolving_graph.py` — moved to v0.3.2 with the
  TODO_v0.3.md #9 cookbook fold.
- ROADMAP_v1.md — design-sharpening candidates derived from the
  cancel-rounds post-mortem (single dispatch, RunContext, hierarchical
  CancelToken — all delivered in v0.4.0).
- Doxygen `/* */` wildcard fix — `acp/types.h` had `/**` blocks
  containing path wildcards (`fs/*`, `terminal/*`) that opened a
  nested comment + suppressed all subsequent diagnostics. Replaced
  with `&#42;` HTML entity.

### Fixed

- Cancel propagation, 5 cumulative rounds:
  1. v0.3.0 single-node — `cancel_token` reaches `Provider::complete`.
  2. v0.3.1 multi-Send pointer drop — fan-out workers now share
     `run_cancel_token_shared()` (was lost when `init_state +
     restore` rebuilt per-worker state outside the channel set).
  3. v0.3.1+ in-process polling — engine super-step loop polls
     between steps, not just at LLM I/O.
  4. v0.3.2 hooks for Python — `add_cancel_hook` registers a
     callback on the per-run token, fires on `cancel()`. Lets sync
     Python `execute()` install ad-hoc cancel handlers without
     the thread-local scope.
  5. v0.3.2 C++ scope + retry + exception typing — fresh-throw
     `NodeInterrupt` on the main thread (avoids libstdc++
     `__exception_ptr::_M_release` race), retry budget honours
     cancel, runtime-vs-logic exception split.
- `execute_stream`-only Python nodes silently fell through to the
  default `execute` path (NotImplementedError). Now `run_stream`
  wires `execute_stream` directly when the user only overrode the
  streaming variant.
- `update_state` accepting list[ChannelWrite] — closes the silent
  no-op (TODO_v0.3.md #5).

### Notes

- 442 ctest + 96 pytest + 2 live LLM (single + fanout cancel)
  green at v0.3.2 tag (`915e90e`).
- 27/30 C++ examples + 20/22 Python examples pass under
  `examples/run_all.py`. Skipped tests need external services
  (Postgres / Crawl4AI / live OpenAI).
- Valgrind 6 examples 0 errors, 815 allocs / 815 frees clean.
- Bench median 5.185 µs/iter on the seq path (v0.3.0 baseline) —
  zero perf regression across the round.

---

## [0.3.0] — 2026-05-04 — Cooperative cancel propagation

Closes the production cost-leak gap reported during the FastAPI SSE
chat-demo evaluation: a frontend `AbortController` cancelling the
asyncio task no longer leaves the upstream OpenAI request running to
completion. Cancel propagates through every layer of the run.

### Added

- `neograph::graph::CancelToken` (atomic flag + asio
  `cancellation_signal`) and `CancelledException` —
  `include/neograph/graph/cancel.h`. Cooperative cancel primitive.
  Pass via `RunConfig::cancel_token` (optional `shared_ptr`); the
  engine super-step loop polls `is_cancelled()` between steps and
  bails with `CancelledException`. The token's `cancellation_slot()`
  binds to the run's `co_spawn` so an in-flight LLM HTTP socket op is
  aborted on the wire (asio `operation_aborted`).
- `CompletionParams::cancel_token` — explicit pin for users threading
  abort across multiple `provider.complete()` calls. `Provider::complete`
  reads it (or falls back to the thread-local
  `current_cancel_token()` set by `PyGraphNode::execute_full_async`)
  and binds the slot to its inner `run_sync` io_context, so even sync
  Python nodes hit by a cancel stop billing.
- `GraphState::run_cancel_token()` — per-run, non-serialized handle
  used by the pybind `PyGraphNode` to install a
  `CurrentCancelTokenScope` around the synchronous Python `execute()`
  call. This is what gives sync Python users transparent cancel
  propagation without changing their node code.
- pybind `engine.run_async` / `run_stream_async`: asyncio
  `Future.cancel()` now wires through `add_done_callback` to
  `CancelToken::cancel()`, and the `co_spawn` binds the token's
  cancel slot.
- pybind safe-resolve helpers `_safe_set_future_result` /
  `_safe_set_future_exception` — guard `future.set_result` /
  `set_exception` calls posted via `call_soon_threadsafe` against
  cancelled-future `InvalidStateError` storms.
- `bindings/python/tests/test_async_cancel_live_llm.py` — live
  OpenAI E2E asserting OpenAI HTTP completes within < 3 s of
  `Future.cancel()` (in practice immediate; pre-fix was ~7–8 s of
  uncancelled streaming). Skipped unless `NEOGRAPH_LIVE_LLM=1`.
- `examples/22_self_evolving_graph.py` — self-evolving graph PoC:
  `prompted_llm` node reads its own prompts from JSON config so an
  LLM rewriter can mutate the graph definition between runs and
  recompile. Demonstrates `0.0 → 0.4` score improvement; documents
  the channel-flow reasoning gap in the rewriter.

### Changed

- `Provider::complete(params)` now binds an inner cancellation slot
  to its `run_sync` when `params.cancel_token` is set OR when a
  thread-local `current_cancel_token()` is active. Previous default
  behaviour (no cancellation) is preserved for callers that don't
  opt in.
- `neograph::async::run_sync` gained an optional
  `graph::CancelToken*` parameter; when non-null the bound spawn
  binds the token's slot.
- pybind `resolve_future_async` routes through the safe-resolve
  helpers instead of calling `future.set_result` directly via
  `call_soon_threadsafe`.

### Roadmap (deferred to v0.3.x — see `TODO_v0.3.md`)

- LangGraph-style auto checkpoint resume on same `thread_id`.
- Streaming-only-node hint in `run_async` error message.
- `cb.emit_token(node, data)` ergonomic helper.
- README "Differences from LangGraph" section.
- `update_state` signature alignment with docs.
- `get_state` flat helper / Pydantic accessor.
- Live verification of cancel propagation in `run_parallel_async`
  and `run_sends_async` branch fan-outs.
- pgvector RAG example.

---

## [Unreleased] — Stage 4

Stage 4 closes the last `run_sync` hop on the async path. `run_async`
now stays on the caller's executor end-to-end: three 50 ms agents
on one `io_context` thread drop from ~150 ms (serial) to ~50 ms
(overlapping) in `examples/27_async_concurrent_runs`.

### Breaking

- **`GraphNode::execute_full_async` default flipped to async-first.**
  It now wraps `co_await execute_async(state)` into a `NodeResult`
  instead of calling sync `execute_full(state)`. Any subclass that
  emits `Command`/`Send` only from a sync `execute_full` override
  MUST add a one-line `execute_full_async` bridge:
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
  Without the bridge, `Command`/`Send` are silently dropped on the
  async path — the 2.0 latent dispatch bug that 3.0 fixed by routing
  through sync at the cost of an `io_context` spawn per super-step.
  All in-tree subclasses (`deep_research_graph`, examples 10/14/21,
  tests 5 sites) now carry the bridge.

### Performance

- Example 27 wall time: **152 ms → 53 ms** (3 agents × 50 ms timer
  step on one `io_context` thread, full overlap).
- No measurable regression on single-run benchmarks; `run()` still
  drives the same coroutine through a fresh single-threaded
  `io_context` via `run_sync`.

### Tests

- 341/341 ctest green
- 295/295 ASan+UBSan green
- Valgrind clean on coroutine-heavy subset (20 tests, 2.4 s)

### Post-release validation (same day)

- **All 30 examples re-run:** 26/29 PASS, 0 FAIL, 3 environment-gated
  (clay_chatbot → raylib, postgres_react_hitl → docker compose,
  deep_research full loop → crawl4ai service). `21_mcp_fanout`
  measured at 3 MCP calls / 8 ms wall — Stage 4 overlap holds under
  real network I/O.

- **ARM64 compatibility (docker buildx --platform linux/arm64):**
  `Dockerfile.arm64-smoke` at repo root. ubuntu:24.04-arm64 +
  core+llm+async+sqlite+tests build under QEMU emulation completes
  in ~15 min; **306/306 ctest green** on ARM64. Stripped binary sizes
  0.81-0.88 MB (nearly identical to x86_64). example 27 runs in
  65 ms under emulation (native x86_64: 53 ms). Confirms Linux/ARM64
  as a supported target alongside macOS beta (Apple Silicon).

- **Cache locality (Ryzen 5800X / Zen 3, Valgrind cachegrind,
  32 KB L1i/d 8-way, 32 MB L3 16-way):**
  `bench_concurrent_neograph` sweep N=1 → 10,000.

  | N | I refs | LLi misses | LLi miss% | Native p50 |
  |---:|---:|---:|---:|---:|
  | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
  | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
  | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

  Last-level instruction misses stay flat at ~4,320 across 4 orders
  of magnitude of N. Unique hot code working set ≈ 277 KB (0.85% of
  L3). 648 M instructions at N=10,000 incur only 4,329 LL misses —
  roughly 1 miss per 150,000 instructions. Native p50 drops from
  17 µs to 5 µs purely from I-cache warming. First measured evidence
  for the "burst concurrency robustness" positioning.

---

## [3.0.0] — 2026-04-22

3.0 removes the Taskflow dependency and unifies sync and async
super-step execution on a single asio coroutine path. Graph-definition
JSON, node ABI, checkpoint schema, and public entry points (`run`,
`run_async`, `run_stream`, `resume`) are source-compatible with 2.0;
the break is confined to `GraphNode` subclasses that emit
`Command`/`Send` from the **sync** `execute_full` override only.

### Breaking

- **`deps/taskflow/` and the Taskflow INTERFACE target are gone.**
  The sync super-step loop, `run_one`, `run_parallel`, `run_sends`,
  and the process-wide `tf::Executor` static are deleted. Downstream
  consumers that `#include <taskflow/...>` via NeoGraph's include
  path must vendor Taskflow separately.
- **`GraphNode::execute_full_async` default now bridges to the sync
  `execute_full` via direct call (no `co_await execute_async`).**
  This preserves `Command`/`Send` emitted from a sync-only override
  — the common 2.0 pattern — through the async path that all entry
  points now share. Async-native nodes that need non-blocking I/O
  AND `Command`/`Send` must override `execute_full_async` directly;
  the docstring has said this since 2.0, but 2.0 never exercised it
  because sync `run()` bypassed the coroutine path entirely.
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` sync
  methods removed.** Use the `_async` peers.
- **CPU parallel fan-out is opt-in.** Previously Taskflow provided a
  process-wide thread pool by default. In 3.0 `run_parallel_async`
  and the multi-Send branch of `run_sends_async` dispatch branches
  on whichever executor drives the coroutine — the single-threaded
  io_context spun up by sync `run()`, or the caller's own executor
  for `run_async()`. I/O-bound fan-out still overlaps (co_await
  suspension on a single thread); CPU-bound fan-out serializes
  unless the caller uses a multi-threaded executor for `run_async()`
  or opts into an engine-owned pool via `engine->set_worker_count(N)`.

### Added

- `neograph::async::run_sync_pool(awaitable, n_threads)` — N-worker
  sync↔async bridge alongside the existing single-threaded
  `run_sync`. Spins a fresh `asio::thread_pool` for the call so
  inner `make_parallel_group` branches execute on separate workers.
- `GraphEngine::set_worker_count(n)` — opt-in engine-owned
  thread_pool used by `NodeExecutor` for parallel fan-out dispatch.
  Rebuilds the executor; must be called before any concurrent run.

### Changed

- `GraphEngine::execute_graph` (sync) is gone. All entry points
  (`run`, `run_stream`, `resume`) route through
  `execute_graph_async` via `neograph::async::run_sync`, so the
  super-step loop, retry backoff, checkpoint I/O, and parallel
  fan-out now live on one coroutine path end-to-end.
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` switched
  from `tf::Executor` / `tf::Taskflow` to `asio::thread_pool` +
  `asio::post` for the caller-side driver.

### Perf (bench_neograph Release -O3 -DNDEBUG on reference Linux, 10-run median)

- `seq` engine overhead (3-node chain, counter): **~5.0 µs** per call.
- `par` engine overhead (5-worker fan-out + summarizer): **~11.8 µs**
  per call.
- Peak RSS of the whole bench process (warm-up + seq + par iters):
  **4.8 MB**.
- vs LangGraph 1.1.9 on the same workload: **131× faster seq, 199×
  faster par** per iteration; RSS ~12× lighter.

Prior drafts of this CHANGELOG listed "~46 µs seq / ~114 µs par"
as a 3.0 regression. Those numbers came from a build tree where
`CMAKE_BUILD_TYPE` was unset, so the bench binary was compiled
without `-O3 -DNDEBUG`. On a proper Release build the async-peer
collapse is a **win** vs 2.0's Taskflow sync path (which the 2.0
README advertised at 20.65 µs seq / 150.7 µs par on the same
host). The corrected chart is at
[`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png).

### Migration

- No action needed if your nodes override `execute()` / `execute_async()`
  and don't emit `Command` / `Send`.
- If you override sync `execute_full` to emit `Command` / `Send`:
  no change required — the 3.0 async-path default now calls your
  sync override directly. `Command.goto_node` routing works via
  sync and async entry points alike.
- If you override `execute_async` (async-native I/O) AND want
  `Command` / `Send`: override `execute_full_async` directly and
  assemble `NodeResult` there. Overriding only `execute_async`
  silently drops `Command` / `Send` because the default
  `execute_full_async` now routes through sync `execute_full`, not
  async `execute_async`.
- If you relied on Taskflow's process-wide pool for CPU parallel
  fan-out via `engine->run()`: call `engine->set_worker_count(N)`
  once after compile(), or drive the engine via `run_async()` on
  your own multi-threaded `asio::thread_pool` / io_context.

---

## [2.0.0] — 2026-04-22

First public release with the Stage 3 async API. This is a breaking
release; the changes below affect compilation (C++ standard) and
ABI (abstract base classes gained async peers). Sync call sites are
preserved bit-for-bit, so **application code that doesn't override
`Provider` / `CheckpointStore` / `GraphNode` / `Tool` continues to
work unchanged**.

### Breaking

- **C++20 required.** The public API exposes `asio::awaitable<T>`
  return types that need `std::coroutine` support. Consumers must
  compile with `-std=c++20` (or higher). GCC 13+, Clang 15+ tested;
  see `docs/ASYNC_GUIDE.md` §4.1 for GCC 13 coroutine workarounds.
- **libpqxx dependency dropped.** `neograph::postgres` now links
  libpq directly. Ubuntu 24.04 users no longer hit the
  `pqxx::argument_error::argument_error(..., std::source_location)`
  link error introduced by libpqxx-7.8t64's C++17/C++20 ABI split.
  CMake find now targets `PostgreSQL::PostgreSQL` (CMake-bundled
  FindPostgreSQL). Consumers who installed only `libpqxx-dev`
  must now also install / retain `libpq-dev`.
- **`Provider`, `CheckpointStore`, `GraphNode`, `MCPClient` ABIs
  extended.** Each grew async peer virtual functions
  (`complete_async`, `save_async`, `execute_async`, `rpc_call_async`
  and their variants). Downstream subclasses recompile against the
  2.0 headers; source is unchanged unless the subclass wants to
  provide a native async override (recommended for any implementor
  that does real I/O).
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list`
  / `delete_thread` are no longer pure virtual.** They now have
  default implementations that bridge to the matching `_async`
  peer via `neograph::async::run_sync`. Subclasses that override
  the sync side keep working; subclasses that didn't provide any
  override (which would have been a compile error before) now
  infinitely recurse — contract: override at least one of each
  sync/async pair.

### Added

- **Async API** across all I/O layers
  (`docs/ASYNC_GUIDE.md` for full reference):
  - `Provider::complete_async` on the base class and all built-in
    providers (OpenAI, Schema, RateLimited).
  - `MCPClient::rpc_call_async` for both HTTP and stdio
    transports. stdio uses `asio::posix::stream_descriptor`.
  - `CheckpointStore::*_async` for all eight sync methods.
  - `GraphNode::execute_async` + stream / full / full_stream
    variants, with async-native crossover defaults.
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async`
    driving `execute_graph_async` — an end-to-end coroutine super-
    step loop including parallel fan-out via
    `asio::experimental::make_parallel_group`.
  - `neograph::AsyncTool` adapter for user tools that want a
    coroutine body while preserving the sync `Tool` interface.
- **`neograph::async` namespace** — HTTP client, connection pool,
  SSE parser, run_sync bridge, URL endpoint splitter. See
  `include/neograph/async/*.h`.
- **New examples**:
  - `examples/27_async_concurrent_runs.cpp` — multiple agents on
    one `io_context`.
  - `examples/05_parallel_fanout.cpp` (rewritten) — async fan-out
    within a single graph run using `run_parallel_async`.
- **CI bench regression gate** (`.github/workflows/ci.yml`) —
  PR checks enforce floors on `bench_async_http` / `bench_async_fanout`
  / `bench_neograph`.

### Performance

Measured on the feat/async-api branch against Stage 2 sync baselines:

- `bench_async_http --mode async_pool --concur 1000`:
  6064 ops/s → **17834 ops/s** (2.9×).
- `bench_async_fanout --concur 50000`:
  thread-per-agent unachievable → **541K ops/s / 67 MB RSS**.
- `examples/27_async_concurrent_runs` (3 × 50ms async work):
  150ms (sync) → **50ms** (1 io_context thread).
- `examples/05_parallel_fanout` (3 × 100-150ms async work):
  370ms (sequential) → **150ms** (1 io_context thread).
- `bench_neograph` engine overhead: unchanged (~30 µs seq /
  ~205 µs par). Coroutine machinery does not regress the hot path.

### Not yet in 2.0.0

- **Taskflow dependency** remains. The sync `engine.run()` path
  still uses it for fan-out; Sem 4.5 revisits whether sync paths
  can be replaced by `run_sync(*_async)` so the dependency can
  drop entirely.

### Cross-platform

Three platforms are supported in 2.0.0 at different stability tiers.
The tier reflects how much real-world validation the platform has
seen before release — not feature coverage (the codebase is single-
sourced with `#ifdef _WIN32` splits; features are equivalent across
platforms once tests pass).

#### Linux — **GA** (production-ready)

* Ubuntu 24.04, GCC 13.
* Full 332/332 ctest green locally (Postgres via docker
  `postgres:16-alpine`) plus all benches inside committed CI floors.
* MCP stdio on fork/pipe/execvp + `asio::posix::stream_descriptor`.
* Postgres async peers on libpq nonblocking + `asio::posix::stream_
  descriptor` wrapping `PQsocket`.
* Reference platform for every performance number quoted above.

#### macOS — **beta**

* macos-latest (Apple Silicon), Clang via Xcode.
* CI builds + runs non-Postgres tests; Postgres integration cases
  self-skip without a service container. POSIX paths (same fork/
  pipe + asio::posix code) are exercised.
* `CoreFoundation` + `Security` frameworks linked through httplib
  for system cert loading on TLS.
* Treat as beta until 2-4 weeks of CI runs and user reports
  confirm no runtime-behaviour differences (coroutine scheduling,
  SIGPIPE / EPIPE shape, pipe buffer sizing). Targeted promotion
  to GA once those roll in without incident.

#### Windows — **alpha**

* windows-latest, MSVC 19.44 (VS 2022), x64.
* CI scope: **core + async + MCP + LLM only**. Postgres and
  SQLite backends are disabled on the Windows CI job because
  vcpkg would compile OpenSSL / libpq / zlib / lz4 from source
  on every run (~20 min, no working binary cache backend upstream
  since `x-gha` was removed). Windows users compile these
  locally via their own vcpkg / choco setup.
* OpenSSL via the runner's preinstalled choco package
  (`C:/Program Files/OpenSSL-Win64/`). TLS paths in httplib +
  asio::ssl compile and link.
* MCP stdio: `CreateProcess` + named-pipe (FILE_FLAG_OVERLAPPED) +
  `asio::windows::stream_handle`. The overlapped-pipe path was
  written against MSDN spec without local Windows validation;
  expect first-users to surface edge cases (ERROR_IO_PENDING
  handling, pipe buffer boundary on large JSON responses).
* Postgres async peers (when enabled locally): `asio::ip::tcp::
  socket::assign` wrapping the SOCKET returned by `PQsocket`
  (cast through `native_handle_type` to preserve 64-bit SOCKET
  values). Not exercised by Windows CI — local only.
* Coroutine machinery lives in MSVC's `<coroutine>`; behaviour
  expected to match GCC/Clang by spec but `examples/27` cross-run
  overlap measurements haven't been confirmed on Windows yet.
* Treat as **alpha** through 2.0.0. Promote to beta once one
  production user runs a multi-agent workload for a week without
  hitting stdio/pipe or coroutine-scheduler issues, AND Postgres
  async peers get locally validated by a user willing to run
  vcpkg's full libpq build.

> **Pattern**: CI green is a floor, not a ceiling. Layer 3 runtime
> behaviour differences (coroutine scheduling timing, pipe buffer
> boundaries, socket takeover semantics) only surface under real
> workloads. The tier language above gives users the right
> expectation for each platform rather than pretending all three
> are interchangeable on day one.

### Fixed post-bump

- **`async::HttpResponse` headers map** — the response surface now
  exposes a `headers` vector of `(name, value)` pairs preserving wire
  order and original casing, plus `get_header(name)` as a
  case-insensitive accessor. Retry-After and Location remain as
  dedicated fields for backward compatibility. Unblocks the MCP
  session tracking fix below.
- **MCP `Mcp-Session-Id` header tracking** — the Sem 2.6
  httplib→async_post migration silently dropped this. Every post-
  initialize RPC now echoes the server-assigned session id back
  via the new headers accessor, so the server's session state
  stays routable.
- **MCP stdio awaitable mutex** — `StdioSession::rpc_call_async`
  used `std::mutex`, which deadlocked when two coroutines on the
  same single-threaded io_context called the same session (the
  second's `lock_guard` blocked the worker the first needed).
  Replaced with an `asio::experimental::channel<void(error_code)>`
  capacity-1 semaphore so the second acquirer suspends
  cooperatively.
- **`PostgresCheckpointStore` async peers** — all eight
  CheckpointStore async methods (`save_async`, `load_latest_async`,
  `load_by_id_async`, `list_async`, `delete_thread_async`,
  `put_writes_async`, `get_writes_async`, `clear_writes_async`)
  are now true-async. Internals: `PQsetnonblocking(1)` +
  `PQsendQueryParams` + `asio::posix::stream_descriptor` on
  `PQsocket()` + `co_await sock.async_wait(wait_read/wait_write)`.
  Four concurrent `save_async` calls on a pool of 4 slots now
  commit-fsync in parallel at the wire level rather than
  serialising through `run_sync`.

---

## [0.1.0] — pre-2026-04

Pre-release development. No public API stability guarantees.
