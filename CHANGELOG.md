# Changelog

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

All notable changes to NeoGraph are documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added
- **OpenRouter provider routing.** `OpenAIProvider` now forwards an object
  passed at `CompletionParams::extra_fields.provider` into the Chat Completions
  request body as `provider`; non-object values fail before an HTTP request.
  This exposes OpenRouter's documented per-call routing preferences while
  leaving other native `extra_fields` keys ignored. The live Beast cookbook
  pins its provider and uses an explicit 180-second timeout for its
  4,000-token generation budget.

- **Copy Ninja local graph-node bridge.** Added transport-free
  `a2a::CopyNinjaNode`, which wraps a separately materialized Copy Ninja
  harness, reads `prompt`, and overwrites `response`. Added the
  `cookbook_the_beast_copy_ninja` live cookbook: its LLM may author only this
  fixed local node, must pass a fourth local-binding gate after the normal
  Core gates, and fails if the synthetic source agent observes an RPC. Card
  text, endpoint, credentials, and source remain excluded from the unadmitted
  candidate and the caller prompt never enters the authoring LLM request.


- **Optional Program component boundary.** Added the opt-in
  `NEOGRAPH_BUILD_PROGRAM` switch, exported `neograph::program` target, and
  `<neograph/program/program.h>` entry point. Installed package component
  discovery now reports Program only when built; Core-only installs keep the
  existing `neograph::core` link interface.

- **Immutable Program value model.** Added stable typed diagnostics, deeply
  owned canonical-JSON and C++-builder `ProgramSource` inputs, immutable
  content-addressed `ProgramBundle`/`ProgramVersion` values, canonical
  serialization, SHA-256 algorithm-tagged identities, source maps, imports,
  and strict versioned stored-value schemas. `neograph::program` is now a
  compiled exported library while remaining dependent only on Core.
  Bundle/version v1 projections now require sealed Core definitions and plan
  identities, semantic-versioned executable digests, contracts, closures,
  bounds, and typed admission/materialization receipts. Their identities bind
  the format and storage version, semantic sets use stable ordering, and
  diagnostics reject invalid pointers, reversed spans, and unknown enums while
  leaving parser spans absent when no exact offset is available.

- **Sealed Program admission closure.** Added immutable `RegistrySnapshot`,
  `AdmissionProfile`, and `PolicySnapshot` values with builder-time callable
  capture, strict canonical manifests, domain-separated fingerprints, and
  fail-closed cross-fingerprint validation in `ProgramVersion`. Core now
  exposes explicitly named local-only parse/link/validate entry points for
  Program materialization; existing local-first/global-fallback overloads
  remain unchanged.
  Registry entries now record canonical exact executable dependency edges for
  transitive admission closure, and local-only condition checks cover legacy
  keyed-edge documents without consulting process-global registries.

- **Single-root `call_core` Program compiler.** Added `ProgramCompiler`, which
  accepts only the closed Program-v1 envelope, performs pure local Core
  parse/round-trip/validation before sealing, and emits aggregate typed
  diagnostics with RFC 6901 pointers and source-map attribution. Compilation
  derives the canonical Program, registry, transitive executable closure,
  capability/effect, import Merkle, sealed-definition, and Core plan identities
  without invoking factories or callables. The authored document schema,
  complete finite budget contract, zero-dispatch rejection tests, and static
  and shared installed-consumer coverage ship with the compiler. Core gained
  additive total parse/round-trip and local validation reports while legacy
  throwing APIs retain their existing behavior.

- **Pinned Program runtime vertical slice.** Added `ProgramCatalog`,
  `EngineGenerationCache`, `ProgramRuntime`, shared `ProgramHandle`,
  immutable `ProgramResult`, typed Program event envelopes, in-memory
  `ProgramStore`, and an append-only CAS `ProgramJournal`. Admission recomputes
  untrusted bundle semantics before materialization; each attempt pins one
  immutable Core generation and invokes the existing `GraphEngine` async path.
  Runtime execution now maps completion, interruption, exact-checkpoint resume,
  cancellation, timeout, Core step exhaustion, checkpoint incompatibility, and
  failures to typed terminal states while preserving nonrenewable budget and
  checkpoint lineage. Journal commits precede checkpoint/terminal event
  delivery, concurrent resume has one CAS winner, and the PR6 slice rejects
  effectful or nonempty-schema Programs until the Core broker exists.

