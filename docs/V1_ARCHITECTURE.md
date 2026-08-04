# NeoGraph v1 Core + Program Architecture

Status: Accepted architecture decision
Date: 2026-07-31
Source baseline: `d80c316de1f3a10f0948477c3689a0b1b80d771b`
Revisit trigger: a measured workload requires a second execution engine, or the
Core execution path cannot satisfy the Program contract without per-step
interpretation overhead.

## Decision

NeoGraph v1 has two public layers and one execution engine:

1. **Core** is the embeddable graph engine. The installed target remains
   `neograph::core`; the C++ graph types remain in `neograph::graph` so existing
   code is not renamed merely for branding.
2. **Program** is the optional agent-program layer. It compiles declarative
   orchestration into immutable, versioned plans that invoke pinned Core graph
   generations.
3. **GraphEngine remains the only node-execution engine.** Program does not add
   a bytecode VM or a second executor for Core/application nodes. Durable child
   orchestration, when enabled, remains Program-owned and must not become a
   second node engine or a public `ControlVm`/`DurableKernel` API.

The architectural boundary is therefore:

```text
C++ builder / JSON / model-authored source
                  |
                  v
        ProgramCompiler (structural compile)
                  |
                  v
             ProgramBundle
                  |
                  v
 ProgramCatalog (publish/admit/activate/materialize)
                  |
                  v
 ProgramVersion + sealed Core generations
                  |
                  v
       ProgramRuntime (orchestration)
                  |
                  v
      GraphEngine (node execution)
                  |
                  v
 checkpoint / store / provider / tool / events
```

### Durable child execution contract

The terms `ControlVm` and `Durable Kernel` describe rejected *separate public
execution-engine designs*, not responsibilities that disappear. The default
contract is:

- `ProgramCompiler` emits a typed immutable orchestration plan;
  `ProgramRuntime` schedules that plan; and `GraphEngine` remains the only
  executor of Core/application nodes.
- Inline plan composition may execute inside one Program run. It is an
  optimization for bounded local orchestration and is not a durable
  sub-agent.
- A durable `spawn` creates a separately admitted and pinned child Program
  run through `ProgramRuntime` and returns a `ProgramHandle`. It must not be
  implemented as recursive execution of the child body inside the parent's
  `RunControl`.
- Parent/child identity, budget reservation and attenuation, lifecycle,
  `await`/join state, cancellation, retry/resume, and lineage are durable
  Program state. In-memory weak links and caches may accelerate this path but
  are never its source of truth.
- The publication/dispatch boundary is crash-safe and idempotent. Unknown
  non-idempotent external effects use the Program effect outbox and explicit
  reconciliation rather than an implicit retry.
- Any durable child supervisor or worker dispatcher is an internal Program
  component behind the existing public Program API. Adapters must not gain a
  second `ControlVm`/`DurableKernel` execution surface or select different
  semantics.

`ProgramRuntime::start_child()` and the typed DSL `spawn` now implement this
contract. `spawn` resolves an admitted `ModuleLinkReceipt`, durably records
`Publishing -> Dispatched -> terminal` child state, and `recover_children()`
reconstructs unfinished children from the parent record. The legacy inline
path remains only for operations that are not a durable `spawn`.

### Remote collaborative agent network

A2A is not only a compatibility transport for one remote agent. It is also the
network boundary for collaboration between independently operated NeoGraph
runtimes, including agents owned by different users.

The collaboration contract has one semantic surface and two transport paths:

```text
same owner / same runtime  -> typed Program port or in-process mailbox
different process / owner  -> A2A adapter over the network
```

Both paths carry the same logical message contract. The transport may differ,
but compilation, admission, budget, capability, lifecycle, cancellation,
replay, and terminal-state semantics remain Program semantics.

A task-specific coordinator may therefore compile and admit a bounded group of
Programs such as:

- a **supervisor** that observes progress, evidence, policy violations, and
  declared failure signals;
- an **executor** that owns the task-specific Core graphs, tools, and effects;
- an optional **reviewer** that checks artifacts and requests a bounded
  correction or retry.

The coordinator may spawn same-owner children through the durable child
contract. A remote collaborator owned by another user is not implicitly a child
under the caller's owner scope. It requires an explicit collaboration
invitation/link that records both owner scopes, the permitted capabilities and
artifacts, expiry, cancellation rights, and the correlation between local
Program runs and the remote A2A task.

Every collaboration message must be attributable and idempotent. Its logical
envelope includes:

```text
collaboration_link_id, sender_owner_scope, receiver_owner_scope,
sender_run_id, receiver_run_id, a2a_task_id, a2a_context_id,
message_id, correlation_id, sequence, kind, idempotency_key, payload/artifacts
```

The local `ProgramTransitionStore` remains the source of truth for each
runtime. A2A retries, acknowledgements, task snapshots, and stream events are
transport evidence; they do not replace local Program journal or terminal
publication. Unknown remote terminal or diagnostic codes must remain explicit
unknown values and must never be converted to success.

