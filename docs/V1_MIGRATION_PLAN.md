# NeoGraph v1 Redesign and Migration Plan

Status: Accepted Core migration baseline; Program-language sequence superseded 2026-08-08
Date: 2026-07-31
Architecture: `V1_ARCHITECTURE.md`
Program-language replacement: `QUICKJS_CONTROL_MIGRATION.md`
Implementation gates: maintained as private project controls
Audited source: `6b5a36fbcdafffb5da766922321157140ab906ce`

The Core, storage, catalog, activation, authority, and GraphEngine work below
remains useful. Any phase that grows or permanently publishes the NeoGraph
Program JSON operation DSL is superseded by the QuickJS migration plan.

## 1. Starting point

The current code is not a failed prototype. It already contains the Core that
v1 should preserve:

- `GraphEngine` compiles, validates, runs, resumes, checkpoints, streams, retries,
  cancels, dispatches tools, and supports dynamic `Send` and subgraphs.
- `EngineConfig` and `EngineResources` provide a complete construction path.
- `GraphAdmin`, `CompletionProvider`, capability-style checkpoint interfaces,
  `ToolSet`, and engine-local registry overlays are meaningful pre-v1 cleanup.
- Harness already compiles bounded DSL/Core input into immutable retained
  artifacts, seals an admission profile, and checks checkpoint/artifact binding.
- The DSL covers static sequence, fan-out/fan-in, route, barrier, cycle, retry,
  typed worker ports, and limited sealed conditions.

The missing product is not another node engine. It is a public Program layer
that turns these pieces into one compilable, versioned, activatable, durable
Agent Program contract.

### Verified baseline

The ordinary GNU 13.3 Release build at
`24cbd86d80815b2c2b46aacb02cbf5a570503262` completed in the existing
`build-v1-baseline` directory in `58.25 s`. CTest reported zero failures across
951 registered tests in `6.35 s`; 34 tests were conditionally skipped: 31
PostgreSQL-service tests, one live LLM cancellation test, and two live
WebSocket-provider tests. The static installed-consumer flow
`scripts/test_find_package.sh` also configured, built, installed, relocated, and
ran its downstream `find_package(NeoGraph)` consumer in `85.44 s`. ASan/UBSan
was deliberately not rerun in this ordinary-baseline wave, so a current
candidate sanitizer run remains a release gate rather than an implied result.

The local Core performance baseline was remeasured with
`taskset -c 0 ./build-v1-baseline/bench_neograph 100000 5000 1 1`. After two
discarded warm-up process runs, ten Release process samples were retained:

| Workload | Raw samples (us) | Median | Nearest-rank p95 | Population stddev |
|---|---|---:|---:|---:|
| `seq`: three incrementing nodes | `5.85155, 5.89490, 5.91027, 5.93758, 6.04154, 5.85096, 5.84122, 6.10376, 6.27868, 6.00706` | `5.923925 us` | `6.27868 us` | `0.132166 us` |
| `par`: five-way fan-out plus join, `worker_count=1` | `13.6211, 13.2768, 13.2917, 13.2280, 14.0613, 13.4455, 13.4602, 13.2889, 13.7311, 13.6345` | `13.45285 us` | `14.0613 us` | `0.248471 us` |

These are same-host comparison baselines, not portable release promises.

### Classic release cut

`24cbd86d80815b2c2b46aacb02cbf5a570503262` is the current candidate code
baseline for one final pre-v1 **Classic** reference release after the bounded
#188/#189/#190/#230 checks pass. Any later source change requires re-running
the applicable candidate gates. Classic is not a second permanent product line
or execution engine:

- reconcile the historical `v2.0.0`/`v3.0.0` tags with the current `0.11.x`
  project line in the release notes, then choose one new release number under
  the repository release rules;
- publish the exact compiler/platform/test/package/performance evidence above;
- announce that v1 C++ consumers rebuild and stored Program formats migrate;
- keep only severe correctness, security, data-loss, and build-break fixes on
  the Classic line while v1 is built;
- do not backport Program, activation, child composition, or new DSL semantics
  to Classic.

### Current Classic release readiness

The product version has one authoritative literal:
`pyproject.toml` declares `0.11.1`; top-level CMake parses that value into
`PROJECT_VERSION`, the generated CMake package reports `0.11.1`, and the Python
extension derives `__version__` from the same field. Public headers contain no
independent product-version literal; the runtime schema stamp is injected from
CMake. The changelogs correctly record `0.11.1` as the latest released version
and retain subsequent work under `Unreleased`.

The historical `v2.0.0` and `v3.0.0` tags (2026-04-22) predate the current
pre-v1 line that restarted at `v0.1.0` (2026-04-26). They must remain immutable
historical tags. The next Classic tag must therefore be a new version on the
current line; `0.12.0` is the SemVer direction for the additive `Unreleased`
content, but choosing it and dating the translated changelogs remains a human
release-owner decision.

Release status is **NO-GO** until every unchecked item is closed:

- [x] GNU 13.3 Release build completes with hardening enabled.
- [x] Registered CTest baseline: `951/951`, zero failures, 34 conditional
  external-service skips enumerated above.
- [x] Static installed `find_package` consumer builds and runs after prefix
  relocation.
- [x] CPU-pinned Core hot-path samples are retained and satisfy the
  preregistered median/p95 ratios.
- [ ] #188: run `benchmarks/dr_compare/mem_probe.py` from a checkout outside
  `/root/Coding/NeoGraph` and retain the output; only the path-independent code
  and `psutil` instructions are present locally.
- [ ] #189: rebuild the corrected WASM smoke command with Emscripten and run the
  Node/browser smoke; the source list and default-worker documentation are
  present, but no retained current smoke result exists.
- [ ] #190: run the worker-count-4 mock comparison once, then one low-cost live
  provider sample when credentials are available, retaining model, transport,
  call-count, and timing metadata.
- [ ] #230: guard the async-only benchmark targets when
  `NEOGRAPH_BUILD_ASYNC=OFF`, reproduce the Galaxy A34 result from one source
  tree, and retain device-specific results in the repository.
- [ ] Run the current candidate ASan/UBSan gate; do not reuse the earlier
  845-test/four-skip sanitizer claim.
- [ ] Human release owner chooses the new version, converts `Unreleased` in all
  changelogs, confirms protected CI, and explicitly approves tag/PyPI/GitHub
  publication.

## 2. Decision cut

### Keep

- `neograph::core` as the lightweight required library.
- `neograph::graph::GraphEngine` and strict Core JSON as the direct-use engine.
- Existing channel/reducer/condition/checkpoint/store/provider/tool semantics
  when they are internally coherent.
- Immutable compilation before execution.
- Explicit cancellation, retry, event, usage, checkpoint, and effect identity.
- Preserve the current dependency choices during the redesign: yyjson for JSON,
  cpp-httplib for HTTP, standalone Asio for async I/O, concurrentqueue for the
  concurrent queue, cppdotenv for environment loading, SQLite3 and libpq for
  storage, and opt-in protobuf/gRPC and libcurl where those components require
  them.

### Add

- optional `neograph::program` public library;
- Program source, compiler, immutable bundle/version, runtime, handle, result,
  store, journal, activation, migration, child Program, and module concepts;