- **SQLite Harness record store (issue #147 follow-up).** Added the optional
  `neograph::mcp_sqlite` target and `SqliteHarnessRecordStore` for WAL-backed,
  schema-versioned artifact/run persistence with immutable artifact and run-to-
  artifact bindings. The Harness MCP binary now stores records in `runs.db`,
  while checkpoints remain in `checkpoints.db`.
- **AMD OpenMP target-offload proof of concept.** Added the opt-in
  `bench_openmp_offload` benchmark, which compares serial CPU execution,
  OpenMP auto-threading, per-iteration GPU mapping, and persistent GPU data on
  the same numeric fan-out workload. It reports real-device versus host
  fallback execution, correctness, transfer-inclusive latency, kernel-only
  latency, and speedup. `NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201` enables the
  ROCm/Clang device image for Radeon AI PRO R9700.


### Changed

- **C++ ABI and SOVERSION policy (issue #194).** Every compiled public
  `neograph_*` library now carries the project `VERSION` and major
  `SOVERSION`; installed shared libraries resolve sibling dependencies from
  their own directory. Pre-v1 releases use ABI generation 0 but may declare
  mandatory rebuild boundaries. All C++ consumers built against `0.11.1` or
  earlier must rebuild for the release containing bounded `NodeCache`, because
  `NodeCache` and `EngineConfig` public layouts changed. The release containing
  bounded `UsageAccumulator` reservations is another mandatory rebuild boundary:
  its public object layout now carries reservation accounting state. Version 1.0
  changes the ABI generation to 1 and freezes the supported v1 layouts. CI now
  builds and runs isolated static and shared installed consumers and checks
  ELF/Mach-O loader metadata. See [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md).
- **`GraphNode::run(input)` migration guide complete.** Python `GraphNode` base class
  no longer references deleted `execute*` methods; when `run(input)` is missing it
  raises a `NotImplementedError` containing the migration documentation path.
  C++/Python reference, async/streaming guides, and example READMEs have been
  aligned with the actual v0.9.0 single entry point. Migration procedures are
  documented with C++ and Python examples in
  [`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md).
- **Provider API permanent compatibility policy (issue #5).** The planned removal of
  `Provider::complete()`, `complete_async()`, `complete_stream()`,
  `complete_stream_async()`, and callback-based `invoke()` has been rescinded and
  `[[deprecated]]` warnings have been removed. Existing APIs continue to receive
  compatibility and security fixes. New Provider implementations and direct callers
  are recommended to use `CompletionProvider::do_invoke()` and
  `invoke_request(CompletionRequest)` respectively; backporting all new features into
  existing APIs is not guaranteed. Public signatures, virtual ordering, object size,
  and vtable remain unchanged.

### Removed

- **Deprecated TransformerCPP integration examples.** Removed `example_inproc_gemma`,
  `NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE`, and `TRANSFORMERCPP_DIR`, which depended on
  an externally hosted repository that is no longer available. `example_local_transformer`,
  which uses a standard OpenAI-compatible local server, is retained.

### Fixed

- **Harness aggregate finding provenance (issue #174).** Details now include a
  `finding_sources` array aligned with the existing flat `findings` array.
  Every entry records its aggregate index, source worker ID, and worker-local
  index without changing schema-validated worker output or the established
  `findings` shape.
- **Harness exported result lint (issue #173).** Node effect contracts may now
  declare written channels in an optional `exports` array when callers consume
  them after graph execution. Both Harness compilation and `GraphEngine`
  runtime validation therefore keep E6 for truly write-only channels without
  falsely warning on `final_result`.
- **MCP 2025-11-25 tool-client contract modernization (issue #147 M0).**
  Initialization is now idempotent and retains negotiated server metadata;
  HTTP tools reuse the discovery session; `/mcp` endpoint construction is
  shared by requests and notifications; tool discovery follows opaque cursors;
  and JSON-RPC code/data, full tool metadata, non-text content,
  `structuredContent`, `isError`, and `_meta` survive C++ and Python paths.
  Added configurable HTTP timeout/static/dynamic headers, output-schema
  validation, strict response-ID checking, and typed `InitializeResult`,
  `ToolDefinition`, `ListToolsPage`, and `CallToolResult` APIs. SSE detection now
  uses `Content-Type` rather than misclassifying JSON containing `data:` URLs.
- **Per-task cancel status and published-emit lifetime safety.** `GraphEngine::run`,
  `run_async`, `run_stream`, `run_stream_async` each create one execution child per
  run from the caller-provided parent, bind only that child to the internal
  `co_spawn`/sync bridge, and pass the same child as `RunContext`. Cancelling all
  concurrent runs under a single parent therefore cannot overwrite each other's
  cancellation slots. Forked execution children retain the existing `shared_ptr`
  ownership through the published emit, preventing use-after-free between engine
  work completion and emit execution. asio `operation_aborted` caused by
  cancellation is propagated as `CancelledException` rather than as a retryable
  node error. `CancelToken` 0.11.x object layout and inline/header-only behaviour
  are unchanged. A recompile is required for already-compiled C++ consumers to
  pick up the updated `fork()` lifetime behaviour. Replacing only the shared
  library preserves object layout compatibility, but existing inline function
  bodies embedded in consumer binaries do not change. However, when external
  code calls `bind_executor()` on a token it created directly, the caller still
  bears responsibility for keeping the token alive until the executor's posted
  work completes.
- **PostgreSQL async connection global timeout policy documented.** Async initial
  connection and replacement use a single timeout across all host/IP addresses.
  An explicit `connect_timeout` written directly in a positive connection string
  is enforced with a minimum of 2 seconds; unspecified, zero, negative, or
  environment-variable/service-file-only values use the operationally safe default
  of 30 seconds. This differs intentionally from libpq's per-host synchronous
  timeout; synchronous creation/replacement behaviour is unchanged.
- **JARVIS mock build fix (issue #130).** Fixed `cookbook_jarvis` compilation failure
  caused by `MicCapture` remaining an incomplete type when audio dependencies are
  absent. Added `NEOGRAPH_JARVIS_FORCE_MOCK` so ASan CI always builds the mock
  configuration regardless of the runner's installed packages. The session runner
  now uses the actual CMake output path and specialist target name, and launches
  the existing `demo_mcp_server.py` correctly.
- **Node failure context preservation (issue #123).** C++ execution errors are
  propagated as `NodeExecutionError` containing the original `exception_ptr`,
  failed node name, and attempt count; the terminal `ERROR` event also records
  the same context. In Python the original exception object, type, args, user
  attributes, and traceback are preserved as-is, with only `.node_name` and
  `.attempts` attributes added. `NodeInterrupt`, cancellation, and out-of-memory
  exceptions follow existing control flow without being wrapped.

### Fixed (docs)

- **Removed ignored per-node prompts from Provider cookbook (issue #116).** Fixed
  three Python examples that described multi-role behaviour using `config.system`
  which the built-in `llm_call` does not read. Each example has been rewritten
  as a strict single-call graph using `NodeContext.instructions`, with related
  READMEs aligned with actual behaviour.
- **Reserved `RunContext::deadline` documentation correction (issue #115).** Fixed
  documentation and Doxygen comments that presented `deadline` and `trace_id` as
  usable per-run metadata, when they cannot be set via `RunConfig` and are not
  exposed in Python.
- **`GraphNode::run` example signature fix (issue #129).** Fixed the public header
  example accepting `const NodeInput&` (by reference) which failed to override
  the actual by-value virtual, and locked down the by-value contract required
  for coroutine argument lifetime with a compile-time test.

### Added

- **Backward-compatible Provider migration path.** New `CompletionRequest` separates
  streaming mode from callback presence, and `CompletionProvider` requires new
  implementations to write only `do_invoke()`. The existing `Provider` vtable, four
  legacy virtuals, callback-based `invoke()`, and Python `complete()` subclass
  contract are retained.

- **Python persistence backends** (#117) — `Store` and `CheckpointStore` are
  now constructible subclass bases with C++ virtual dispatch into Python.
  `StoreItem`, `CheckpointPhase`, `Checkpoint`, and `PendingWrite` are exposed
  with JSON-shaped fields; checkpoint pending-write methods remain optional.
- **Python synchronous cancellation** (#119) — Python callers can construct a
  `CancelToken`, assign it to `RunConfig.cancel_token`, and cooperatively stop
  `engine.run()` from another thread.

- **Python checkpoint history** (#118) — `GraphEngine.get_state_history()`
  exposes newest-first checkpoint records so callers can inspect parent links,
  metadata, steps, and IDs before forking from a historical state.

- **DSL surface (elaboration layer) + schema evolution gate** (#75 M4).
  - **Elaborator**: `vars` (`{"$var":...}` / `${...}` interpolation, acyclic
    enforcement) / `templates`+`use` (exact parameter match enforcement, node
    prefix renaming — including local references, barriers, and routes; channels
    are shared state so they merge globally) / `when` conditional inclusion.
    **Non-Turing-complete and total**: every DSL document normalises to a unique
    core in finite time and is idempotent with respect to that core. All errors
    are reported with DSL source coordinates (`use[2].args`, `vars.model`) and a
    source map (output position → generating syntax) is included. Lock-file
    workflow: `./example_elaborate harness.dsl.json > harness.json` (example 53).
  - **`GraphCompiler::upgrade_to_latest()`**: lossless v0→v1 mechanical transform —
    keys that strict rejects are isolated into the `x-upgraded-<key>` comment
    namespace (zero data deletion), empty barriers are explicitly removed. The
    entire corpus is tested to guarantee "legacy permissive compilation IR ==
    post-upgrade strict compilation IR" (canon equivalence, version stamp
    excluded).
  - **Schema evolution gate**: add-only subset judgement against
    `tests/fixtures/schema_snapshot.json` baseline (decidable subset of the JSON
    Subschema family) — removal of node types/properties/reducers/conditions,
    required-set increases, closed-condition label changes, and effect contract
    changes all cause test failure = CI merge block. Incompatible changes force
    version bump + upgrader + snapshot regeneration in the same review commit.

- **PBT / delta verification harness** (#75 M3). 300-seed deterministic topology
  generator (valid strict documents from schema envelope, self-instrumented
  feature coverage — test fails when conditional_edges/barrier/interrupt
  occurrence drops below 30%: untested features become failures, not silent
  holes).
  - **Mutation detection**: confirmed on a 300-seed corpus that translation
    validation catches every application of all 5 drop types
    (conditional_edges/edge/barrier/interrupt/channel) + 3 miswire types
    (route collapse / edge retarget / node rename = drop+fabrication
    counterbalance). Application-rate floor (10% of seeds) also asserted.
  - **Reference interpreter delta**: an independent model re-implementing the
    documented super-step semantics (goto preemption, barrier accumulation,
    lexicographic fallback, implicit __end__) from a code-disjoint
    implementation, compared against the Scheduler on 12-step × 300-graph
    (DESIL lesson: a verifier alone cannot catch wrong-execution).
  - **Engine ↔ Studio shared corpus**: `tests/fixtures/topology_corpus/` 15
    variants (3 valid + 12 violating E3–E11) are byte-identical with
    NeoGraph-Studio `tests/corpus/`, both asserting the same
    verdict (code:severity multiset) — the two implementations cannot silently
    diverge.

- **GraphValidator — topology static semantic checks (E3–E11 + effect)** (#75 M2).
  Pass layer between parsing (M1) and execution. In strict documents
  (schema_version>=1) errors are compilation failures, warnings are stderr
  lints; in permissive documents only error-level diagnostics surface as stderr
  warnings (zero noise on existing graphs). Judgement philosophy = checker
  soundness first: only things that can never be correct under engine semantics
  are errors (dangling reference E3, barrier without signal path E8 — goto
  bypasses barrier accounting so unrecoverable, empty routes E10 — dispatch
  would dereference rend() UB, undeclared channel write E4 — confirmed throw
  at runtime); things that Command.goto/Send can justify are warnings
  (reachability E7, escapeless cycle E11, plain fan-in without barrier E9,
  overwrite race E5, dead channel E6). Every diagnostic is accompanied by a
  machine-readable witness (counterexample) JSON — for Studio canvas
  highlighting (M3).
  - **Route completeness (E10)**: `ConditionSpec` label contract introduced.
    Declaring the output label set of a condition via the `register_condition`
    3-argument overload requires closed-condition routes to match labels
    exactly — uncovered labels fall to the scheduler's "lexicographic last
    route" fallback (order-dependent arbitrary target), which is an error.
    Built-in `has_tool_calls` = closed {false,true}, `route_channel` = open +
    known {default}.
  - **Channel effect contracts**: `register_type` 4-argument overload declares
    per-node-type reads/writes channels. E4/E5/E6 analysis activates only when
    **every** node type in the graph is declared (a single unknown type skips
    the entire analysis — soundness over coverage). Built-in 3 types
    (llm_call/tool_dispatch/intent_classifier) fully declared.
  - `node_effects` · `condition_specs` added to `export_schema()` (existing
    `conditions` array retained for backward compatibility). 22 new tests.

- **Topology compile-time consistency gate — consumed-key accounting + translation
  validation** (#75 M1). Dual mechanism structurally blocking the "silent semantic
  loss" class (same species as v0.1.0–v0.1.7 `conditional_edges` silent drops):
  - **Consumed-key accounting**: documents declaring `"schema_version": 1` switch
    to strict compilation — unconsumed keys (typo `conditionnal_edges`,
    unsupported fields, barriers silently dropped by empty `wait_for`, ignored
    `to` on inline conditionals) are all collected and reported as compilation
    errors. Marking occurs **inside** the parse block, so erasing the parse stage
    also erases marks, causing strict documents that use those features to fail
    immediately — a structure where drop regressions cannot be silent. `_`/`x-`
    prefixed keys (`_comment`, `x-studio-*`) are always allowed as comment
    namespace. Existing documents without `schema_version` retain permissive
    behaviour (byte-preserved).
  - **Translation validation**: `CompiledGraph::to_json()` re-emission +
    `GraphCompiler::canon()` normal form check `canon(input) ==
    canon(re-emit)` on every compilation. Mismatches (= the compiler dropped
    something or miswired) throw on strict documents and stderr-warn on
    permissive documents. Equivalence is structural comparison — miswirings
    like swapped route keys are also caught (a class that existence-comparison
    misses).
  - `NodeFactory::config_schema(type)` query added, `schema_version` field
    documented in `export_schema()`. 27 new tests (`tests/test_compiler_strict.cpp`)
    — v0.1.x drop-mutant simulation (conditional_edges/barrier/interrupt drops
    + route miswirings) included.

## [0.11.1] - 2026-06-25

### Changed

- **stdio MCP concurrent calls — correlation-ID demultiplexer for I/O overlap.**
  `0.11.0` concurrent tool dispatch only actually overlapped HTTP MCP. stdio MCP
  held a capacity-1 channel lock for the **entire request→response round-trip**
  in `StdioSession::rpc_call_async`, serialising multiple calls in one turn
  through a single session pipe (wall time ≈ sum of latencies). The single pipe
  was not the root cause — JSON-RPC `id` exists precisely to pipeline over one
  connection. Replaced the lock with a correlation-ID demultiplexer:
  - Repurposed the capacity-1 channel as a **write-only lock** — held only for
    the instant of frame write, so two calls' bytes never interleave while reads
    are no longer serialised.
  - A single reader coroutine (`run_reader`) exclusively owns the read side and
    delivers each response line to the correct caller's sink via JSON-RPC `id`.
    N concurrent calls overlap reads so wall time ≈ max(latency) — but **only
    when the peer MCP server processes concurrently** (a single-threaded
    sequential server hits Amdahl's floor).
  - The reader runs lazily only while in-flight calls exist and exits when
    waiters are empty, so the private `run_sync` io_context returns normally.
    Waiters exist only while their caller is awaiting and keep the session alive
    via `MCPTool`'s `shared_ptr`, so the reader never touches a destroyed
    session (no destructor join needed). On pipe EOF/error the reader closes all
    sinks, so awaiting callers receive an exception instead of hanging
    indefinitely.
  - **No API/syntax change** — public headers unchanged, existing code needs no
    recompilation. Engine overhead regression 0 (`bench_neograph` interleaved
    A/B, seq/par Δ 0%).
  - Tests: thread-based delay fixture `tests/fixtures/mcp_stdio_slow.py` +
    `ConcurrentStdioCallsOverlapIO` (5×100 ms calls complete in ~130 ms vs.
    500 ms serial floor; verifies each response routes to its caller via `id`).
    ASan+UBSan ×3 clean.

## [0.11.0] - 2026-06-25

### Added

- **Concurrent tool dispatch — `Tool::execute_async` official async path.**
  `ToolDispatchNode` executes multiple `tool_call`s from a single assistant turn
  **concurrently** using the engine's `make_parallel_group`. Previously each call
  ran sequentially via synchronous `execute()`, and MCP tools especially blocked
  on spawning their own `io_context` via `run_sync` per call, preventing
  parallel MCP calls from overlapping (discovered in an external C++ fork with
  parallel MCP calls). Fix:
  - Virtual `execute_async()` added to `Tool` — default implementation bridges to
    synchronous `execute()`, so existing tools work unchanged.
  - `MCPTool` converted to `AsyncTool` with native `execute_async` (stdio uses
    `rpc_call_async`, HTTP uses new `MCPClient::initialize_async`/
    `call_tool_async` for async handshake — `run_sync` removed).
  - `ToolDispatchNode::run` dispatches calls concurrently via the same
    `make_parallel_group` idiom as node fan-out (single calls are inlined),
    results applied in call order. Backward-compatible via synchronous
    `execute()` facade.
  - Verification: 478/478 ctest, Valgrind 0 leaks, TSAN 0 races.

### Fixed

- **Python async execution exception preservation (issue #122).** Fixed
  `run_async`, `run_stream_async`, and `resume_async` overwriting the original
  Python node exception with a new `RuntimeError` wrapping it as a string. Now
  the original Python exception object, type, user attributes, and traceback are
  preserved through pybind11's standard exception conversion path, and C++
  `py::type_error` is delivered as Python `TypeError` matching synchronous
  execution. The empty callback in `resume_async` is now retained until the
  coroutine completes, also fixing the dangling-reference conflict exposed in
  pybind11 3.x.

### Fixed (docs)

- **README summary badges corrected for missing conditions and internal
  contradictions revealed by sandbox measurements.** The "The four axes"
  summary-table badges stripped measurement conditions from the body/deep-dive,
  reading as exaggerated. Corrected to align with body measurement figures and
  conditions (measurement data table itself unchanged):
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`** — the badge's
    17 µs contradicted the body (`At N=10,000 concurrent ... 7 µs p99`) and
    `flat` described GPU-bound load-test run-latency (648 ms), not a µs
    measurement. Badge aligned with body measurement figures and conditions.
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`** — `libc.so.6`-only
    and 1.2 MB only hold for MinSizeRel + static libstdc++ builds (default
    Release dynamically links libstdc++/libgcc_s/libm/libc). Condition already
    documented in deep-dive §size restored to badge.
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** —
    direct dependencies are indeed `certifi` + `pydantic` (two), but the actual
    installation tree is 7 packages including pydantic transitive deps
    (pydantic-core, typing-extensions, annotated-types, typing-inspection).
- **Added `-DNEOGRAPH_BUILD_POSTGRES=OFF` to deep-dive MinSizeRel reproduction
  command.** PostgreSQL defaults to ON, so configure fails on hosts without libpq
  when run as-is. Fixed.

## [0.10.0] — 2026-05-20

### Added

- **Serial fan-out one-shot stderr warning (issue #62, PR #63).** The default of
  `compile()` is `set_worker_count(1)` — fan-out branches execute serially on the
  caller's executor with no engine-owned thread pool. This intended behaviour
  looks like silent serial execution to users who built multi-Send graphs based
  on docs alone. Added a one-shot guidance message to stderr the first time
  `NodeExecutor` dispatches a multi-Send (or multi-outgoing-edge) fan-out
  without a pool. `std::atomic` + compare-exchange guarantees exactly one
  emission even under concurrent fan-out. Calling `set_worker_count(N>=2)`
  rebuilds `NodeExecutor`, naturally resetting the flag. Suppressible via
  environment variable `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` (or `true` / `yes`)
  — for intentional worker=1 serial execution, benchmarks, and CI stderr
  assertion cases. Covered by 5 Linux + macOS unit tests
  (`test_fanout_worker_warning.py`): fire / one-shot / pool opt-in silence /
  env-var silence / single-Send no-warning. Windows: pytest capfd is
  incompatible with MSVC CRT fd caching in wheel binaries so module-level
  skip — wheel binary stderr output itself is normal.

- **Topology JSON Schema export — `NodeFactory::export_schema()`** (issue #56,
  prerequisite for code-free visual block editors). Exports the topology JSON
  format the engine consumes as a machine-readable schema (JSON Schema Draft
  2020-12) in one piece: `{ neograph_version, $schema, topology (fixed
  envelope), node_types, reducers, conditions }`. A separate repo's block
  editor auto-generates its palette from this schema → editor and engine
  cannot drift across versions. Entirely additive:
    - `NodeFactory::register_type(type, fn, json config_schema)` 3-argument
      variant added. Existing 2-argument delegates to permissive default
      schema — existing user nodes/calls unaffected.
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` query accessors added.
    - Configuration schema declared for 4 built-in types (`llm_call`/
      `tool_dispatch`/`intent_classifier`/`subgraph`). `NEOGRAPH_VERSION`
      exposed as a compile definition (pyproject.toml single source of truth)
      → schema version stamp.
    - `examples/52_export_schema.cpp` (`example_export_schema`):
      `./example_export_schema > schema.json` — standard path for the editor
      repo CI to produce the artifact pinned to a NeoGraph version.
    - Python: `neograph_engine.export_schema()` → dict (editor repo CI
      dumps after `pip install neograph-engine`).
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4. Key:
      top-level `conditional_edges` surviving the loader→compile round-trip
      (regression guard against v0.1.0–v0.1.7 silent-drop recurrence).

### Fixed

- **Topology top-level container format validation (#126).** `channels`/`nodes`
  must be objects; rejected in all modes if not. `edges`/`conditional_edges`
  array validation enforced in strict mode, legacy keyed-edge-map compatibility
  retained. Errors record the path and JSON type, not the full input.
- **`max_steps` termination state exposed (#114).**
  `RunResult::max_steps_exhausted()` and the read-only Python property
  `RunResult.max_steps_exhausted` added. True only when `max_steps` is reached
  while nodes remain to execute; same state provided in gRPC single-response
  and streaming final JSON. C++ struct size unchanged.

- **`set_worker_count` / `set_worker_count_auto` docstring correction
  (issue #62, PR #63).** The v1.0 prep cycle intentionally reverted the
  `compile()` worker pool default from `set_worker_count(hardware_concurrency())`
  to `set_worker_count(1)` (see `src/core/graph_engine.cpp:69-93` comments for
  rationale), but four user-facing docstrings retained the old claim → users
  who built multi-Send fan-out graphs trusting docs got silent serial execution
  on a single thread. Not visible with unit tests (fake spawn, instant body);
  only exposed in real wall-time e2e.
  - Rewrote both `set_worker_count` / `set_worker_count_auto` Python docstrings
    in `bindings/python/src/bind_graph.cpp` to match actual behaviour:
    `compile()` default is 1, `set_worker_count_auto()` /
    `set_worker_count(N>=2)` is explicit opt-in.
  - Corrected both Doxygen comments in `include/neograph/graph/engine.h`
    accordingly. Doxygen Pages auto-rebuilds on master push.
  - Corrected the same stale claim (default = hardware_concurrency) in
    `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md`.

- **Three missing API migrations from v0.9.0 ship supplemented.** PR `9b`
  (`19819d8`) in the v1.0 prep cycle destructively removed the `GraphNode`
  legacy 8-virtual chain, but PR `#48` (`6e654ad`, "C++ examples migrate to
  `GraphNode::run()`") only migrated `examples/` — the following 3 files were
  missed, shipping v0.9.0 in a build-broken state:
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3
      sustained-burst verification key benchmark)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (memory/
      concurrency comparison matrix body against LangGraph and other engines)
    - `wasm/smoke.cpp` (Phase 1 WASM feasibility smoke)

  CI did not pick these targets up as add_executables or (Docker build
  dependency) isolated them in a separate environment, so the merge to master
  and tag passed.

  **Fix**: all three migrated from `std::vector<ChannelWrite> execute(const
  GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in)
  override` + `co_return out` pattern. Node logic unchanged.

  **v1.0 key selling-point native re-verification**
  (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - Concurrency 10K · wall 10–23 ms · p99 17–21 µs · peak RSS **5.6 MB**
      (matches v0.3.0 / v0.5.0 measurements — no memory selling-point
      regression after destructive 9b)
    - 0 errors at 10K
  **Docker matrix (LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6-way comparison) also re-measured within the same session**
  (`results_v0.9.0_docker_recheck.jsonl`).

  During matrix re-run, one independent regression discovered alongside the
  missing API migration — `benchmarks/concurrent/Dockerfile.neograph` could
  not build at all because it failed to track CMake option default changes on
  master (same at v0.9.0 ship time). Over time the following option defaults
  flipped OFF → ON:
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      (requiring `libpq-dev` / `libsqlite3-dev` respectively)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (one prior incident closed in
      `feedback_libcurl_unconditional_dep.md` — only the option toggle was
      added while the default remained ON, breaking the empty-container build
      path again)
    - `find_package(OpenSSL REQUIRED)` is unconditional without an option
      toggle (CMakeLists.txt:256) — separate v1.0 cleanup candidate

  **Dockerfile fix**: `libssl-dev` apt addition + all non-core options pinned
  with explicit `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF`.
  Comment notes "explicit freeze due to two drift incidents".
  `find_package(OpenSSL REQUIRED)` conditionalisation in CMakeLists.txt left
  as a separate task — impact verification needed for other build paths (PyPI
  wheel, ARM64, etc.).

  **6-way matrix key results** (concurrency=10000, 2 cpus / 1 GiB):

  | engine          | mode          | wall_ms | p99_us      | peak_MB | ok/err |
  |---|---|---|---|---|---|
  | **neograph**    | threadpool    | **16**  | **18**      | **5.1** | 10000/0 |
  | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
  | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
  | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
  | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
  | llamaindex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

  NG vs LangGraph (marketing comparison axis): wall **237× faster**, p99
  **4134× faster**, peak RSS **12× lower**.

  **Harsh scenario** (concurrency=10000, 1 cpu / 512 MiB):
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM killed** (exceeded 512 MB cap)
    - **AutoGen asyncio: OOM killed**

  Same v0.3.0 / v0.5.0 measurements — **no regression of NeoGraph's "10K
  concurrent workers, peak RSS 5 MB, no OOM" selling point after destructive
  9b.**

## [0.9.0] — 2026-05-14 — v1.0 prep (Candidate 1 Phase B + Candidate 6)

Two v1.0 single-dispatch unifications from ROADMAP_v1.md converge in one cycle:

  - **Candidate 1 Phase B (`9b`–`9f`)** — all of `GraphNode`'s legacy 8
    virtuals (`execute` / `execute_async` / `execute_stream` /
    `execute_stream_async` / `execute_full` / `execute_full_async` /
    `execute_full_stream` / `execute_full_stream_async`) +
    `add_cancel_hook` + `CurrentCancelTokenScope` + `state.
    run_cancel_token_` + all 6 `PyGraphNodeOwner` legacy overrides removed.
    **Destructive** — deprecation window closed. User GraphNode subclasses /
    user Python nodes must migrate to the single method `run(NodeInput)` /
    `def run(self, input)`.
  - **Candidate 6** — `Provider` 4-virtual cross-product → 1-virtual
    `invoke()`. Still in the addition + deprecation phase — legacy 4 virtuals
    unchanged and functional, deprecation warnings only visible. That side's
    Phase B (`Provider` legacy removal) also closes just before v1.0.0 ship.

The same cycle also includes b59444f's potential parallel-regression revert
(`e5ecb08`) + explicit fan-out example calls + 3 CI environment fixes
(httplib macro guard / Windows MSVC unistd.h / pybind pytest migration),
all part of this [Unreleased].

### Added

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 standard single
  dispatch entry point. Handles both non-streaming (`on_chunk == nullptr`) and
  streaming (`on_chunk` provided) in one method. Consolidates the previous
  4-virtual cross-product (`complete` / `complete_async` / `complete_stream` /
  `complete_stream_async`) into one async-streaming superset. Default
  implementation forwards to the 4 legacy virtuals so existing Provider
  subclasses work unchanged. 6 new ctest (`ProviderInvokeDefault`).
  (PR #40)
- **`invoke()` cancel propagation parity** — when `params.cancel_token` is not
  set and an engine thread-local scope is active, `current_cancel_token()` is
  stamped automatically. Equivalent to legacy sync `complete()` behaviour (node
  body inside the engine calling `provider->invoke(params, ...)` automatically
  receives the running graph's cancel signal). 3 new ctest
  (`InvokeCancelPropagation`). (PR #43)
### Changed

- **All internal LLM calls in engine routed through `invoke()`** — `LLMCallNode`,
  `IntentClassifierNode` (PR #41/#42), `Agent::complete` /
  `Agent::run_stream` (PR #43), `SupervisorLLMNode` /
  `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode` (PR #43),
  `PlannerNode` / `ExecutorNode` (PR #44). LLM dispatch within NeoGraph
  unified to a single surface.
- **C++ examples migration (2 files)** — `31_local_transformer.cpp`,
  `cookbook/ai-assembly/member_server.cpp` now use the new `invoke()`. No
  deprecation warning in user builds. (PR #45)
- **`GraphEngine::compile()` default worker count reverted to 1** (`e5ecb08`).
  `b59444f` was the root cause of a latent 18-day (2026-04-26 → 2026-05-13)
  parallel micro-bench regression of 11.8 → 283 µs (24×) — commit pinpointed
  via bisection (11 worktrees in parallel). From v1.0 default=1 (optimal for
  CPU-tiny sequential/parallel dispatch); for intentional fan-out add one line
  `engine->set_worker_count_auto()` to open hardware_concurrency. Explicit
  calls added to 5 affected fan-out examples (10/14/21/36 +
  deep_research_graph builder). See "Perf retrospective" section in
  ROADMAP_v1.md for details.

### Deprecated

- **`Provider::complete` / `complete_async` / `complete_stream` /
  `complete_stream_async`** — all 4 legacy virtuals carry
  `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` markers. Legacy
  methods function as-is through the deprecation window. Removed in v1.0.0.
  Internal forwarders wrapped with `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` so
  warnings appear only at user-facing override / call sites. (PR #44)

### Removed (Candidate 1 Phase B — destructive)

- **`GraphNode` legacy 8 virtuals** — `execute(GraphState&)` /
  `execute_full(...)` / 6 variants + `ExecuteDefaultGuard` recursion guard
  + 300+ lines of default chain. All removed. `run(NodeInput)` is the only
  pure virtual. (commit `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` member + `cancel()` hook
  iteration** — `cancel.h` retains only `fork()` + `cancel()` +
  `is_cancelled()` + `slot()`. (commit `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local +
  `GraphState::run_cancel_token_` + 3 accessors** — `RunContext::cancel_token`
  is the sole cancel channel. `src/core/cancel.cpp` emptied down to a stub
  (file itself a future-delete candidate). (commit `9e8e956`)
- **6 `PyGraphNodeOwner` legacy overrides** — pybind trampoline calls only
  `run(self, input)`. Python user code also requires a single method from
  v0.9.0. (commit `9e8e956`)
- **2 obsolete pytest files** — `test_execute_stream_dispatch.py` (v0.3.2
  stream-only fallback dispatch verification) + `test_streaming_only_error_
  hint.py` (execute_full_stream takes priority — meaningless in v1.0).
  (commit `4392fbb`)

### Fixed

- **Explicit calls added to 5 fan-out examples** — restored the real parallel
  intention buried by `e5ecb08`'s default worker count revert:
  `examples/10_send_command.cpp`, `examples/14_plan_executor.cpp`,
  `examples/21_mcp_fanout.cpp`, `examples/36_classifier_fanout.cpp`,
  `src/core/deep_research_graph.cpp`'s `create_deep_research_graph()` builder
  now calls `set_worker_count_auto()`. Verification: `classifier_fanout`
  4.22× speedup (25.2 ms sequential → 6.0 ms parallel). (commit `99c470b`)
- **`bench_async_http` httplib macro guard** — `bench_async_http.cpp` includes
  `<httplib.h>` via `<neograph/async/conn_pool.h>` but `CPPHTTPLIB_OPENSSL_SUPPORT`
  was undefined, causing the ODR guard to reject. Added
  `target_compile_definitions(... PRIVATE ...)` to the CMake target.
  (commit `d4be42a`)
- **Windows MSVC `unistd.h` missing** — `test_schema_provider_extra_
  fields_temperature.cpp` used POSIX-only `mkstemps` + `close`, failing the
  Windows build entirely. Wrapped the entire file in `#ifndef _WIN32` guard
  (coverage guaranteed by Linux/macOS). (commit `3c49f12`)
- **16 Python tests migrated** — wheel CI pytest hit `AttributeError` on 28 node
  classes with legacy `def execute(self, state)` pattern. Batch-migrated to
  `def run(self, input)`; streaming nodes gained `input.stream_cb` None-guard.
  (commit `4392fbb`)

### Migration (user code)

**Provider calls (Candidate 6 — deprecation phase)**

New code:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

The 4 legacy virtual overrides continue to work through the deprecation
window, but `-Wdeprecated-declarations` warnings are visible at user override
sites. Removal occurs just before v1.0.0; migration within the deprecation
window is recommended.

**`GraphNode` subclass (Candidate 1 Phase B — destructive)**

C++ code:
```cpp
// old (up to v0.8.x)
class MyNode : public GraphNode {
    NodeResult execute_full(const GraphState& state) override {
        auto x = state.get("x");
        NodeResult out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        return out;
    }
};

// v0.9.0+ current code (single method, coroutine entry)
class MyNode : public GraphNode {
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto x = in.state.get("x");
        // in.ctx.cancel_token / in.ctx.step / in.stream_cb also accessible
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        co_return out;
    }
};
```

Python code:
```python
# old (up to v0.8.x)
class MyNode(neograph_engine.GraphNode):
    def execute(self, state):
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]

# v0.9.0+ current code
class MyNode(neograph_engine.GraphNode):
    def run(self, input):
        state = input.state  # input.ctx.cancel_token / input.stream_cb etc. also accessible
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]
```

**Fan-out intent (worker count default change)**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

Migration 1/2/3 sections in `docs/migration-v0.4-to-v1.0.md` (run() /
ctx.cancel_token / worker count default) + Provider section (to be added in
next docs sweep) provide case-by-case before/after guidance.

## [0.8.0] — 2026-05-13 — DX policy + downstream-driven API gaps addressed

Bundles 8 issues (#22, #25, #26, #27, #28, #34, #35 + #16 follow-up)
surfaced by real-world downstream (ProjectDatePop) feedback and internal
coverage diff into a single minor bump. Two new public helpers
(`RunResult::channel<T>`, `RunContext::store`), 11 new offline examples,
`docs/migration-v0.4-to-v1.0.md` migration guide, and a 5-item DX bundle
reducing friction newcomers hit in their first 30 minutes.

### Added

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** —
  one-liner helpers to extract channel values from the result. Both output
  shapes (nested `output["channels"][name]["value"]` standard + flat keys added
  by builders like `react_graph`) handled automatically. 9 new ctest. (Issue #25)
- **`RunContext::store`** — node body reaches the Store with one line
  `in.ctx.store->get(ns, key)`. The old pattern (capturing `shared_ptr<Store>`
  in a `NodeFactory` lambda) still works — new code only needs the new shape.
  3 new ctest. (Issue #27)
- **`Provider::complete_stream` non-pure default body** — minimal mock / test
  fixture only needs to override `complete()`. Existing streaming-native
  overrides remain unchanged. 2 new ctest. (Issue #22)
- **`neograph::json` arrays `.front()` / `.back()`** — nlohmann muscle-memory
  pattern (`msgs.back()["content"]`) now compiles. 4 new ctest. (Issue #26)
- **11 new offline examples (41-51)** — `resume_if_exists_chat`,
  `custom_reducer_condition`, `store_personalization`,
  `request_queue_backpressure`, `cancel_token`, `node_cache`,
  `sqlite_checkpoint`, `openinference`, `async_tool`, `minimal`. All rc=0,
  no API key / external service dependencies. Fills gaps among the 27/53
  `NEOGRAPH_API` classes that previously had zero references.
- **`examples/51_minimal.cpp`** — 30-line introductory example with one node,
  no LLM, no tool, no mock provider. Understand how NeoGraph runs in under
  5 minutes.
- **`docs/migration-v0.4-to-v1.0.md`** — case-by-case before/after 4 examples +
  common mistakes migrating from the `[[deprecated]]` old 8-virtual chain
  (`execute` / `execute_async` / etc.) → new `run(NodeInput) ->
  awaitable<NodeOutput>`. Also linked from the `NEOGRAPH_DEPRECATED_VIRTUAL`
  macro message.
- **README "Common pitfalls 5" section** — five things newcomers hit in their
  first 30 minutes (`channel<T>` usage, `in.ctx.store`, `neograph::graph::`
  sub-namespace, `<httplib.h>` macro, GCC 13 coroutine ICE) in one place.
  Each item has a fix + related example/issue link.
- **Compile-time `#error` guard (`include/neograph/api.h`)** — when a user TU
  includes `<httplib.h>` before NeoGraph headers without `CPPHTTPLIB_OPENSSL_SUPPORT`,
  compilation fails with a clear message + opt-out macro
  (`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`). Promotes the old #16 runtime SEGV to a
  compile-time failure.
- **`example_minimal` 5 new friendly error-message ctest** — contract lock on
  `Unknown reducer` / `Unknown condition` / `Unknown node type` / `Write to
  unknown channel` messages embedding available names + registration method +
  troubleshooting link in the message body.
- **`docs/troubleshooting.md` 4 new entries** — Tracer adapter `close()`
  hang/crash (#24), GCC 13 coroutine ICE (#23), friendly error message
  guidance (#22), `RunResult::output` shape (#25).
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning` block** —
  explicitly documents the raw-pointer pitfall for adapter authors. Points to
  the `RecordedSpan` + wrapper separation pattern as the correct approach.
  References existing `tests/test_openinference_cpp.cpp::InMemoryTracer` + new
  `examples/49_openinference.cpp::PrintTracer`. (Issue #24)

### Fixed

- **`SchemaProvider::build_body` silent drop of `extra_fields` when
  `params.tools` is empty.** Old code gated `extra_fields` application inside
  `if (!params.tools.empty())`, causing core schema fields like `reasoning` and
  `response_format` to disappear entirely from tool-less calls. Fix: moved
  outside the tools branch so always applied. 3 new ctest. (Issue #34)
- **`temperature_path` schema-side opt-out.** Reasoning models (gpt-5.x,
  o-series) have mutually exclusive `temperature` and `reasoning.effort`, but
  the schema had no way to declare "this provider does not accept temperature",
  forcing a `params.temperature = -1.0f` sentinel workaround on every call.
  Fix: specifying `"temperature_path": null` in the schema causes build_body
  to skip it entirely. 4 new ctest. (Issue #35)
- **Friendly RuntimeError messages** — `ReducerRegistry::get` /
  `ConditionRegistry::get` / `NodeFactory::create` "Unknown <thing>: foo"
  and `GraphState::write` / `apply_writes` `Write to unknown channel` now
  embed available names + registration method + troubleshooting link in the
  message body. Newcomers can determine next steps from the message alone.
- **`SchemaProvider::complete_stream_async` HTTP/SSE branch** now dispatches on
  a long-lived dedicated `bridge_thread_` (old: `Provider` base default spawned
  a fresh `std::thread` per call). Old behaviour triggered an SEGV in glibc
  `internal_strlen` with cold thread-local resolver / NSS state. WS branch is
  already native co_await so unaffected. Token dispatch on awaiter's executor
  preserved (PR #10 invariant). (Issue #16)
- **`example/09_all_features.cpp`** Store demo — added docstring pointer directing
  to `examples/43_store_personalization.cpp` for node-body read pattern.
  Option 2 — option 3 (in-line live node) to be cleaned up together once
  #27's `RunContext::store` lands. (Issue #28)

### Docs

- Canonical shape of `RunResult::output` (channels-wrapped) and its relationship
  to flat-key projections added by builders like `react_graph` documented in
  header docstring. Use of new helpers (`channel<T>` / `channel_raw` /
  `has_channel`) recommended. (Issue #25)
- `RunContext::store` field `@brief` block — two plumbing patterns
  (`in.ctx.store` recommended / old factory-closure capture compatible) with
  code examples side by side. (Issue #27)
- Both paths documented in `examples/43_store_personalization.cpp` file header
  comment.

## [0.7.0] — 2026-05-11 — C++ openinference + async streaming bridge

Closes the four issues filed against v0.6.0 in one minor bump.
Headline: the `Provider::complete_stream_async` default no longer
segfaults when awaited from inside an outer engine coroutine
(issue #4) — the most common shape for SSE / streaming HTTP backends
sitting in front of NeoGraph. Companion: a C++ peer of the v0.6.0
Python OpenInference layer so Phoenix / Arize / Langfuse render
C++-driven traces the same way they render Python ones (issue #9).
Plus: cosmetic Python OTel detach noise silenced (issue #2) and
the same-`thread_id` concurrent-run + `schema_mutex_` × on_chunk
locking invariants are now pinned in the docstrings (issue #6).

### Added

- C++ peer of `neograph_engine.openinference` (issue #9). New
  `neograph::observability` module covers two pieces:
  - `Tracer` / `Span` — small dep-free abstract interface so NeoGraph
    itself doesn't pull in opentelemetry-cpp. Downstream provides an
    adapter wrapping its own backend (OTel SDK, in-memory test fake,
    logging recorder, etc.). 4 attribute setters (string, int64,
    double, bool — bool deliberately renamed `set_attribute_bool`
    so a `const char*` literal can't accidentally resolve to it),
    plus `add_event` for streamed-token diagnostics, status, and
    `end()`.
  - `openinference_tracer(tracer)` — opens a CHAIN-kind root span,
    returns an `OpenInferenceTracerSession` whose `cb` field plugs
    into `engine.run_stream()` and opens a CHAIN-kind child span per
    node, with `NODE_START`/`END` payloads stuffed into
    `input.value` / `output.value` JSON blobs and `LLM_TOKEN` events
    recorded as discrete span events.
  - `OpenInferenceProvider(inner, tracer)` — wraps any `Provider`,
    attaches the OpenInference LLM-kind attribute set
    (`llm.model_name`, `llm.invocation_parameters`,
    `llm.input_messages.{i}.message.{role,content}`,
    `llm.output_messages.0.message.{role,content}`,
    `llm.token_count.{prompt,completion,total}`) on every
    `complete*` call. The streaming overloads also append
    `llm.token` events and a final assembled `output.value`.
  - 7 parity tests in `tests/test_openinference_cpp.cpp` driving an
    `InMemoryTracer` reference adapter — assert root + per-node CHAIN
    span hierarchy, ERROR / INTERRUPT status surfacing, LLM_TOKEN
    span-event recording, straggler-span cleanup on session close,
    LLM provider attribute set, streaming token events, and
    exception status propagation.

### Fixed

- `Provider::complete_stream_async` default bridge no longer blocks
  the awaiting coroutine's executor for the duration of the stream.
  Pre-fix the default was `co_return complete_stream(...)` inline,
  which (a) suspended the engine's `io_context` worker thread for
  the whole HTTP/SSE recv loop — so other node coroutines on the
  same executor stalled — and (b) for `SchemaProvider`'s WebSocket
  Responses branch, additionally nested a fresh `run_sync` io_context
  on top of the engine worker via `run_sync(complete_stream_ws_responses(...))`,
  racing on shared provider state and producing intermittent segfaults
  when called from inside an outer `GraphEngine::run_stream_async`.
  New default spawns a dedicated worker thread for the synchronous
  `complete_stream`, dispatches each token back onto the awaiter's
  executor (so the user's `on_chunk` runs single-threaded with the
  awaiting coroutine — no reentrancy), and resumes the coroutine via
  a one-shot `steady_timer.cancel()`. Worker-thread exceptions
  re-raise on the awaiter. `SchemaProvider` adds a native
  `complete_stream_async` override that skips even the worker thread
  for the WebSocket path by directly `co_await`ing
  `complete_stream_ws_responses`. `OpenAIProvider` benefits from the
  new base default transparently (no WS path, no special case).
  Two new tests in `tests/test_provider_async_default.cpp`:
  `StreamAsyncBridgeDoesNotBlockExecutor` (a concurrent ticker
  coroutine advances during the stream + chunks deliver on the
  awaiter's thread, not the worker's) and
  `StreamAsyncBridgeRethrowsWorkerException`. (Issue #4)

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
Per-run cancellation metadata moves from a non-channel-set `GraphState`
member + a thread-local smuggling channel into an explicit `RunContext`
argument. `deadline` and `trace_id` were added only as reserved extension
slots and are not populated by `RunConfig`. `CancelToken` gains
hierarchical `fork()` so multi-Send fan-out workers each own a
private signal that the parent's `cancel()` cascades to.

### Added

- `RunContext` (`include/neograph/graph/engine.h`) — explicit per-run metadata:
  usable `cancel_token`, `thread_id`, `step`, `stream_mode`, plus reserved
  `deadline` and `trace_id` slots. Engine threads it through every
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
