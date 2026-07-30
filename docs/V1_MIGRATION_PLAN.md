# NeoGraph v1 Redesign and Migration Plan

Status: Proposed execution plan
Date: 2026-07-31
Architecture: `V1_ARCHITECTURE.md`
Implementation gates: maintained as private project controls
Audited source: `d80c316de1f3a10f0948477c3689a0b1b80d771b`

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

A fresh Release build at the audited commit completed. CTest reported zero
failures across 845 registered tests; four live/external-service tests were
skipped by their existing conditions. The ASan/UBSan run likewise reported
zero failures with the same four conditional skips.

The local Core performance baseline was remeasured with
`taskset -c 0 ./build-v1-baseline/bench_neograph 100000 5000 1 1`. After two
discarded warm-up process runs, ten Release process samples were retained:

| Workload | Raw samples (us) | Median | Nearest-rank p95 | Population stddev |
|---|---|---:|---:|---:|
| `seq`: three incrementing nodes | `5.84083, 5.98021, 6.30862, 6.16142, 6.58091, 8.00288, 6.51198, 6.77576, 6.64864, 6.30510` | `6.41030 us` | `8.00288 us` | `0.57062 us` |
| `par`: five-way fan-out plus join, `worker_count=1` | `13.1746, 14.3094, 15.6606, 13.5228, 14.1794, 15.3024, 14.9657, 15.1876, 16.2286, 14.4484` | `14.70705 us` | `16.2286 us` | `0.90167 us` |

These are same-host comparison baselines, not portable release promises.

### Classic release cut

`d80c316` is the candidate code baseline for one final pre-v1 **Classic**
reference release after the bounded #188/#189/#190/#230 checks pass. Classic is
not a second permanent product line or execution engine:

- first reconcile historical `v3.0.0` tags with the current `0.11.x` project
  metadata and choose one release number under the repository release rules;
- publish the exact compiler/platform/test/performance evidence above;
- announce that v1 C++ consumers rebuild and stored Program formats migrate;
- keep only severe correctness, security, data-loss, and build-break fixes on
  the Classic line while v1 is built;
- do not backport Program, activation, child composition, or new DSL semantics
  to Classic.

## 2. Decision cut

### Keep

- `neograph::core` as the lightweight required library.
- `neograph::graph::GraphEngine` and strict Core JSON as the direct-use engine.
- Existing channel/reducer/condition/checkpoint/store/provider/tool semantics
  when they are internally coherent.
- Immutable compilation before execution.
- Explicit cancellation, retry, event, usage, checkpoint, and effect identity.

### Add

- optional `neograph::program` public library;
- Program source, compiler, immutable bundle/version, runtime, handle, result,
  store, journal, activation, migration, child Program, and module concepts;
- one small typed orchestration vocabulary above Core;
- owner-scoped version and run identity;
- Program conformance suites shared by C++, Harness/MCP, and other adapters.

### Retire

- sole Control VM and separate Durable Kernel as the target architecture;
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
| #189 WASM smoke sources | **Partial.** Missing Core sources and behavior checks landed; README still states the wrong default worker mode. | Correct the sentence, rebuild with Emscripten, run browser smoke, then close. | Docs/Release / P0 |
| #190 dr_compare workers | **Partial.** Code and docs use worker count 4. | Run mock once; run one low-cost live-provider sample only when a key is available, then close with call metadata. | Performance / P0 |
| #192 Provider dispatch | **Superseded.** ABI-safe `CompletionProvider::do_invoke` replaced the proposed `Provider::invoke` cutover. | Close as superseded; keep the old Provider vtable, require new implementations to use CompletionProvider. | Core/Release / P0 |
| #193 checkpoint dispatch | **Partial.** New capability interfaces remove recursion for compliant backends, but legacy defaults still mutually recurse and sync-only stores still block async executors. | Add a nonblocking sync-store adapter, migrate internal callers, and retain regression tests for zero-override and sync-only implementations before closing. | State/Core / P1-P2 |
| #214 `RunInvocation` | **Pending.** A2A, ACP, and gRPC still construct `RunConfig` and call GraphEngine separately. | Define one owned protocol-neutral invocation that can target Core now and Program after cutover; do not create a universal protocol adapter. | Protocol / P2 |
| #215 invocation contracts | **Partial.** A2A/ACP have separate cases; no shared suite or gRPC parity. | Build a parameterized contract suite on #214 covering cancellation, identity, events, policy, Store, terminal states, pressure, and shutdown. | Protocol / P2 |
| #216 engine surfaces | **Partial.** Complete config and `GraphAdmin` exist, but execution-only dependency and admin concurrency policy do not. | Keep Core construction/run/admin responsibilities explicit; make adapters depend on a narrow invocation capability. | Core / P1-P2 |
| #217 tool ownership | **Partial.** `ToolSet` is safe; legacy `NodeContext::tools` can still dangle. | Make owned `ToolSet`/sealed Program capability imports standard; remove the raw transfer path at the v1 rebuild boundary. | Core/Program / P1 |
| #218 scoped registries | **Partial.** Local overlays and isolation tests exist; global fallback remains. | Freeze immutable registry snapshots and remove ambient fallback for strict Core/Program admission. Migrate Python registration deliberately. | Core/Program / P1 |
| #219 MCP transport split | **Partial.** Internal sessions exist, but protocol/tool code still branches on HTTP vs stdio. | Add one MCP transport capability with owned cancellation, timeout, shutdown, and error translation. | Protocol / P6 |
| #220 SchemaProvider split | **Partial.** Request mapping has a test seam; parsing, transport, pools, and callback ownership remain coupled. | Extract value-level parsers, then transport strategy; preserve the Provider-facing contract. | Extension/Protocol / P6 |
| #230 Galaxy A34 benchmark | **Partial.** Measurement exists; async-off CMake benchmark guards and repository result documentation do not. | Fix target guards, reproduce from one source tree, keep the result device-specific, then close. | Performance/Release / P0 |
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
| #256 bounded DSL | **Partial.** Static topology, limited condition, and typed ports are present; dynamic send/interrupt and sealed recursive subgraphs remain. | Move supported syntax into Program frontend; implement lifecycle-sensitive constructs only through Program state/checkpoint contracts. | Program/State / P2-P3 |
| #257 immutable generations | **Pending.** Existing immutable artifacts and compatible fork are foundations, not activation/version/migration protocol. | Becomes ProgramVersion, activation CAS, pinned runs, MigrationPlan, child attachment, lineage, and GC. | Program/State / P4-P5 |
| #260 fusion/AVX2 | **Partial experiment.** Fusion and compiler vectorization were useful for one sidecar shape; direct AVX2 had no proven win. | Publish corrected benchmark commit; design typed numeric buffer and safe fusion rules before any production candidate. No direct AVX binding. | Core/Performance / independent lane |