Remote collaboration is therefore a valid reason for a meaningful A2A network
path, but not for a second node executor or a second public VM. The A2A server
and client are adapters around the Program contract. A same-host pair may use a
typed mailbox for the low-latency path and use the identical envelope through
A2A when the pair is moved to separate processes or hosts. Metrics must
separate network, queue, Program scheduling, Core execution, provider, and
artifact-publication time.

The legacy A2A `GraphEngine` constructor and in-memory task store remain
source-compatible compatibility surfaces. The Program-backed A2A adapter now
binds a `RunInvocation` and an accepted collaboration mailbox record before
admission, reconnects the exact run after publication crashes, and projects
durable lifecycle state back to A2A tasks. This closes the NeoGraph-local
Program/A2A cutover; it does not classify NeoCode or NeoProtocol as rebased
consumers, and cross-host enablement remains gated on their explicit
conformance evidence.

### Cross-repository compatibility and rebase boundary

The [NeoProtocol](https://github.com/fox1245/NeoProtocol) and
[NeoCode](https://github.com/fox1245/NeoCode) repositories are historical
integration/reference snapshots, not current NeoGraph v1 compatibility
guarantees. NeoCode's recorded harness contract pins an older NeoGraph commit
and a local JSON-lines-over-stdio sidecar. NeoProtocol's federated ACP
reference likewise names the historical `neograph::acp` surface. Those
integrations must be rebased after the current Program contract is frozen;
compiling an old adapter is not compatibility evidence.

The ownership boundary remains:

- NeoGraph owns ProgramVersion, RunInvocation, admission, budgets,
  capabilities, lifecycle, journal, checkpoint/recovery, effect
  reconciliation, and owner isolation.
- NeoCode owns the user-facing session, workspace, tool, permission, and
  harness surfaces.
- NeoProtocol owns wire envelopes, capability/consent exchange, signaling,
  and remote collaboration transport.

The rebase sequence is deliberately one-way:

1. Freeze the ProgramVersion, RunInvocation, CollaborationLink, message,
   artifact, cancellation, and idempotency contracts.
2. Replace NeoCode's historical sidecar assumptions with a thin adapter to
   current Program lifecycle operations.
3. Rebase NeoProtocol's Task Offer, ACP, WebRTC, and workspace adapters onto
   the same invocation and collaboration contracts.
4. Add explicit protocol, schema, and NeoGraph contract-revision metadata;
   reject incompatible combinations before execution.
5. Run the two-runtime owner-isolation and restart/retry conformance scenario
   before enabling cross-host transport.

Old pinned bundles and sidecar records receive an explicit exact-import,
converted, drain-only, or rejected classification. They must never be
silently accepted as current Program versions. Issue #7 tracks this
cross-repository rebase gate. The machine-readable declaration in
`spec/cross-repository-compatibility-v1.json` records the comparison between
the current `ProgramVersion`, `RunInvocation`, and A2A collaboration surfaces
and those historical references. Its metadata/source check is intentionally
fail-closed: NeoCode and NeoProtocol remain `historical_only` until a consumer
declares every current contract revision and verified conformance evidence;
changing a label alone cannot make a consumer current. Run the focused check
with `python3 scripts/check_cross_repository_compatibility.py` (also wired as
`CrossRepositoryCompatibility.Metadata` when tests are enabled).

### Execution strategy: VM semantics without a general-purpose VM

Program retains the control semantics normally associated with a Control VM,
but the default implementation does not encode/decode a bytecode program or
cross a second generic VM/Core boundary.

- Admission lowers source into a typed immutable plan with stable operation
  identifiers, typed references, and statically known continuation edges.
- `ProgramRuntime` directly dispatches those plan operations through its
  scheduler/coroutine machinery. This is VM-like execution semantics without
  a separate bytecode interpreter, stack machine, or JIT.
- Inline sequence/branch/loop work stays on the fast in-memory path and does
  not perform a durable commit for every operation.
- Durable child `spawn`, `await`/join, checkpoint, and external-effect
  publication persist only at their semantic durability boundaries. The
  persistence and recovery cost of those boundaries is mandatory and must
  not be hidden in a performance claim.
- A future workload that requires arbitrary dynamic code, unbounded topology
  mutation, or a generic sandboxed interpreter reopens this decision only with
  a measured correctness and performance gate. It does not silently create a
  second adapter-specific runtime.

The specialized direct-dispatch implementation is complete for the admitted
bounded operation set: runtime dispatch selects the sealed
`ProgramOperationKind` and typed descriptor rather than reparsing operation
tags from JSON. Future operation kinds must preserve diagnostics, source
coordinates, cancellation, budgets, and replay identity before joining this
path.

Program is optional. A user who only needs a static graph links
`neograph::core`, builds a `GraphEngine`, and pays no Program dependency,
allocation, activation lookup, or branch on the run hot path.

## Product contract

NeoGraph is an embeddable C++ graph engine with an optional runtime Agent
Program compiler. A Program may be written by a developer or proposed by a
model, but source text is never execution authority. Before execution, source is
normalized and type-checked into a `ProgramBundle`; admission then checks its
capabilities, effects, budgets, Core identities, and dependencies before sealing
an authorized `ProgramVersion`.

This contract separates two claims:

- the compiler can prove that the declared Program is structurally coherent
  and bounded, while admission can prove that its declared capabilities,
  effects, and budgets fit an authorized immutable policy snapshot;
- neither compiler nor admission can prove that a model answer is factually
  correct. Behavioral evaluation and evidence gates remain runtime concerns.

### Contract-driven multi-model implementation

NeoGraph and NeoCode may use different model roles for one implementation
request, but the roles must share one immutable contract rather than an
implicit conversation. A frontier model may propose and review the contract;
it is not an execution authority or a correctness oracle. A lower-cost
implementation worker may perform the bounded code changes; it is not allowed
to redefine the task while implementing it.

The planner output is a typed manifest with these required semantic sections:

- `assumptions`: premises that must be true for the plan to be valid;
- `requirements`: observable behavior that must be delivered;
- `non_goals`: explicit scope exclusions;
- `acceptance`: stable identifiers, expected outcomes, and required evidence;
- `fixed_test_vectors`: values or fixtures that the worker cannot rewrite;
- `independent_oracles`: reference implementations, standards, hidden fixtures,
  or human decisions that do not derive their expected result from the worker;
- `risk_register`: known uncertainty, contradiction, and escalation points;
- `retry_policy`: bounded attempts, time, cost, and failure escalation.

The manifest lifecycle is `proposed -> reviewed -> frozen`. Only a frozen
manifest may select implementation work. Review must check premise completeness,
requirement consistency, acceptance observability, and whether every required
element has an oracle or an explicit human gate. The implementation worker
receives the frozen manifest and scoped workspace context. It may report a
contract gap, but it must not change requirements, expected values, permissions,
scope, retry limits, or completion status.

Verification is a separate authority from the worker's self-report. The
deterministic runner executes the declared build and test commands, and
independent or reference-based checks are used where available. Evidence is
valid only when bound to the manifest hash, Program/version identity, workspace
revision, command, toolchain, and artifact hash. Missing acceptance evidence,
unresolved blocking diagnostics, failed independent checks, or exhausted
budgets produce `blocked` or `failed`, never `completed`.

This flow is intended to reduce senior-engineer toil—context recovery,
repetitive patching, known-check reruns, and regression archaeology—while
keeping premise decisions, ambiguous trade-offs, and release approval under
senior control. It does not promise perfect software: a wrong premise can
still produce a perfectly implemented wrong result. Subjective work therefore
ends at a human decision gate with candidates and evidence rather than a false
automatic correctness claim.

This is a Harness/Program orchestration contract over the existing typed
dispatch path. It does not introduce a second execution engine, generic
bytecode VM, or permission-escalation path. Issue #8 records the implementation
and conformance gate for this contract.

## Goals

- Preserve the low-overhead Core path and its current graph semantics.
- Make Program a first-class public library rather than an MCP-only service.
- Support developer-authored and model-authored orchestration through the same
  compiler and immutable artifact format.
- Support sequence, branch, bounded loop, parallel, race, retry, cancellation,
  wait, checkpoint, child Program, and immutable version activation without a
  general-purpose VM.
- Keep programs inspectable, source-mapped, reproducible, capability-scoped,
  and replayable.
- Permit open-ended evolution as a sequence of finite admitted versions, never
  as unchecked mutation of a running graph.
- Reach v1 with one documented C++ API, one error model, and deliberate binary
  compatibility boundaries.

## Non-goals

- Executing raw model output, arbitrary C++, Python, shell, or ambient host APIs.
- Mutating a live `GraphEngine` topology in place.
- Replacing `GraphEngine` with a bytecode interpreter.
- Inventing a new persistence engine when `CheckpointStore`, `Store`, and the
  Harness journal already own the required data.
- Claiming automatic semantic equivalence between arbitrary program versions.
- Forcing Program, Harness, MCP, SQLite, PostgreSQL, LLM, A2A, or ACP dependencies
  on Core-only users.

## Layer responsibilities

### Core

Core owns the mechanics that must remain fast and reusable outside Agent
Programs:

- graph definition validation and compilation;
- immutable `GraphEngine` construction;
- channels, reducers, conditions, barriers, dynamic `Send`, and subgraphs;
- node invocation, fan-out scheduling, retry, cancellation, and interruption;
- run state, checkpoint, replay inputs, stores, event streaming, and tool gates;
- provider and tool invocation contracts;
- engine-scoped node/reducer/condition registries.

Core does not know about Program source, modules, tenant activation, child
Program lineage, Program compilation receipts, or Program policy documents.

### Program

Program owns orchestration and lifecycle above Core:

- source parsing, authoring sugar, source maps, and diagnostics;
- typed Program interfaces and module linking;
- capability/effect closure and admission policy;
- run-wide time, cost, concurrency, expansion, and instruction-like operation
  budgets;
- immutable bundles, versions, dependency receipts, and activation records;
- orchestration between pinned Core graph generations;
- child Program attachment and explicit compatible version forks;
- Program-level result, trace, and lineage views.

Program never executes a Core node itself. It asks a pinned `GraphEngine`
generation to run or resume and consumes the resulting typed outcome and events.

### Adapters

Harness C++, MCP, HTTP, CLI, selected Python bindings, A2A, ACP, and gRPC are
adapters. A2A, ACP, and MCP retain their protocol-owned JSON-RPC envelopes;
NeoGraph does not create a second generic JSON-RPC execution API. Every adapter
translates its transport or language contract into the public Program API. No
adapter owns separate compilation, admission, execution, or persistence
semantics.

Adapter wire schemas are versioned independently. An adapter must preserve an
unknown Program diagnostic/event/terminal code as an explicit unknown value or
reject the unsupported schema; it may never map an unknown terminal state to
success.

## Program semantic model

### Small execution vocabulary

The Program compiler lowers authoring sugar to a small orchestration plan:

- `call_core`: invoke one immutable Core graph generation;
- `sequence`: run children in order;
- `parallel`: run bounded children concurrently and join all;
- `race`: complete on a declared winner rule and cancel the remaining children;
- `branch`: select a child from a typed condition result;
- `loop`: repeat a child under a declared condition and hard iteration limit;
- `spawn`: admit and start a separately compiled child Program;
- `await`: wait for a declared handle/event with timeout and cancellation;
- `checkpoint`: request a durable safe point;
- `emit`: publish a typed Program event;
- `cancel`: cancel a declared scope;
- `return`: produce the Program output.

`map`, `retry`, `quorum`, reviewer/fixer loops, and other conveniences are
compiler expansions or bounded compatibility sugar over this vocabulary. They
do not add feature-specific branches to Core or GraphEngine.

This plan is not bytecode. It is a typed, immutable orchestration graph whose
nodes retain source coordinates and directly reference compiled Core generations
or other Program nodes. ProgramRuntime schedules that graph; GraphEngine remains
the only executor of application nodes.

The current P3 slice directly schedules `sequence`, `branch`, `return`, bounded
`loop`/`retry`, `parallel`, `race`, `cancel`, `await`, `emit`, and `checkpoint`.
`map` and `quorum` remain compatibility sugar with bounded direct scheduling.
`spawn` is intentionally not a durable child-program primitive yet: its admitted
vocabulary and budget checks remain for existing child-link tests, while the
durable `ProgramRuntime::start_child`/`await` lifecycle is the P6 boundary. No
new plan operation is silently treated as a no-op; malformed operations fail
admission with a typed-plan diagnostic.

ProgramRuntime is therefore an intentional second **scheduling domain**, but not
a second node executor. It owns readiness and joins for Program operations,
child handles, and the Program-wide cancellation/budget tree. GraphEngine alone
owns `GraphNode` readiness, fan-out, retry, checkpoint, and teardown inside one
`call_core`. The boundary is one owned Core invocation plus one typed terminal
result/event stream. Contract tests must cover cancellation propagation in both
directions, destruction with losing race children, checkpoint ordering, budget
debits, and equivalence with direct Core execution. ProgramRuntime may not reach
inside GraphEngine's ready queue or checkpoint state.

### Boundedness

The source language may express loops and recursive child composition, but every
admitted run is finite under one non-renewable parent budget:

```text
RunBudget
  wall_time
  model_tokens
  monetary_cost
  max_concurrency
  max_program_operations
  max_core_steps
  max_dynamic_compiles
  max_child_depth
  max_total_children
```

Child Programs and replacements receive a subset of the remaining budget.
Resume, retry, fork, rollback, and child attachment never replenish it. Budget
exhaustion is a typed terminal outcome, not a generic exception or model error.

### Authority

Every external action resolves through a sealed capability reference. The
compiler calculates the transitive capability and effect closure. Admission
checks it against the caller/tenant policy snapshot.

```text
child authority <= parent remaining authority
replacement authority <= source admitted authority
run authority <= admitted policy snapshot
```

A control-plane actor may create a new top-level run with a new policy. A
running Program cannot authorize itself.

### Enforced Core execution scope

Declaration is not enforcement: an arbitrary native `GraphNode` can call a host
API without telling the compiler. v1 therefore adds a protocol-neutral
`graph::RunScope` to Core. It carries the existing cancellation/deadline context
plus a run-scoped budget ledger, effect broker, stable invocation identity, and
child-scope derivation. Program creates the root scope; every `call_core` receives
an attenuated child view through `RunContext`.

Admitted Provider, Tool, MCP, filesystem, network, and other external resource
capabilities must reserve budget before dispatch, reconcile actual usage, and
publish non-idempotent effects through the broker's
`prepare/commit/ambiguous` protocol. The same durable parent ledger is shared by
retries, resumes, forks, and child Programs.

Native C++ cannot be sandboxed by this library. Every executable registry entry
must therefore declare `brokered` or `trusted_native` effect mode.
Multi-tenant/model-authored admission rejects `trusted_native`; an embedding
host may allow it only through an explicit broad capability and then receives
no exactly-once or complete cost-accounting claim for ambient effects. Direct
Core use remains available with a Core-owned local scope and no Program
dependency.

`brokered` is a host attestation, not a fact the compiler can derive from native
machine code. The trusted computing base therefore includes the attested entry
implementation and its resource wrappers. Model-authored source cannot choose
this classification. Multi-tenant admission accepts only owner-approved,
signed/allowlisted attestations bound to the implementation digest; untrusted
native code is rejected or isolated outside the process.

## Durable artifacts

### ProgramSource

The authored document plus its source kind, declared schema version, imports,
and source coordinates. Accepted initial frontends are C++ builder values and
canonical JSON. YAML or model-specific syntax may be added later only by
lowering to the same typed source model.

### ProgramBundle

An immutable, content-addressed compilation result containing:

```text
bundle_id
source_hash
canonical_program_hash
compiler_build_id
program_schema_version
registry_snapshot_fingerprint
module dependency Merkle root
typed input/output contracts
orchestration plan
sealed Core graph definitions + compiled-plan identities
capability/effect closure
declared budget requirements and bounds
source map
compile diagnostics/receipt
```

A behavior-changing source, registry implementation, schema, module dependency,
or Core compile identity produces a different bundle. A changed admission
profile, owner, or policy produces a different `ProgramVersion`, not a different
structural bundle.

The persisted form is a versioned schema. In-memory C++ object layout is not the
storage format.

### ProgramVersion and activation

`ProgramVersion` binds one immutable bundle to an admission profile, ownership
scope, policy snapshot, dependency receipts, and a successful Core
materialization receipt. `ProgramActivation` binds a scope to an active version
using a compare-and-swap generation:

```text
scope
active_version_id
activation_generation
policy_snapshot_hash
```

Compilation is structural and does not grant runtime authority. Admission
checks the bundle's capability/effect closure and declared budget requirements
against the caller's immutable admission and policy snapshots. Dependency
resolution, capability preflight, Core materialization, and optional warming
happen before activation. A new run reads activation once and pins a direct
reference to the complete version/runtime tuple. Ordinary Core steps perform no
activation lookup.

A persisted bundle never contains a process-local `GraphEngine` object. On
publish, admission after restart, or cache refill, the materializer verifies the
stored schema and hashes, requires the exact compatible registry/compiler
identities, rebuilds each engine from its sealed strict Core definition, and
checks the resulting compiled-plan identity. Only that verified in-memory tuple
may become activatable. Failure produces an incompatible-bundle diagnostic; it
never reparses source into new semantics. This is a control-path cost, not a run
hot-path cost.

Registry fingerprints are not hashes of names and schemas alone. Every node,
reducer, condition, Provider, Tool, and imported executable declares a
caller-supplied semantic version plus implementation digest. Those values enter
the registry snapshot and bundle identity. Opaque `std::function` targets are
never guessed or introspected. Materialization rejects an identical manifest
whose implementation identity differs.

### ProgramRun

A run pins:

- Program version and bundle identity;
- current Program plan location and child handles;
- exact Core generation for every active call;
- remaining parent budget and capability set;
- cancellation tree;
- checkpoint/journal/effect lineage;
- input, output, event, and terminal contracts.

Program checkpoints extend rather than reinterpret Core checkpoints. Each active
Core call keeps its native checkpoint identity. The Program checkpoint records
which Core checkpoint belongs to which Program operation.

NeoGraph does not assume a distributed transaction across independently backed
`CheckpointStore`, `ProgramStore`, and effect systems. The commit protocol is:
write an immutable Core checkpoint, then atomically publish the Program
continuation, remaining budget, and checkpoint reference in `ProgramJournal`,
then drain journal-owned event/effect outboxes. A crash before journal
publication leaves an unreferenced checkpoint that GC may collect; a crash
after publication replays from the published reference. Physical rollback of
the first write is not required, but false visible success is forbidden.

## Compilation and execution

### Compile path

```text
source
 -> parse and schema check
 -> normalize authoring sugar
 -> resolve immutable imports
 -> type-check ports and conditions
 -> calculate capability/effect closure
 -> lower Core fragments to strict Core definitions
 -> compile/validate/link Core generations
 -> calculate budgets and safe points
 -> seal identities and emit ProgramBundle
```

Any error returns stable machine-readable diagnostics with phase, code, source
path/span, message, and witness. No Core node, provider, tool, or capability may
be invoked during compilation.

### Admission path

```text
ProgramBundle + admission profile + owner scope + policy snapshot
 -> verify bundle and registry/compiler compatibility
 -> verify dependency receipts
 -> check capability/effect closure and declared budget bounds
 -> materialize and verify sealed Core generations
 -> emit immutable ProgramVersion and admission receipt
```

Admission has no side effects beyond publishing the new immutable version. It
does not activate the version or start a run.

### Run path

```text
resolve activation once (or receive explicit ProgramVersion)
 -> pin ProgramVersion and policy snapshot
 -> create ProgramRun and root cancellation/budget scope
 -> schedule ready Program operations
 -> call pinned GraphEngine generations
 -> atomically journal Program transitions and Core checkpoint links
 -> emit typed events
 -> return one terminal ProgramResult
```

ProgramRuntime is asynchronous internally. A synchronous `run()` is a boundary
adapter over the same asynchronous implementation, not a second execution path.

### Safe points and replacement

A running Program is never edited. Replacement is:

```text
compile/admit target version without changing activation
 -> quiesce source run at a safe point
 -> write an immutable migration-attempt checkpoint
 -> calculate MigrationPlan from that exact snapshot
 -> classify compatibility
 -> atomically publish fork binding + Program journal reference
 -> continue target run
```

Every transition is classified as one of:

- `new_runs_only`;
- `fork_compatible`;
- `drain_only`;
- `operator_reconciliation`;
- `blocked`.

A migration plan must cover or reject channels, continuations, barriers, pending
work, retry/cancellation identity, interrupts, budgets, authority, checkpoint
lineage, effects, caches, and output contracts. Rejection leaves source run,
activation, published checkpoint lineage, Program journal, and effects
semantically unchanged. An unreferenced immutable attempt checkpoint may remain
and is collected by reference-aware GC; byte-identical storage is not promised.
There is no restart-from-input fallback.

## Public C++ API shape

The names below are the intended v1 surface, not permission to add all types in
one change.

```cpp
#include <neograph/graph/engine.h>       // Core only
#include <neograph/program/program.h>    // Optional Program layer

using namespace neograph;

// Core remains directly usable.
auto core = graph::GraphEngine::build_strict(core_definition, core_config,
                                              core_resources);
auto core_result = core->run(core_run_config);

// Program compilation is structural; admission grants runtime authority.
program::ProgramCompiler compiler(program::CompilerConfig{
    .registry = registry_snapshot,
});
program::ProgramBundle bundle = compiler.compile(program_source);

program::ProgramCatalog catalog(program::CatalogConfig{
    .program_store = program_store,
    .registry = registry_snapshot,
    .engines = engine_cache,
});
program::ProgramVersion version = catalog.admit(
    bundle,
    program::Admission{
        .owner = owner_scope,
        .profile = admission_profile,
        .policy = policy_snapshot,
    });

program::ProgramRuntime runtime(program::RuntimeConfig{
    .catalog = catalog,
    .checkpoints = checkpoint_store,
    .state_store = long_term_store,
    .journal = program_journal,
});
program::ProgramHandle handle = runtime.start(
    version,
    program::Invocation{.input = input, .budget = budget});
program::ProgramResult result = handle.wait();
```

Required public concepts:

| Concept | Role |
|---|---|
| `ProgramSource` | Owned typed source document; no runtime authority |
| `ProgramBuilder` | C++ authoring convenience that produces `ProgramSource` |
| `ProgramCompiler` | Pure control-path compiler against immutable snapshots |
| `ProgramBundle` | Immutable, serializable compilation artifact |
| `ProgramVersion` | Bundle plus admission, policy, dependency, ownership, and Core materialization receipts |
| `ProgramCatalog` | Authorized control plane for publish, admit, activate, rollback, resolve, and retire |
| `ProgramRuntime` | Data plane that schedules Program operations and invokes Core engines |
| `ProgramHandle` | Cancel, await, stream, inspect identity; no mutable runtime internals |
| `ProgramResult` | Typed terminal status, output, usage, lineage, diagnostics |
| `ProgramStore` | Durable bundles, versions, activations, and run metadata |
| `ProgramJournal` | Atomic Program transition/effect record |
| `MigrationPlan` | Explicit compatibility proof or rejection evidence |

Public ownership rules:

- compile/build functions consume owned configuration where lifetime matters;
- `ProgramCatalog` owns control-plane authorization and version lifecycle;
  `ProgramRuntime` cannot publish, admit, activate, rollback, or retire a version.
- registries and policies are immutable snapshots;
- handles are cheap shared ownership of a run control block;
- no API borrows a stack callback or configuration beyond the call unless the
  signature takes ownership;
- public value types do not expose transport-specific fields;
- persisted identifiers are opaque strings with documented scope, never raw
  pointers or process-local indexes.

## Core API evolution to v1

Core is mature enough that redesign should remove ambiguity, not rename every
working type.

1. `GraphEngine::build()`/`build_strict()` with complete `EngineConfig` and
   `EngineResources` remains the standard construction path.
2. `GraphEngine::compile()` and post-build configuration setters remain through
   the pre-v1 migration window, then are candidates for removal at the announced
   v1 rebuild boundary. Runtime objects become immutable after construction.
3. `run()` and `run_async()` keep one semantic implementation. Sync is an adapter
   over async; neither may hide a different checkpoint, cancellation, or retry
   contract.
4. `RunResult` keeps channels-wrapped output as the source of truth. Typed
   channel access remains the stable convenience API.
5. Process-global executable registry fallback is removed from strict Core and
   Program admission before v1. Explicit engine-scoped snapshots are required.
6. `graph::RunScope` becomes the protocol-neutral cancellation, budget, effect,
   and invocation-identity boundary used by admitted resource capabilities.
7. Provider, tool, checkpoint, store, and event interfaces remain Core
   contracts. Program depends on them; it does not duplicate them.
8. Pre-v1 layout changes require one explicit rebuild notice. At 1.0, exported
   virtual order and public object layouts follow `docs/ABI_POLICY.md`.

## Build and package boundaries

Target dependency direction is acyclic:

```text
neograph::core
    ^
    |
neograph::program
    ^             ^              ^
    |             |              |
program_sqlite  program_postgres  neograph::harness
                                      ^
                                      |
                         MCP / HTTP / CLI / protocol adapters
```

Rules:

- `NEOGRAPH_BUILD_PROGRAM=OFF` is supported and leaves the Core binary and public
  link interface unchanged.
- `neograph::program` links Core, but Core never links Program.
- SQLite and PostgreSQL Program stores are separate optional targets and must
  pass the same persistence contract suite before v1 when enabled.
- Harness becomes a Program service, not the owner of Program semantics.
- MCP client/server types remain transport components and do not leak into
  Program headers.
- Installed CMake components report `program` only when built.
- Python bindings may expose Program when it adds NeoGraph-specific compile,
  checkpoint, activation, or lineage value; Python-standard alternatives remain
  preferred for generic JSON/schema manipulation.

Dependency selection is not part of the Core/Program redesign. The preserved
baseline is yyjson, cpp-httplib, standalone Asio, concurrentqueue, cppdotenv,
SQLite3, libpq, and opt-in protobuf/gRPC and libcurl. Substituting any of these
requires a separate architecture decision with performance, allocation,
binary-size, compile-time, ABI, license, security, supported-platform, and
static/shared installed-consumer evidence, plus removal of the replaced stack.
Heavy optional dependencies remain default-off.

## Performance contract

Baseline Core hot path was remeasured from
`24cbd86d80815b2c2b46aacb02cbf5a570503262` in a GNU 13.3 Release build. The
command was CPU-pinned, each process ran the benchmark's hot loop, two process
runs were discarded as warm-up, and ten process samples were retained:

- built-in `seq` workload (three incrementing nodes): median `5.923925 us`,
  nearest-rank empirical p95 `6.27868 us`, population standard deviation
  `0.132166 us`;
- built-in `par` workload (five-way fan-out plus join, `worker_count=1`):
  median `13.45285 us`, nearest-rank empirical p95 `14.0613 us`, population
  standard deviation `0.248471 us`.

These measurements are local WSL2 evidence, not cross-machine release promises.
They establish the comparison protocol and show that Program must not tax
Core-only execution.

Required gates:

- Core-only build: no Program-linked objects or new runtime branch in
  `GraphEngine::run*`.
- Core hot path: retain ten process samples after two warm-ups. Candidate median
  must be at most `1.10x` its same-host baseline and nearest-rank p95 at most
  `1.15x`. This applies to Program disabled and Program enabled-but-unused. A
  failure triggers one paired 20-sample baseline/candidate rerun under the same
  pinning, compiler, and flags; either threshold still failing blocks the change.
  Thresholds are fixed before implementation results are seen.
- Program explicit-version start: no source parse, canonicalization, module
  resolution, policy calculation, or activation lookup after admission.
- Active-version start: exactly one activation read before pinning; none per Core
  step.
- Warm single-`call_core` Program overhead is measured separately from Core node
  work, including allocations and scheduler wakeups.
- Compilation, activation, migration, retained-version memory, checkpoint
  growth, recovery, and throughput receive separate budgets; one aggregate
  latency number is insufficient.

No AVX, JIT, coroutine, pool, or cache optimization is accepted on theory alone.
Each remains an optional measured lever after the architecture is correct.

## Failure model

Compilation and admission use stable diagnostic codes. Runtime returns a
`ProgramResult` terminal status for expected outcomes:

- `completed`;
- `interrupted`;
- `cancelled`;
- `budget_exhausted`;
- `timed_out`;
- `failed`;
- `ambiguous_effect`;
- `checkpoint_incompatible`.

Programmer errors and violated internal invariants may throw. Provider/tool/node
failures are recorded with original classification and do not masquerade as
successful Program output.

Non-idempotent effects carry durable identity. An unknown outcome is never
silently retried; it enters `ambiguous_effect` until reconciled.

## Security and tenant rules

- Control-plane operations (publish, admit, activate, rollback, migrate, delete)
  require separate authorization from data-plane run invocation.
- Every ID lookup is scoped by tenant/owner before existence is disclosed.
- Provider credentials and MCP/CLI capabilities are host-owned resources, not
  serialized Program fields.
- Local host-login and user-global configuration adapters are rejected in
  multi-tenant mode unless an authenticated tenant broker owns the credential
  and process boundary.
- Module publication is immutable and signed or attested. Whole-Program compile
  revalidates transitive dependencies, effects, and policy.
- Replay identifies exact Program/Core/module/provider/tool versions and performs
  no unrecorded live effect.

## Observability

Every event carries:

```text
run_id, program_version_id, bundle_id, operation_id,
core_generation_id, core_run_id, parent_run_id,
trace_id, attempt, timestamp, terminal/error classification
```

Source maps connect runtime events and diagnostics to Program source spans.
Metrics separate compile, queue, Program scheduling, Core execution, provider,
tool, checkpoint, and journal time so framework overhead is measurable rather
than inferred from wall time.

## Compatibility and cutover

The current Harness DSL, strict Core documents, retained artifacts, and MCP
methods are migration inputs, not parallel permanent architectures.

- Existing strict Core JSON remains a supported Core input.
- Existing bounded Harness DSL is translated to `ProgramSource` and compiled by
  `ProgramCompiler`.
- Existing retained Harness artifacts are imported into `ProgramBundle` only
  when their hashes, registry/admission profile, and executable semantics can be
  preserved exactly. Otherwise they drain on the pinned legacy path or fail with
  an explicit compatibility classification.
- Existing MCP method names may remain as transport compatibility during the
  pre-v1 window, but their implementation delegates to Program. No second
  Harness-only compiler/runtime remains after cutover.
- The historical `ControlVm` and VM integration schemas are now retained only
  as explicitly superseded records (`docs/PROGRAMMABLE_HARNESS_DSL_DESIGN.md`,
  `spec/programmable-harness-vm-integration.sdd.yaml`, and
  `spec/programmable-harness-graph-engine-inventory-v1.json`). They are not
  public APIs or current execution claims; live compatibility APIs remain
  available until their separately announced rebuild boundary.

## Acceptance gates

The architecture is implemented only when all are true:

1. A Core-only installed consumer builds and runs without Program enabled.
2. A developer can build and run the same static graph through Core directly.
3. A developer can compile a Program containing branch, bounded loop, parallel,
   race, retry sugar, checkpoint, and child Program, then execute it through
   pinned Core generations.
4. Invalid types, unknown imports, excess budgets, capability expansion, and
   unsupported runtime constructs fail before any worker/provider/tool dispatch.
5. New-run activation is atomic and generation-checked; in-flight runs remain
   pinned.
6. Compatible fork and incompatible migration both preserve exact
   source-visible checkpoint lineage, budget, authority, effect, and behavior.
7. Crash injection around checkpoint/journal publication produces no false
   success or duplicate untracked effect.
8. Core baseline, Program overhead, compile/activation/migration costs,
   allocations, and persistence growth meet preregistered budgets.
9. Full available tests, ASan/UBSan, applicable TSan stress, persistence restart,
   replay, installed-consumer, and supported-platform builds pass.
10. Public headers, CMake components, schemas, changelog, and translated user
    documentation agree; every existing example and cookbook entry has an
    explicit Core, Program, protocol-adapter, historical, or removal
    disposition. The machine-readable disposition and proof inventory is
    `spec/neograph-example-disposition-v1.json`; entries marked
    `pending-component-smoke` are explicit remaining P8 work, not current
    compatibility claims.
11. MCP, HTTP, CLI, selected Python bindings, A2A, ACP, and gRPC have explicit
    cutover dispositions; every supported surface passes the shared Program
    conformance suite plus protocol-specific wire tests.
12. SQLite and opt-in PostgreSQL Program stores pass one backend-neutral
    restart, atomicity, owner-isolation, tamper, retention, and GC suite.
13. The dependency baseline is unchanged, or each approved substitution has the
    separate decision and evidence required by the package-boundary policy.
14. Two independently owned Program runtimes can collaborate through the A2A
   adapter with explicit consent, owner isolation, task/artifact correlation,
   cancellation semantics, restart-safe duplicate handling, and no capability
   leakage.
15. NeoCode and NeoProtocol consumers declare a current NeoGraph Program
   contract revision and pass the legacy-integration rebase/conformance gate
   before claiming v1 compatibility.

## Rejected alternatives

### Sole Control VM + Durable Kernel

Rejected as the default architecture. The measured reference path added about
`58.7 us` around a `4.3 us` VM operation and was roughly `13x` slower than the
current integrated Core path in the recorded strict-linear experiment. More
importantly, it duplicated scheduling, checkpoint, and execution ownership.
A future VM requires a concrete workload that Core + Program cannot express and
must beat explicit performance and correctness gates.

### One enlarged GraphEngine API

Rejected. Adding model-authored source, module registries, activation, tenant
policy, and child Program lifecycle directly to `GraphEngine` would couple every
Core user to control-plane semantics and make the fast path harder to reason
about.

### Harness remains the architecture

Rejected. Harness is a useful application and transport service, but keeping
its compiler, retained artifacts, and lifecycle as MCP-only concepts prevents
normal C++ users from using the same capabilities and encourages duplicate APIs.

### Mutable live graphs

Rejected. In-place topology mutation makes checkpoint, pending work, retry,
cancellation, effect, and authority semantics ambiguous. Immutable generations
plus explicit migration are safer and keep ordinary execution lock-free.