- one small typed orchestration vocabulary above Core;
- owner-scoped version and run identity;
- Program conformance suites shared by C++, Harness/MCP, and other adapters.

### Retire

- a separate public sole Control VM and Durable Kernel execution architecture;
- any interpretation that removes durable control-plane responsibilities from
  Program when independently recoverable child Programs are enabled;
- Harness-only ownership of Program semantics;
- process-global executable lookup for admitted Programs;
- raw borrowed tool ownership on the standard v1 construction path;
- transport-specific direct calls to wide `GraphEngine` APIs;
- duplicate compatibility names after callers have migrated.

### Deliberately do not do

- rename `neograph::graph` to `neograph::core` in C++ merely for symmetry;
- put Program compilation/activation fields into `GraphEngine`;
- add direct AVX2 intrinsics to production from the current experiment;
- bind every C++ class into Python by name count;
- preserve pre-v1 source aliases indefinitely. One announced rebuild boundary is
  cleaner and safer.

## 3. Current issue audit

`OPEN` was not treated as evidence that work remains. Each issue was checked
against source, tests, and current documents.

| Issue | Finding | v1 disposition | Owner / phase |
|---|---|---|---|
| #112 Python `llm::Agent` | **Reject.** Python `GraphEngine` already provides ReAct, async cancellation, checkpoint/HITL, Store, and ToolGate; standalone Agent adds no NeoGraph-specific ability. | Close with the capability-based reason. Reopen only for a requested lightweight API or a real coroutine Agent contract. | Extension / P0 |
| #179 WSL2 baseline | **Done.** Fractional timing and worker-mode documentation exist. | Close as a platform record; do not promote its old numbers to a universal gate. | Performance / P0 |
| #187 v1 API tracker | **Partial.** Many children landed, but shared invocation and remaining ownership/decomposition work are not complete. | Rewrite as the Core/Program/Protocol v1 umbrella; completion follows the focused items below. | Cross-cutting / P0-P8 |
| #188 memory-probe portability | **Partial.** Checkout-relative path and `psutil` instructions landed. | Run once outside `/root/Coding/NeoGraph`, retain evidence, then close. | Release / P0 |
| #189 WASM smoke sources | **Partial.** Missing Core sources, behavior checks, and corrected default-worker documentation landed. | Rebuild with Emscripten, run the Node/browser smoke, retain evidence, then close. | Docs/Release / P0 |
| #190 dr_compare workers | **Partial.** Code and docs use worker count 4. | Run mock once; run one low-cost live-provider sample only when a key is available, then close with call metadata. | Performance / P0 |
| #192 Provider dispatch | **Superseded.** ABI-safe `CompletionProvider::do_invoke` replaced the proposed `Provider::invoke` cutover. | Close as superseded; keep the old Provider vtable, require new implementations to use CompletionProvider. | Core/Release / P0 |
| #193 checkpoint dispatch | **Partial.** New capability interfaces remove recursion for compliant backends, but legacy defaults still mutually recurse and sync-only stores still block async executors. | Add a nonblocking sync-store adapter, migrate internal callers, and retain regression tests for zero-override and sync-only implementations before closing. | State/Core / P1-P2 |
| #214 `RunInvocation` | **Partial.** `program::RunInvocation` is canonical at the Program runtime boundary and is used by Harness and the Program-backed A2A adapter. Legacy GraphEngine A2A construction, ACP, and gRPC have not been rebased. | Keep legacy routes explicit; migrate each remaining protocol through the owned invocation and prove lifecycle parity before removing its compatibility path. | Protocol / P2-P8 |
| #215 invocation contracts | **Partial.** Program/A2A and Harness regression suites cover canonical identity, cancellation, events, recovery, and terminal projection. ACP/gRPC parity is still absent. | Add a parameterized protocol suite only when each remaining adapter has a Program route; do not count legacy GraphEngine tests as Program conformance. | Protocol / P2-P8 |
| #216 engine surfaces | **Partial.** Complete config and `GraphAdmin` exist, but execution-only dependency and admin concurrency policy do not. | Keep Core construction/run/admin responsibilities explicit; make adapters depend on a narrow invocation capability. | Core / P1-P2 |
| #217 tool ownership | **Partial.** `ToolSet` is safe; legacy `NodeContext::tools` can still dangle. | Make owned `ToolSet`/sealed Program capability imports standard; remove the raw transfer path at the v1 rebuild boundary. | Core/Program / P1 |
| #218 scoped registries | **Partial.** Local overlays and isolation tests exist; global fallback remains. | Freeze immutable registry snapshots and remove ambient fallback for strict Core/Program admission. Migrate Python registration deliberately. | Core/Program / P1 |
| #219 MCP transport split | **Partial.** Internal sessions exist, but protocol/tool code still branches on HTTP vs stdio. | Add one MCP transport capability with owned cancellation, timeout, shutdown, and error translation. | Protocol / P6 |
| #220 SchemaProvider split | **Partial.** Request mapping has a test seam; parsing, transport, pools, and callback ownership remain coupled. | Extract value-level parsers, then transport strategy; preserve the Provider-facing contract. | Extension/Protocol / P6 |
| #230 Galaxy A34 benchmark | **Done in this branch.** Async-off CMake target guards, one-source-tree reproduction instructions, explicit machine/compiler metadata, and repeated-median result output now exist. | Retain the measurement as device-specific evidence; do not promote it to a universal performance gate. | Performance/Release / P0 |
| #231 adaptive fan-out/token path | **Pending.** Only a fixed no-op 5-way benchmark exists. | Keep as a measurement program. Add width × body cost × worker and token batching/crossover tests before changing defaults. | Core/Performance / independent lane |
| #237 channel lifecycle | **Partial.** Reducers, full snapshots, SQLite deduplication, and pending writes exist; combine/retention/checkpoint policy is not explicit. | Define separate channel combine, retention, and persistence policies; resolve executor/validator order wording and measure long histories. | Core/State / P1-P3 |
| #238 subgraph persistence | **Partial.** Per-invocation derived identity and context inheritance exist; mode and nested inspection do not. | Add explicit stateless/per-invocation/per-thread modes, stable graph paths, nested inspection, and concurrency rules after #237. | Core/Program / P3 |
| #239 typed C++ state schema | **Pending.** `ChannelKey<T>` is only a typed name over JSON. | Design one typed descriptor/builder lowering to the existing `ValidatedTopology`; no second engine and no template-for-template Python copy. | Core/Program / P1 |
| #241 generated media/LRO | **Pending.** Current result/parser handles text and tool calls, not provider-neutral artifacts or long-running operations. | Define `Artifact`, request envelope, one-shot/stream/LRO modes, and bounded cancellable polling; bind to Python because graph lifecycle integration is unique. | Extension / P6 |
| #242 SchemaProvider registry | **Pending.** Strategies are private enums/switches. | Follow #241 with explicit-injection primitive registry; defer Python callback registration until a real partial-extension use case exists. | Extension / P6 |
| #244 multi-tenancy | **Partial.** HTTP authorization scope exists; stores, IDs, caches, providers, tools, credentials, and quotas are not end-to-end scoped. | Thread owner scope through Program and Core storage/identity; add same-public-ID isolation matrix; do not move JWT/OAuth parsing into Core. | Core/Program/Extension / P7 |
| #250 authenticated host models | **Partial.** A generic worker seam exists; only direct API-key Provider execution is implemented. | Implement installable host adapters with safe process ownership, preflight, structured errors, cancellation, and no token extraction. | Program/Extension / P7 |
| #251 global MCP adoption | **Partial.** Existing MCP clients can be called; discovery/trust/pinned identity/process hardening are absent. | Harden process launch first, then implement credentialless global discovery and two-step trust/admission for local mode only. | Program/Extension / P7 |
| #252 Programmable Agents epic | **Partial.** Compiler-backed Harness and behavioral evaluation exist; child Programs, activation, modules, and feedback promotion do not. | Rewrite the epic around Core + Program; remove VM/Kernel claims and use this phase plan as its dependency graph. | Program / P0-P8 |
| #254 sole Control VM | **Superseded.** Production uses GraphEngine and the cutover was already withdrawn. | Close; retain experiment history only. | Docs / P0 |
| #255 lightweight planner/Kernel | **Partial experiment.** One reference Kernel lost in a narrow strict-linear measurement; other semantics were not measured. | Record that candidate as rejected. Keep broader runtime-boundary measurement only if a future concrete design needs it; do not generalize. | Core/State/Performance / independent lane |
| #256 bounded DSL | **Partial.** The admitted bounded control set now lowers to typed direct operations, including durable `spawn`/`await`; arbitrary dynamic `Send` and topology mutation remain deliberately out of scope. | Add new syntax only when it can be sealed into the immutable plan and given lifecycle/checkpoint semantics. | Program / P3 |
| #257 immutable generations | **Partial.** ProgramVersion admission/activation CAS, pinned runs, MigrationPlan, durable child attachment, lineage, and retention are implemented. Backend/SDK/ABI cutover remains. | Keep storage parity and consumer migration as separate P4-P8 gates. | Program/State / P4-P8 |
| #260 fusion/AVX2 | **Partial experiment.** Fusion and compiler vectorization were useful for one sidecar shape; direct AVX2 had no proven win. | Publish corrected benchmark commit; design typed numeric buffer and safe fusion rules before any production candidate. No direct AVX binding. | Core/Performance / independent lane |