### Issue housekeeping order

1. Close/reclassify #112, #179, #192, and #254 with the evidence above.
2. Finish the bounded validation items in #188, #189, #190, and #230.
3. Rewrite #187 and #252 to point at Core + Program and this plan.
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
| `neograph::program_postgres` | Program + PostgreSQL | Add only when the production tenant control plane requires it |
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
| Control VM bytecode | typed immutable Program plan | No bytecode encoder/decoder/JIT in default architecture. |
| Durable Kernel | existing Core state/checkpoint/store + Program journal | Split ownership by layer; no second engine object. |
| SchemaProvider private strategy enums | request mapper + transport + parser + injected primitive registry | #241 contract before #242 extensibility. |
| Python `llm::Agent` parity | Python GraphEngine/Program | Do not bind; expose Program abilities and generated artifacts where Python cannot reproduce lifecycle semantics. |

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

Order:

1. sequence, branch, return;
2. bounded loop and retry sugar;
3. parallel, race, cancellation;
4. await, emit, checkpoint;
5. static/dynamic DSL constructs and explicit subgraph persistence modes.

Exit gate: one reference Agent Program uses every P3 primitive, every failure has
a stable terminal/diagnostic class, and GraphEngine gains no Program-specific
branch. Full vocabulary completion, including `spawn`, is gated on P6.

### P4 — Versions, activation, and durable stores

Deliverables:

- ProgramCatalog lifecycle for admitted ProgramVersion records and owner-scoped ProgramStore;
- activation compare-and-swap, pinned admitted runs, rollback;
- SQLite store, reference-aware GC, restart recovery;
- PostgreSQL only after a real tenant deployment requires it.

Exit gate: two scopes activate different versions in one process; old runs stay
pinned; new runs see atomic changes; no activation lookup occurs per Core step.

### P5 — Migration, journal, and effect safety

Deliverables:

- MigrationPlan and five compatibility classes;
- ProgramJournal migration publication and durable effect-outbox hardening;
- narrow compatible checkpoint fork; fail-closed default;
- ambiguous non-idempotent effect reconciliation.

Exit gate: one compatible fork succeeds; all incompatible cases preserve
source-visible state, published lineage, journal, and effects; crash injection
yields no false success or untracked duplicate.

### P6 — Child Programs, modules, and extension cleanup

Deliverables:

- typed child ports, bounded depth/count, parent budget transfer, authority
  attenuation, and lineage;
- immutable verified module coordinates, dependencies, receipts, whole-Program
  compile, quarantine/revocation;
- MCP transport split, SchemaProvider mapper/transport/parser split;
- generated artifacts/LRO contract, then injectable SchemaProvider primitives.

Exit gate: a Program composes and runs a verified child module, replay shows the
whole lineage, and independently valid but incompatible modules fail link.

### P7 — Tenant and host capability boundary

Deliverables:

- owner scope through Program/Core stores, IDs, caches, providers, tools,
  credentials, quota, and retention;
- separate control-plane and data-plane permissions;
- safe authenticated host-model adapter;
- hardened local global-MCP adoption with explicit trust and no credential
  extraction; disabled by default in multi-tenant mode.

Exit gate: same-public-ID cross-tenant matrix has zero leakage; no ambient host
credential can enter a tenant Program.

### P8 — SDK convergence and v1 cutover

Deliverables:

- one Core C++ quickstart and one Program C++ quickstart;
- selected Python Program/generated-artifact bindings based on actual ability;
- MCP/HTTP/CLI/Python adapters share the same Program conformance suite;
- migrate/remove compatibility setters, raw ownership, global admission fallback,
  old Harness compiler/runtime, ControlVm, and obsolete schemas;
- exact import/convert/drain/block decision for retained data;
- synchronized CMake exports, ABI policy, changelog, and translated user docs.

Exit gate: no duplicate public concept, no stale architecture claim, and the v1
ABI/storage freeze passes installed-consumer and persistence gates.

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
| 12 | SQLite Program store and GC | restart, retention references, corruption/tamper tests |
| 13 | MigrationPlan + effect outbox and ProgramJournal hardening | crash matrix, semantic source preservation, effect identity |
| 14 | Child Program and module receipts | authority/budget denial matrix, whole-link failures |
| 15 | Tenant ownership | same-ID cross-scope and credential/cache/quota isolation |
| 16 | Host model/global MCP adapters | clean profile, process-tree cancel, no secret extraction, local-only guard |
| 17 | SDK cleanup and v1 removal | all callers migrated, installed consumers, storage migration, docs |

A PR that changes both the Core execution mechanism and Program semantics is too
large. Split at the layer boundary and prove equivalence first.

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