### Issue housekeeping order

1. Close/reclassify #112, #179, #192, and #254 with the evidence above.
2. Finish the bounded validation items in #188, #189, and #190.
3. Retain #230 as a device-specific benchmark record and update #187/#252 to point at Core + Program and this plan.
4. Keep #193, #214-#220, #237-#251, #256, and #257 as focused deliverables
   with the revised phase ownership.
5. Keep #231/#255/#260 out of the architecture critical path. Performance work
   may proceed in isolated worktrees, but it cannot change the design without a
   measured production candidate.

## 4. Target repository shape

The first implementation does not move every Core source. It establishes public
and link boundaries first; source movement follows only when it reduces coupling.

```text
include/neograph/
  graph/                  existing Core public API
  program/
    source.h
    builder.h
    diagnostic.h
    compiler.h
    bundle.h
    version.h
    runtime.h
    handle.h
    result.h
    store.h
    journal.h
    migration.h

src/
  core/                   existing GraphEngine implementation
  program/
    source.cpp
    compiler.cpp
    bundle.cpp
    runtime.cpp
    store.cpp
    journal.cpp
    migration.cpp
    operations/

spec/
  program-source-v1.schema.json
  program-bundle-v1.schema.json
  program-version-v1.schema.json
  program-checkpoint-v1.schema.json
  program-event-v1.schema.json
  program-migration-v1.schema.json

adapters/
  remain in current component directories until a move pays for itself
```

CMake targets:

| Target | Dependency | Purpose |
|---|---|---|
| `neograph::core` | foundation only | Direct graph compile/execute/checkpoint |
| `neograph::program` | Core | Program compile/orchestration, in-memory stores |
| `neograph::program_sqlite` | Program + SQLite | Durable local Program data |
| `neograph::program_postgres` | Program + PostgreSQL | Durable PostgreSQL Program data; optional target with v1 parity required when enabled |
| `neograph::harness` | Program | Harness application/service, not architecture owner |
| `neograph::mcp_server` | MCP types + selected service adapter | Transport |

Do not make a repository-wide directory move in P1. It creates merge conflicts
without proving the boundary. First make dependency arrows true in CMake and
headers; move files later in small mechanical commits.

## 5. Public API migration map

| Current surface | v1 surface | Migration rule |
|---|---|---|
| `GraphEngine::build/build_strict` | unchanged Core standard path | Keep and freeze after v1 conformance. |
| `GraphEngine::compile` | `build` or ProgramCompiler | Deprecate during pre-v1, remove only at announced rebuild boundary if all callers migrate. |
| post-build engine setters | complete `EngineConfig`/`EngineResources` | Migrate callers, make runtime immutable, then remove compatibility setters. |
| `NodeContext::tools` raw pointers | owned `ToolSet` in Core; sealed capability references in Program | No borrowed transfer in standard v1 path. |
| local registry + global fallback | immutable explicit registry snapshot | Strict Core/Program misses fail closed. Legacy loose Core mode may remain only if clearly named and isolated. |
| Harness DSL/Core request | `ProgramSource` frontend | Preserve source coordinates and stable diagnostics. |
| Harness admission profile | `program::AdmissionProfile` | Move semantic ownership out of MCP. |
| Harness artifact | `ProgramBundle` + `ProgramVersion` | Import only with exact identity; otherwise classify as legacy/drain-only. |
| Harness compile/start/get/resume/cancel/events | public ProgramCompiler/Catalog/Runtime/Store/Handle API | MCP becomes a thin adapter; wire compatibility may remain during pre-v1. |
| `HarnessWorkerCall` | Program `call_core` bindings/capability invocation | Do not put transport fields in Program types. Preserve legacy aggregate only until adapter migration completes. |
| direct A2A/ACP/gRPC `GraphEngine` calls | owned `RunInvocation` capability | Shared contract suite; protocol lifecycle remains protocol-specific. |
| proposed `GraphProgramVersion` | `ProgramVersion` | One name only. |
| Control VM bytecode | typed immutable Program plan | No bytecode encoder/decoder/JIT in the default architecture. |
| Durable Kernel | Program-owned transition/journal/checkpoint/effect state plus an internal durable child dispatcher | Keep one public Program API and one Core node executor; do not add a public second engine. |
| SchemaProvider private strategy enums | request mapper + transport + parser + injected primitive registry | #241 contract before #242 extensibility. |
| Python `llm::Agent` parity | Python GraphEngine/Program | Do not bind; expose Program abilities and generated artifacts where Python cannot reproduce lifecycle semantics. |

### Adapter and protocol cutover matrix

Every supported frontend must converge on the same public Program contract.
JSON-RPC is a wire envelope used by existing protocols, not a new independent
execution API.

| Surface | v1 route | Required proof | Phase |
|---|---|---|---|
| Core C++ | direct `GraphEngine` | existing strict Core conformance and installed consumer | P1-P8 |
| Program C++ | `ProgramCompiler`/`ProgramCatalog`/`ProgramRuntime` | canonical Program contract suite | P2-P8 |
| Harness C++ | thin Program application/service | Harness behavior through Program with no Harness-only compiler/runtime | P2/P8 |
| MCP, including its JSON-RPC envelope | MCP adapter to Program | wire compatibility plus Program conformance and unknown-code preservation | P2/P6/P8 |
| HTTP | HTTP adapter to Program | request/lifecycle mapping, cancellation, streaming, and unknown-code preservation | P8 |
| CLI | CLI adapter to Program | compile/run/resume/cancel lifecycle and stable process exit mapping | P8 |
| Python | selected NeoGraph-specific Program bindings | cancellation/checkpoint/replay/activation/lineage parity; no generic JSON/schema duplication | P8 |
| A2A, including JSON-RPC 2.0 | A2A adapter to Program `RunInvocation` plus explicit remote collaboration links | A2A lifecycle, cross-owner consent, task/artifact correlation, streaming, cancellation, restart, and shared invocation conformance | PR8/P8 |
| ACP, including JSON-RPC 2.0 | owned `RunInvocation` to Program/Core | ACP lifecycle and shared invocation conformance | PR8/P8 |
| gRPC | owned `RunInvocation` to Program/Core | gRPC lifecycle, unary/streaming behavior, and shared invocation conformance with `NEOGRAPH_BUILD_GRPC=ON` | PR8/P8 |
| Standalone generic JSON-RPC | no new public engine surface | prove A2A/ACP/MCP envelopes remain protocol-owned; add a generic surface only through a separate accepted product decision | P8 |

An adapter is not migrated merely because it compiles. Its supported lifecycle,
terminal states, diagnostics, cancellation, streaming, checkpoint/resume, and
owner-scope behavior must pass the shared contract suite plus protocol-specific
wire tests.


### Remote collaboration contract

The v1 A2A surface is also the network path for collaboration between two
independently operated NeoGraph runtimes. This includes the case where two
users' agents cooperate on one task while retaining separate owner scopes.

The remote path must not introduce a second execution engine:

```text
User A Program -> A2A adapter -> User B Program
      |                              |
 local Program store             local Program store
```

Each side keeps its own immutable Program version, run record, budget ledger,
capability policy, journal, and terminal publication. A collaboration link
explicitly binds the two sides and limits:

- the owner scopes and authenticated agent identities;
- the permitted message kinds and artifact contracts;
- the capabilities/effects that may be requested or exposed;
- expiry, cancellation, retry, and acknowledgement rights;
- A2A task/context IDs to local Program run and correlation IDs.

Same-runtime collaboration may use a typed Program port or mailbox. The
identical logical envelope must also be serializable through A2A for separate
processes, hosts, or users. A2A retries and task snapshots are transport
evidence; the local Program transition store remains authoritative. A duplicate
or ambiguous non-idempotent effect must be reconciled through Program effect
state, never inferred from a successful HTTP response alone.

The first remote collaboration conformance scenario is a two-user
pair-programming task: user A's coordinator delegates a bounded subtask to
user B's executor, receives progress and structured artifacts, sends a
correction or clarification, and completes or cancels the collaboration
without exposing either user's unrelated tools, credentials, or history.

The Program-backed A2A adapter and durable collaboration mailbox now cover
NeoGraph-local request admission, recovery, task projection, and
owner/capability attenuation. The legacy GraphEngine constructor remains a
compatibility surface; NeoCode/NeoProtocol and cross-host enablement remain
blocked on the explicit Issue #7 rebase evidence.

### Cross-repository rebase gate

The [NeoProtocol](https://github.com/fox1245/NeoProtocol) and
[NeoCode](https://github.com/fox1245/NeoCode) repositories are historical
integration/reference snapshots, not current NeoGraph v1 consumers. NeoCode's
recorded harness specification pins an older NeoGraph commit and a local
JSON-lines-over-stdio sidecar. NeoProtocol's federated ACP reference names the
historical `neograph::acp` surface. Neither repository may claim current
compatibility until its adapter has been rebased onto the current Program
contract.

The migration order is:

1. Freeze `ProgramVersion`, `RunInvocation`, `CollaborationLink`, message,
   artifact, cancellation, and idempotency contracts.
2. Make NeoCode a thin adapter over current Program lifecycle operations;
   preserve session, workspace, tool, permission, and harness ownership in
   NeoCode.
3. Rebase NeoProtocol's Task Offer, ACP, WebRTC, and workspace adapters onto
   the same invocation and collaboration contracts; keep signaling and wire
   framing protocol-owned.
4. Add explicit protocol, schema, and NeoGraph contract-revision metadata and
   reject incompatible combinations before admission or execution.
5. Prove two-runtime owner isolation, restart/retry behavior, artifact
   correlation, cancellation, and duplicate-dispatch handling before enabling
   cross-host transport.

Historical bundles and sidecar records are classified as exact-import,
converted, drain-only, or blocked. They are never silently treated as
current `ProgramVersion` values. Issue #7 records this cross-repository gate;
Issue #6 remains the remote collaboration behavior contract. The focused,
machine-readable gate is `spec/cross-repository-compatibility-v1.json` with
`scripts/check_cross_repository_compatibility.py`; it compares the current
ProgramVersion, RunInvocation, and A2A collaboration surfaces and rejects a
historical consumer that lacks explicit rebase revisions and verified evidence.

### Contract-driven multi-model implementation flow

Issue #8 records the cross-cutting Harness contract for using a frontier
planner, a lower-cost implementation worker, and an independent verifier
without weakening the Program/Core boundary. This is an orchestration and
evidence flow, not a second execution engine.

The migration order is:

1. Add a typed manifest containing `assumptions`, `requirements`, `non_goals`,
   `acceptance`, `fixed_test_vectors`, `independent_oracles`, `risk_register`,
   and bounded `retry_policy`.
2. Enforce the manifest lifecycle
   `proposed -> reviewed -> frozen`; only frozen manifests may select a worker.
3. Give the worker only the frozen contract and scoped workspace context. A
   worker may report a missing premise or contract gap, but may not rewrite
   acceptance identifiers, expected values, scope, permissions, or retry
   limits.
4. Run deterministic build/test commands and independent/reference checks
   outside the worker's self-report.
5. Bind every evidence record to the manifest hash, Program/version identity,
   workspace revision, command, toolchain, and artifact hash.
6. Publish only when every required acceptance identifier has evidence and no
   blocking diagnostic remains. Otherwise terminate as `blocked` or `failed`.
7. Use an explicit human decision gate for subjective or oracle-deficient
   work; never convert a candidate and its narrative into automatic
   correctness.

The expected product effect is reduced senior-engineer toil in context recovery,
repetitive edits, known-check reruns, and regression archaeology. Senior
ownership remains mandatory for premises, ambiguous trade-offs, and release
approval. A frontier planner is not a truth oracle: an invalid premise can
still yield a coherent but wrong implementation.

Exit gate: one conformance fixture demonstrates
`planner -> reviewed/frozen manifest -> worker -> independent verification ->
publication`, plus the fail-closed path for missing evidence or a failed
independent check. The fixture must preserve manifest, evidence lineage,
terminal status, and recovery behavior, and must execute through the existing
typed Program/Core dispatch path without a generic bytecode VM.

### Implementation audit — 2026-08-05

This audit is source-and-contract evidence, not a claim that every historical
consumer has migrated. “Partial” means the local contract exists but one or
more acceptance gates in the corresponding architecture issue remain open.
The current checkout passed the configured serial debug CTest gate (1,303 tests,
three explicitly skipped live/integration tests, no failures; 150.99 seconds)
and the configured TSan CTest gate (1,303 tests, the same three skips, no
unsuppressed race reports; 271.40 seconds). The full ASan/UBSan CTest gate also
passed all 1,303 tests with four intentional skips (the same three live tests
plus the RSS-under-ASan stress test), with leak detection enabled and no
failures (413.88 seconds).

| Area | Status | Evidence and remaining boundary |
|---|---|---|
| Typed plan/direct dispatch (Issue #5) | **Done in NeoGraph.** | `ProgramPlanDispatchDescriptor` seals operation/source/reference metadata; `run_attempt.cpp` dispatches `ProgramOperationKind` directly. |
| Durable child Program lifecycle (Issue #4) | **Implemented with bounded recursive authority.** | `start_child`, durable publication/dispatch/completion, recovery, timeout, duplicate-dispatch, and lineage tests exist. Child grants carry remaining descendant depth, attenuate by one per hop, and reserve subtree child quotas before publication. |
| Program-backed A2A collaboration (Issue #6) | **Done for the current adapter contract.** | `ProgramAgentAdapter`, owner-scoped `CollaborationMailbox`, authenticated peer/task authorization, replay/idempotency, revocation, and artifact attenuation tests. |
| Frozen Harness implementation contract (Issue #8) | **Done for the local contract surface.** | Immutable `ContractManifest`/`ContractRun`, independently bound evidence, fail-closed verification, SQLite restart/tamper coverage, and Harness conformance tests. |
| Evidence-ledger swarm foundation (Issue #11) | **Implemented foundation; issue remains partial.** | `EvidenceLedger`, SQLite persistence, `ResearchTaskBoard`, durable board budgets, source/version identity, lease expiry/reassignment, owner isolation, immutable publication, negative evidence, contradiction resolution, and restart tests exist in `src/core/research_task_board.cpp`, `src/core/sqlite_evidence_ledger.cpp`, and `tests/test_evidence_ledger.cpp`. The 100-worker/Program, federated/NetLAB, adaptive large-swarm, and full fault/benchmark matrices remain open. |
| Async tool/resource arbitration (Issue #13) | **Implemented local execution surface; issue remains partial.** | `ToolExecutionController` has awaitable native/thread/process bridges, versioned policy classes, keyed/exclusive/capacity/single-flight/external-limited admission, cancellation/deadlines, output limits, process-group cleanup, typed terminal results, and reconciliation flags. `tests/test_tool_execution.cpp` covers these paths and host-admission integration. Weighted owner/root fairness, complete bounded priority inheritance, durable queue/restart recovery, and the full threat/benchmark matrix remain open. |
| Host admission and adaptive concurrency (Issue #14) | **Implemented bounded controller; issue remains partial.** | `HostResourceProfile::detect_current` applies conservative process/cgroup-v2, memory, file-descriptor, disk, and safety-reserve limits; `HostAdmissionController` adds component-wise intersection, FIFO/aging admission, RAII leases, cancellation, capacity shrink, pressure reduction, hysteresis, bounded recovery, and snapshots. `tests/test_host_admission.cpp` covers the local contract. Durable parked-child state, full GPU/network/provider/device detection, restart remeasurement, cooperative preemption, and the 1,000-logical-child capacity/benchmark matrix remain open. |
| Task-specific Harness synthesis/reuse (Issue #9) | **Partial.** | Strict Program compilation/admission, child execution, A2A, evidence, and contract verification are available. Reusable Harness retrieval/rebinding, attributable evaluation/feedback, and the benchmark/fault matrix remain open. |
| Recursive child authority (Issue #12) | **Implemented local bounded slice; issue remains partial.** | Leaf-only `(max_child_depth,max_total_children)=(0,0)` policy remains valid; explicit paired grants support bounded recursion through the hard depth ceiling, with fail-closed depth/count checks and persisted recovery guards. Full issue acceptance remains open as recorded in the issue comment. |
| ACP/gRPC Program cutover and shared protocol suite | **Partial.** | Compatibility adapters still directly construct Core `RunConfig`; no Program parity claim is made. |
| NeoCode/NeoProtocol rebase (Issue #7) | **Blocked externally, fail-closed locally.** | Compatibility metadata rejects a current-consumer claim without an explicit rebase revision and conformance evidence. |
Status (2026-08-05): **Implemented for the currently admitted non-child vocabulary.**
The typed direct-dispatch path covers sequence, branch, bounded loop/retry,
parallel/race/quorum/map, await, cancel, checkpoint, emit, return, and Core
operations. Durable `spawn`/child `await` remains the P6 child path; arbitrary
dynamic `Send` and topology mutation remain out of scope.
Status (2026-08-05): **Partial.** In-memory and SQLite catalog/store activation,
rollback, owner isolation, retention, and migration records are implemented.
PostgreSQL Program transition persistence and backend-parity process-restart
proof remain open.
Status (2026-08-05): **Partial.** Migration classes, exact compatible forks,
journal CAS, pending input/effect records, outbox binding, and ambiguity
classification are implemented locally. The runtime-level crash gate spanning
Core checkpoint, pending value, Program transition, and journal publication
remains open.
Status (2026-08-05): **Partial.** Child modules, receipts, bounded durable
spawn/await, authority attenuation, lineage, and artifact/LRO primitives exist.
The MCP transport split and SchemaProvider parser/transport split remain open.
Status (2026-08-05): **Partial.** Host admission, owner-scoped Program/A2A
identity, and consent/attenuation foundations exist. End-to-end tenant,
authenticated host-model, and credentialless trusted global-MCP boundaries
remain open.
Status (2026-08-05): **Partial.** Core and Program quickstarts plus the example
disposition manifest exist, but only the quickstarts are verified. ACP/gRPC
Program cutover, component/example smoke coverage, SDK/ABI/storage freeze, and
translated documentation remain open.
### PR7 implementation checkpoint — historical closure and current boundary

The end-of-day checkpoint below recorded the first Harness-to-Program cutover
and intentionally preserved a failing durable-adapter boundary. That boundary
was subsequently closed by the PR7 hardening work recorded in issue #3 and is
not a current failure claim.

The completed local PR7 slices are:

- Harness request translation, Program compilation/admission, runtime start,
  resume, cancel, reconnect, recorded replay, and compatible fork projection;
- owner-scoped Program artifact/run lookup and reconnect snapshots;
- typed pending input/effect persistence, exact-call reconciliation, TTL
  handling, and one-winner resume compare-and-swap;
- source-transition lookup and pending-value forwarding for cross-artifact
  compatible forks;
- recorded capability binding isolation from live bindings and host
  configuration participation in binding/artifact identity;
- Program journal/transition-store publication and the P2 Harness file/SQLite
  adapters.

The historical checkpoint failures were:

- `HarnessProgramStoreTest.SqliteReopensExactOwnerBoundRunAndLegacyRowsStillWork`;
- `HarnessProgramStoreTest.TwoSqliteInstancesHaveOneCasWinnerAndRollbackInvalidBatch`;
- `HarnessProgramStoreTest.ReopenRejectsTamperAndMissingPublishedOutbox`.

Issue #3 records their resolution through provider-budget accounting,
deterministic host identities, durable pending atomicity, Harness conformance
coverage, transition fault injection, recovery, and the final validation gates.
Its recorded configured CTest result was 1,281 passed, three explicitly
skipped live/integration tests, and no failures; issue #3 is closed.

Current focused and configured evidence in this checkout also includes:

- 1,303 configured debug CTest tests passed serially; three live/environment
  tests were explicitly skipped and no test failed;
- the same 1,303-test CTest gate passed under TSan with ASLR disabled; the only
  suppression added during this pass covers Asio's shared executor
  reference-count implementation, not NeoGraph symbols;
- the full 1,303-test ASan/UBSan gate passed with leak detection enabled; the
  RSS stress test is intentionally skipped under ASan because its measurement
  is dominated by sanitizer shadow memory;
- documentation validators passed: 42 English sources and 126/126 translations,
  with active runtime documentation claims clean and historical claims labeled;
- the local libFuzzer canary could not run because clang/clang++ is not
  installed and package installation requires unavailable privileged access;
- five A2A mailbox/adapter regressions passed under ASan/UBSan after fixing the
  temporary `std::string_view` identity lifetime;
- the implementation audit above and issue #2 retain the remaining P3-P8
  boundaries instead of treating PR7 as the complete v1 redesign.

A PR that changes both the Core execution mechanism and Program semantics is
still too large. Split at the layer boundary and prove equivalence first.
The following is the v1 completion definition, not a claim that the current
branch has already passed every gate. Current P3 is implemented for the
bounded admitted vocabulary; P4-P8 and the external NeoCode/NeoProtocol rebase
remain explicitly partial or blocked in the implementation audit above.


### Stored-data migration

Every retained format receives a schema version and one of four dispositions:

- **exact import**: identity and semantics are provably preserved;
- **converted**: deterministic converter plus round-trip/replay fixture;
- **drain only**: pinned legacy runtime completes it, but no new runs use it;
- **blocked**: operator action is required.

There is no “best effort” checkpoint conversion. Old and new records may coexist
only while a pinned run or explicit retention reference requires both.

## 6. Phase plan

### P0 — Decision, audit, and baseline

Deliverables:

- land `V1_ARCHITECTURE.md` and this plan;
- mark old VM/Kernel documents and specs as superseded history;
- update #187/#252 and issue dispositions;
- finish #188/#189/#190/#230 bounded checks;
- record Core behavior, sanitizer, package, and performance baselines.
- reconcile version metadata and cut the bounded Classic reference release;

Exit gate: one unambiguous architecture decision, every open issue classified,
and every carried requirement assigned to a phase.

### P1 — Optional Program component and Core boundary

Deliverables:

- `NEOGRAPH_BUILD_PROGRAM`, `neograph::program`, install/export component;
- stable diagnostics, `ProgramSource`, `ProgramBuilder`, `ProgramBundle` schema;
- `ProgramCompiler` that compiles exactly one strict Core graph call;
- immutable registry/admission snapshots with explicit semantic versions and implementation digests;
- Core `graph::RunScope` budget/effect boundary and nonblocking checkpoint adapter;
- PImpl or otherwise ABI-stable long-lived Program service objects;
- Core cleanup for owned tools, typed state descriptor foundation, and channel
  lifecycle contract without adding Program dependencies.

Exit gate: deterministic bundle compilation, zero dispatch on rejection,
Core-only installed consumer unchanged, Program installed consumer working.

### P2 — End-to-end vertical slice and adapter convergence

Deliverables:

- `ProgramCatalog`, `ProgramRuntime`, `ProgramHandle`, `ProgramResult`, and in-memory ProgramStore;
- direct pinned `GraphEngine` generation cache;
- one `call_core` Program that runs/resumes/cancels/streams/checkpoints;
- minimal `ProgramJournal` commit binding Program continuation, remaining budget, and Core checkpoint identities;
- existing durable Harness record-store adapter for P2 reconnect proof; P4 replaces it with the general ProgramStore backend;
- owned protocol-neutral invocation boundary and shared conformance fixture;
- Harness translation to ProgramSource and delegation to the public Program API.

Exit gate: Core-direct and Program-wrapped results are equivalent; current
Harness conformance passes through Program; no Harness-only execution path.

### P3 — Non-child orchestration vocabulary
Status (2026-08-05): **Implemented for the currently admitted non-child vocabulary.**
The typed direct-dispatch path covers sequence, branch, bounded loop/retry,
parallel/race/quorum/map, await, cancel, checkpoint, emit, return, and Core
operations. Durable `spawn`/child `await` remains the P6 child path; arbitrary
dynamic `Send` and topology mutation remain out of scope.

Order:

1. sequence, branch, return;
2. bounded loop and retry sugar;
3. parallel, race, cancellation;
4. await, emit, checkpoint;
5. static/dynamic DSL constructs and explicit subgraph persistence modes.

Exit gate: one reference Agent Program uses every P3 primitive, every failure has
a stable terminal/diagnostic class, and GraphEngine gains no Program-specific
branch. Full vocabulary completion, including `spawn`, is gated on P6.

### Execution implementation invariant

The Program control semantics must not be implemented as a second generic
bytecode VM by default. The compiler/runtime path is:

```text
admitted source -> typed immutable plan -> direct scheduler dispatch
```

The plan may provide VM-like control semantics, but it does not require a
bytecode encoder/decoder, stack machine, JIT, or a second Core execution
boundary. Inline operations stay in memory; durable child publication,
`await`/join, checkpoint, and external-effect publication pay persistence cost
only at their semantic boundaries.

This is a performance and correctness contract, not permission to drop
durability. A typed direct-dispatch implementation must retain stable
diagnostics, source coordinates, cancellation, budgets, and replay identity.
The narrow historical VM comparison is evidence for measurement, not a
portable performance promise. Any future generic interpreter requires a new
measured architecture decision.

### P4 — Versions, activation, and durable stores
Status (2026-08-05): **Partial.** In-memory and SQLite catalog/store activation,
rollback, owner isolation, retention, and migration records are implemented.
PostgreSQL Program transition persistence and backend-parity process-restart
proof remain open.

Deliverables:

- ProgramCatalog lifecycle for admitted ProgramVersion records and owner-scoped
  ProgramStore;
- activation compare-and-swap, pinned admitted runs, rollback;
- SQLite and PostgreSQL ProgramStore/ProgramTransitionStore implementations,
  reference-aware GC, and restart recovery;
- one backend-neutral persistence contract suite covering atomic journal/effect
  publication, owner isolation, reconnect, fork, corruption/tamper rejection,
  retention, and GC against both backends. PostgreSQL remains an opt-in
  component, but it is not deferred past v1 when `NEOGRAPH_BUILD_POSTGRES=ON`.

Exit gate: two scopes activate different versions in one process; old runs stay
pinned; new runs see atomic changes; no activation lookup occurs per Core step;
and SQLite and PostgreSQL pass the same persistence contract suite across a real
process restart.

### P5 — Migration, journal, and effect safety
Status (2026-08-05): **Partial.** Migration classes, exact compatible forks,
journal CAS, pending input/effect records, outbox binding, and ambiguity
classification are implemented locally. The runtime-level crash gate spanning
Core checkpoint, pending value, Program transition, and journal publication
remains open.

Deliverables:

- MigrationPlan and five compatibility classes;
- ProgramJournal migration publication and durable effect-outbox hardening;
- narrow compatible checkpoint fork; fail-closed default;
- ambiguous non-idempotent effect reconciliation.

Exit gate: one compatible fork succeeds; all incompatible cases preserve
source-visible state, published lineage, journal, and effects; crash injection
yields no false success or untracked duplicate.

### P6 — Child Programs, modules, and extension cleanup
Status (2026-08-05): **Partial.** Child modules, receipts, bounded durable
spawn/await, authority attenuation, lineage, and artifact/LRO primitives exist.
The MCP transport split and SchemaProvider parser/transport split remain open.

Deliverables:

- typed child ports, bounded depth/count, parent budget transfer, authority
  attenuation, and lineage;
- immutable verified module coordinates, dependencies, receipts, whole-Program
  compile, quarantine/revocation;
- durable `spawn` publication through the Program API, durable parent/child
  `await`/join state, cancellation propagation, duplicate-dispatch handling,
  and restart recovery;
- MCP transport split, SchemaProvider mapper/transport/parser split;
- generated artifacts/LRO contract, then injectable SchemaProvider primitives.

`spawn` is not an independently recoverable sub-agent until its child run,
parent relationship, budget reservation, lifecycle, and dispatch outcome
survive a process restart. An in-memory child link is only an optimization.
No adapter may introduce a parallel Control VM/Durable Kernel API to bypass
this contract.

Exit gate: a Program composes and runs a verified child module, replay shows the
whole lineage, independently valid but incompatible modules fail link, and a
crash/restart at the child publication/dispatch boundary produces neither a
lost child nor an untracked duplicate.

### P7 — Tenant and host capability boundary
Status (2026-08-05): **Partial.** Host admission, owner-scoped Program/A2A
identity, and consent/attenuation foundations exist. End-to-end tenant,
authenticated host-model, and credentialless trusted global-MCP boundaries
remain open.

Deliverables:

- owner scope through Program/Core stores, IDs, caches, providers, tools,
  credentials, quota, and retention;
- separate control-plane and data-plane permissions;
- safe authenticated host-model adapter;
- hardened local global-MCP adoption with explicit trust and no credential
  extraction; disabled by default in multi-tenant mode.
- explicit cross-owner collaboration links with authenticated consent,
  artifact/capability allowlists, expiry, cancellation rights, and zero
  ambient credential or history sharing;

Exit gate: same-public-ID cross-tenant matrix has zero leakage; no ambient host
credential can enter a tenant Program.

### P8 — SDK convergence and v1 cutover
Status (2026-08-05): **Partial.** Core and Program quickstarts plus the example
disposition manifest exist, but only the quickstarts are verified. ACP/gRPC
Program cutover, component/example smoke coverage, SDK/ABI/storage freeze, and
translated documentation remain open.

Deliverables:

- one Core C++ quickstart and one Program C++ quickstart;
- the machine-readable `spec/neograph-example-disposition-v1.json` manifest
  classifying every existing example and cookbook entry as Core-kept,
  Program-ported, protocol-adapter-ported, historical-only, or removed with
  rationale;
- build/run smoke proof for every retained example, including minimum runnable
  coverage for SQLite, PostgreSQL, MCP, A2A, ACP, and gRPC where the component is
  enabled;
- selected Python Program/generated-artifact bindings based on actual ability;
- MCP/HTTP/CLI/Python/A2A/ACP/gRPC adapters pass the matrix in Section 5 and the
  same Program conformance suite;
- two independently owned Program runtimes complete the pair-programming
  conformance scenario through A2A, including progress/artifact exchange,
  correction, cancellation, retry, restart, duplicate dispatch handling, and
  owner-isolation assertions;
- migrate/remove compatibility setters, raw ownership, global admission fallback,
  old Harness compiler/runtime, ControlVm, and obsolete schemas;
- exact import/convert/drain/block decision for retained data;
- synchronized CMake exports, dependency policy, ABI policy, changelog, and
  translated user docs.

Exit gate: no duplicate public concept, no stale architecture claim, no
unclassified retained example/cookbook, and the v1 ABI/storage/dependency freeze
passes installed-consumer, adapter, example, and persistence gates.

## 7. Pull-request sequence

Each row is independently reviewable and revertible. Do not combine adjacent
rows merely to reduce PR count.

| PR | Scope | Required proof |
|---:|---|---|
| 1 | Architecture/SDD/issue disposition | Docs links, YAML/JSON validation, independent architecture review |
| 2 | Program CMake target + empty public component | Core-only and Program installed consumers on static/shared builds |
| 3 | Diagnostics + ProgramSource + bundle schemas | invalid corpus, deterministic serialization, schema round-trip |
| 4 | Immutable registry/admission snapshots | local isolation, global-miss rejection, explicit implementation digest changes bundle identity |
| 5 | Single-`call_core` ProgramCompiler | compile rejection dispatch counter stays zero |
| 6 | ProgramCatalog/Runtime/Handle/Result + minimal ProgramJournal | control/data authorization separation; Core-direct equivalence; in-process cancellation/resume/checkpoint |
| 7 | Harness delegates to Program | existing Harness tests and retained reconnect/tamper fixtures |
| 8 | Owned RunInvocation + shared protocol suite | A2A/ACP/gRPC conformance and deliberately broken adapter negative test |
| 9 | Sequence/branch/loop/retry | reference semantics and budget accounting |
| 10 | Parallel/race/await/cancel | teardown stress, loser cancellation, deterministic join rules |
| 11 | ProgramVersion admission and activation in the in-memory catalog | concurrent CAS/admission/rollback tests |
| 12 | SQLite and PostgreSQL Program stores and GC; backend work may land as stacked review slices | shared restart, retention, owner-isolation, corruption/tamper, transaction-atomicity, and GC contract suite |
| 13 | MigrationPlan + effect outbox and ProgramJournal hardening | crash matrix, semantic source preservation, effect identity |
| 14 | Child Program and module receipts | authority/budget denial matrix, whole-link failures |
| 15 | Tenant ownership | same-ID cross-scope and credential/cache/quota isolation |
| 16 | Host model/global MCP adapters | clean profile, process-tree cancel, no secret extraction, local-only guard |
| 17 | SDK cleanup and v1 removal | all callers migrated, installed consumers, storage migration, docs |

### PR7 implementation checkpoint — historical closure and current boundary

The end-of-day checkpoint below recorded the first Harness-to-Program cutover
and intentionally preserved a failing durable-adapter boundary. That boundary
was subsequently closed by the PR7 hardening work recorded in issue #3 and is
not a current failure claim.

The completed local PR7 slices are:

- Harness request translation, Program compilation/admission, runtime start,
  resume, cancel, reconnect, recorded replay, and compatible fork projection;
- owner-scoped Program artifact/run lookup and reconnect snapshots;
- typed pending input/effect persistence, exact-call reconciliation, TTL
  handling, and one-winner resume compare-and-swap;
- source-transition lookup and pending-value forwarding for cross-artifact
  compatible forks;
- recorded capability binding isolation from live bindings and host
  configuration participation in binding/artifact identity;
- Program journal/transition-store publication and the P2 Harness file/SQLite
  adapters.

The historical checkpoint failures were:

- `HarnessProgramStoreTest.SqliteReopensExactOwnerBoundRunAndLegacyRowsStillWork`;
- `HarnessProgramStoreTest.TwoSqliteInstancesHaveOneCasWinnerAndRollbackInvalidBatch`;
- `HarnessProgramStoreTest.ReopenRejectsTamperAndMissingPublishedOutbox`.

Issue #3 records their resolution through provider-budget accounting,
deterministic host identities, durable pending atomicity, Harness conformance
coverage, transition fault injection, recovery, and the final validation gates.
Its recorded configured CTest result was 1,281 passed, three explicitly
skipped live/integration tests, and no failures; issue #3 is closed.

Current focused evidence in this checkout also includes:

- 83 focused ASan/UBSan tests passed with leak detection and fail-fast
  sanitizer settings;
- the same 83 focused contention, tool, host, ledger, ACP, and checkpoint
  tests passed under TSan with ASLR disabled as required by the build;
- the implementation audit above and issue #2 retain the remaining P3-P8
  boundaries instead of treating PR7 as the complete v1 redesign.

A PR that changes both the Core execution mechanism and Program semantics is
still too large. Split at the layer boundary and prove equivalence first.
## 8. Test and measurement strategy

### Behavior

- For every new compiler diagnostic, add one source fixture that proves no
  execution dispatch occurs.
- For every Program primitive, test normal, boundary, cancellation, timeout,
  budget, checkpoint/resume, and error behavior.
- Keep a small executable reference semantics for randomized Program-plan tests.
- Compare Core-direct and Program-wrapped execution until adapter removal.

### Concurrency and lifetime

- Race activation, admission, rollback, cancellation, journal publication, GC,
  and shutdown.
- Declare callback target lifetime before the source of callbacks and drain all
  workers/coroutines before destruction.
- Run applicable TSan stress separately from ordinary development builds.

### Persistence

- Crash before and after every transaction-visible boundary.
- Restart in a new process, not just reopen an object in one process.
- Verify exact version, registry, policy, budget, effect, and checkpoint lineage.
- Corrupt/tamper every identity field and prove fail-closed behavior.

### Dependency and packaging preservation

The Core/Program redesign does not authorize dependency substitution. The
baseline remains yyjson, cpp-httplib, standalone Asio, concurrentqueue,
cppdotenv, SQLite3, libpq, and opt-in protobuf/gRPC and libcurl. A change to one
of those choices requires a separate accepted decision with:

- same-host performance and allocation evidence for affected hot paths;
- binary-size, compile-time, ABI, license, security, and supported-platform
  comparison;
- static/shared installed-consumer and CMake export verification;
- an explicit migration/removal plan proving no duplicate dependency stack
  remains.

No dependency replacement may be hidden inside a Program phase or protocol
adapter PR. Ordinary component additions must preserve default-off gates for
heavy optional dependencies.

### Performance

Maintain separate measurements for:

- Core built-in `seq` and `par` (`worker_count=1`) hot paths;
- warm single-`call_core` Program overhead;
- each Program scheduling primitive;
- compile, activation, migration, and recovery control paths;
- allocations, RSS, checkpoint size, retained-version size, p50/p95, throughput.

For performance-relevant work, compare fresh Release baseline and candidate
worktrees with identical compiler/options and CPU pinning. Discard two warm-ups
and retain ten raw process samples. Core median must remain within `1.10x` and
nearest-rank p95 within `1.15x` of the same-host baseline. A failure gets one
paired 20-sample rerun; either limit still failing blocks the change. Report raw
samples, median, p95, and variance; do not move the limits after seeing results.

## 9. Documentation plan

Do not translate unstable internals phase by phase. Documentation tiers are:

1. **Architecture source**: `V1_ARCHITECTURE.md`, updated with every public
   decision change. Detailed implementation gates remain private.
2. **Developer reference**: generated/Doxygen public API after each public phase.
3. **User guides**: Core and Program quickstarts after P2/P3 semantics stabilize.
4. **Migration guide and translations**: after P8 cutover names and behavior are
   fixed.

The P8 example/cookbook manifest is a cutover gate, not an informal inventory.
`spec/neograph-example-disposition-v1.json` is the source of truth: every
tracked entry names its target layer, required optional components, build/run
command, expected observable result, documentation links, and removal rationale
when deleted. The two focused quickstarts are the first `verified` entries;
component-matrix compilation and representative runnable proof for the remaining
retained entries are explicit follow-up work rather than implicit claims.

Historical documents keep a visible superseded banner and link to the current
decision. They are removed only when no issue, migration fixture, or retained
artifact needs them.

## 10. Risk register

| Risk | Prevention / decision |
|---|---|
| Program becomes a VM under another name | Keep typed orchestration nodes coarse; no instruction stack, bytecode, JIT, or node execution. Revisit only from measured inability. |
| Program taxes Core | One-way CMake dependency; Core benchmark with Program disabled and enabled-but-unused. |
| Duplicate runtime ownership | GraphEngine alone executes nodes; Program alone owns Program lifecycle; stores/journal contracts have one writer per record. |
| False migration compatibility | Reject by default; explicit field-by-field MigrationPlan and exact source preservation. |
| Budget or authority reset | One durable parent ledger and subset transfer only. |
| Duplicate effects after crash | Stable durable effect identity; ambiguous outcomes block, never auto-redispatch. |
| ABI freeze too early | Use the pre-v1 rebuild boundary now; freeze only after P8 conformance and installed consumers. |
| Huge rewrite stalls | Vertical slice first, then one primitive/lifecycle boundary per PR. No repository-wide move. |
| Old issues drive duplicate work | Maintain the classification table; open state alone never means pending. |
| Python surface grows by symmetry | Bind only abilities that Python libraries cannot reproduce while preserving NeoGraph lifecycle. |
| Performance claims overgeneralize | Keep #231/#255/#260 as isolated measured lanes with raw samples and explicit scope. |

## 11. Completion definition

The redesign is complete only when:

- Core works and packages without Program;
- Program compiles, inspects, runs, checkpoints, resumes, cancels, versions,
  activates, migrates where proven, and composes child modules through Core;
- C++, Harness/MCP, and other adapters share one Program contract;
- every run is attributable to immutable source/bundle/version/registry/policy/
  capability/Core identities;
- every expected terminal condition is machine-readable;
- no run widens authority, replenishes budget, mutates a live graph, or executes
  raw source;
- crash/restart/replay and cross-tenant matrices pass;
- Core and Program performance gates are reported with raw evidence;
- no superseded VM/Kernel or Harness-only architecture is described as current;
- the v1 C++ ABI and stored schemas are deliberately frozen.
- SQLite and opt-in PostgreSQL Program persistence have backend-parity contract
  proof;
- MCP, HTTP, CLI, Python, A2A, ACP, and gRPC have explicit cutover dispositions
  and supported surfaces pass shared Program conformance;
- every existing example and cookbook entry is ported, retained with an explicit
  Core/protocol role, archived as historical, or removed with rationale;
- the dependency baseline is unchanged, or every approved substitution has the
  separate evidence and migration record required by the dependency gate.
