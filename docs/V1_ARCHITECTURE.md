# NeoGraph v1 Core + Program Architecture

Status: Accepted Core architecture; source-language direction amended 2026-08-10
Date: 2026-07-31
Source baseline: `d80c316de1f3a10f0948477c3689a0b1b80d771b`
Source-language amendment:
[`QUICKJS_CONTROL_ARCHITECTURE.md`](QUICKJS_CONTROL_ARCHITECTURE.md) supersedes
this document wherever it defines JavaScript authoring or removes legacy
source-language implementations. The bounded Core authoring DSL/elaborator is
deleted; strict Core JSON, typed Core IR, and trusted C++ embedding remain.
The typed Core IR, GraphEngine, catalog, activation, authority, durability, and
tenant boundaries below remain in force.
Post-cutover controller extension:
[`SELF_EVOLVING_AGENT_CONTROLLER.md`](SELF_EVOLVING_AGENT_CONTROLLER.md)
defines developer-authorized profiles, machine-readable capability compilation,
immutable self-evolution, reusable Harness memory, and the evidence boundary for
any general-agent-controller claim. It reuses, rather than replaces, the
retained boundaries in this document.

## Decision

NeoGraph v1 has two public layers and one execution engine:

1. **Core** is the embeddable graph engine. The installed target remains
   `neograph::core`; the C++ graph types remain in `neograph::graph` so existing
   code is not renamed merely for branding.
2. **Program** is the optional agent-program layer. General Program source is
   standard JavaScript executed by embedded QuickJS; Program admission binds it
   to immutable versions, sealed native/Core imports, and nonrenewable budgets.
3. **GraphEngine remains the only node-execution engine.** QuickJS interprets
   Program control code and yields typed commands to `ProgramRuntime`; it does
   not execute Core/application nodes or own Core scheduling, persistence, or
   effect commit.

The architectural boundary is therefore:

```text
JavaScript define()/main() source + sealed modules
                       |
                       v
      bounded QuickJS contexts + admission
                 /                 \
                v                   v
 define(): validated strict     main(): ProgramBundle
 Core definition                     |
        |                             v
        |                ProgramCatalog (publish/admit/
        |                  activate/materialize)
        |                             |
        |                             v
        |                ProgramVersion + sealed Core/
        |                    native bindings
        |                             |
        |                             v
        |                QuickJS generator -> typed command
        |                             |
        |                             v
        |                 ProgramRuntime (durability/replay)
        |                             |
        +-----------------------------+
                       |
                       v
         GraphEngine / admitted host binding
                       |
                       v
      checkpoint / store / provider / tool / events
```

### Bounded JavaScript contexts

JavaScript authoring has two distinct execution contexts. `define()` runs only
in the compiler's bounded compile-time QuickJS context and emits the graph that
is normalized, sealed, and admitted. A generator `main(input)` does not run
during structural compilation: it runs in a bounded runtime QuickJS session
owned by one Program attempt. That session is retained only for the lifetime of
the attempt. Every yielded typed command is durably journaled before dispatch,
so recovery and recorded replay reconstruct behavior from the durable command
journal rather than retaining or trusting ambient JavaScript process state.

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

### Durable research evidence coordination

Large research Harnesses use a durable `EvidenceLedger`, not all-to-all chat, as
their authoritative coordination state. `SourceIdentity` keys one immutable
source version by canonical locator, version, and content hash; aliases resolve
to that identity without collapsing a later version. A task is either a primary
extraction or an explicitly marked independent review, reproduction, or
rebuttal. The ledger rejects accidental duplicate primary work but never
mistakes an intentional review for duplicate work.

`SqliteEvidenceLedger` serializes task claims, expiry, and publication with
SQLite transactions. A lease includes the task generation, worker, and
owner scope, so an expired or superseded worker cannot renew or publish.
Publication atomically stores a typed artifact and the task's terminal state;
retrying the same artifact is idempotent, while a different artifact is
rejected. Negative/no-support evidence records its exact searched scope and
remains evidence rather than a global source closure. Conflicting evidence
coexists and deterministically asks for reconciliation rather than
last-writer-wins.

All task, lease-expiry, claim-resolution, source-lifecycle, and evidence reads
are owner-scoped. The authenticated Program/A2A admission boundary supplies
that scope; callers cannot use a known task id to acquire, inspect, or publish
another owner's work. Source identity is a shared immutable registry only;
artifact visibility and task authority remain local to the owner.

The ledger is deliberately a coordination substrate, not a second scheduler or
node executor. Program admission, child lineage, owner/host resource ceilings,
capabilities, effects, and A2A authorization remain enforced by the existing
Program and host-admission contracts. A directed A2A message can ask for a
review or report a finding, but it changes allocation or acceptance only after
a typed artifact is durably committed.

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

A fetched A2A well-known card is discovery evidence, not admission.
`AgentCardCollector` makes one explicit card GET with no Authorization header
and never follows a redirect or invokes the advertised RPC endpoint.
`AgentCardCandidateCompiler` distils the collected card into an immutable,
digest-pinned, unadmitted candidate containing only bounded protocol facts and
safe skill identifiers. The candidate excludes the card's free-form text,
declared remote RPC endpoint, provider configuration, security schemes, and
credentials; it cannot dispatch the source agent. The sole Copy Ninja PoC
materializer additionally requires an independently observed profile pinned to
that digest and constructs a local graph. Any other behavior still requires the
ordinary local Program/admission path.

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

### Execution strategy: embedded JavaScript with a sealed command boundary

The bounded typed-plan dispatcher remains the current legacy implementation,
but it is no longer the target general authoring model. Standard JavaScript on
embedded QuickJS owns expressions, functions, closures, branches, loops, and
ordinary local computation.

- Admission compiles pinned JavaScript source and sealed modules with an exact
  QuickJS build and binds every Core/native import to an immutable slot.
- A generator yields typed commands; `ProgramRuntime` validates, budgets,
  journals, dispatches, and resumes those commands.
- QuickJS never reaches into `GraphEngine` readiness or checkpoint state and
  never commits an external effect directly.
- Recovery replays pinned source against exact recorded command outcomes.
  Completed effects are not redispatched; any command mismatch fails closed.
- Memory limits, execution interruption, cancellation, and nonrenewable budgets
  bound every admitted production run even though the language is general.
- QuickJS and the native binding layer remain optional Program dependencies.
  Core-only users pay no dependency, allocation, symbol, branch, or binary-size
  cost.

The authoritative language, sandbox, ABI, replay, and cutover contracts are in
[`QUICKJS_CONTROL_ARCHITECTURE.md`](QUICKJS_CONTROL_ARCHITECTURE.md) and
[`QUICKJS_CONTROL_MIGRATION.md`](QUICKJS_CONTROL_MIGRATION.md).

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
  general-purpose node-execution VM. JavaScript `define()` is compile-time graph
  authoring; an optional generator `main(input)` is bounded retained runtime
  control that yields only typed Program commands.
- Keep programs inspectable, source-mapped, reproducible, capability-scoped,
  and replayable.
- Permit open-ended evolution as a sequence of finite admitted versions, never
  as unchecked mutation of a running graph.
- Reach v1 with one documented C++ API, one error model, and deliberate binary
  compatibility boundaries.

## Non-goals

- Executing raw or unsealed model output, arbitrary C++, Python, shell, or
  ambient host APIs.
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

The current implementation uses a typed immutable operation graph whose nodes
retain source coordinates and directly reference compiled Core generations or
other Program nodes. It directly schedules `sequence`, `branch`, `return`,
bounded `loop`/`retry`, `parallel`, `race`, `cancel`, `await`, `emit`,
`checkpoint`, `map`, `quorum`, and bounded child operations.

That vocabulary is now a frozen legacy implementation and migration input. New
general computation constructs do not extend `ProgramOperationKind` or the
Program-v2/v3/v4 JSON schemas. JavaScript expresses ordinary control flow and
yields only NeoGraph domain commands. Legacy operations remain available only
for correctness fixes, stored-version drain, and equivalence/migration
evidence until the announced cutover removes them.

ProgramRuntime is therefore an intentional second **scheduling domain**, but not
a second node executor. It owns readiness and joins for Program operations,
child handles, and the Program-wide cancellation/budget tree. GraphEngine alone
owns `GraphNode` readiness, fan-out, retry, checkpoint, and teardown inside one
`call_core`. The boundary is one owned Core invocation plus one typed terminal
result/event stream. Contract tests must cover cancellation propagation in both
directions, destruction with losing race children, checkpoint ordering, budget
debits, and equivalence with direct Core execution. ProgramRuntime may not reach
inside GraphEngine's ready queue or checkpoint state.

The current source-level compatibility boundary is code, schemas, and tests.
The replacement authoring and removal sequence is defined only by
[`QUICKJS_CONTROL_MIGRATION.md`](QUICKJS_CONTROL_MIGRATION.md); the retired DSL
roadmaps are not future implementation authority.

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

### JavaScript authoring boundary

`SourceKind::JavaScript` is an opt-in authoring and bounded control frontend,
not a second executor for Core/application nodes. `ProgramCompiler` evaluates
the synchronous `define()` export in a private compile-time QuickJS context and
seals exactly one Core definition into the immutable `ProgramBundle`. If the
source also exports generator `main(input)`, `ProgramRuntime` opens a separate
bounded QuickJS session for that run attempt. The session owns ordinary
JavaScript control flow only; yielded typed Program commands remain subject to
admission, budgets, the durable command journal, dispatch, and replay.

The evaluated export set determines the output contract: define-only modules
retain the Core root's channels-wrapped result, while modules with
`main(input)` validate the generator's terminal return directly against the
Program/Harness result schema.

Both contexts expose a non-extensible, versioned `ng` surface and install no
ambient module loader, network, provider, tool, filesystem, shell, or
native-plugin capability. Wrong `define()` returns, top-level await, non-
generator `main(input)`, and malformed yielded commands fail closed. Compile
contexts cap memory, stack, wall time, output bytes, and interrupt polls.
Runtime sessions are retained only for their attempt and are bounded by the
admitted `RunBudget`; recovery replays durable command results instead of
retaining JavaScript state. The source envelope pins the QuickJS
engine/language/host-API versions. Builds without
`NEOGRAPH_BUILD_QUICKJS_CONTROL` reject JavaScript sources rather than linking
QuickJS into Core-only consumers.

### Host admission

`RunBudget` is a durable per-Program spending limit; it is not a statement
that the current machine can safely run every admitted Program at once.
`HostResourceProfile` is the separate host-owned, versioned capacity snapshot.
It records measured, estimated, or conservative-fallback evidence, subtracts a
non-admitted safety reserve, and never treats an unknown component as
unlimited.

`HostAdmissionController` atomically reserves a vector of CPU, memory, GPU,
process, thread, file-descriptor, disk, network, tool, provider, token, cost,
and wall-time components. It gives feasible queued work aging-priority/FIFO
order; changing a profile never revokes a held lease, but blocks new grants
while the profile is overcommitted.

Program host admission is opt-in as an all-or-nothing pair in `RuntimeConfig`:
a shared controller plus a request resolver. The resolver sees only immutable
attempt context and chooses resource quantities and scheduling hints. The
runtime stamps the owner and a unique per-attempt operation identity, clamps
the queue timeout to the Program deadline, and starts Core only after a lease
is granted. A user cancellation, runtime shutdown, or wall-time expiry removes
a queued request without dispatching Core. The lease is terminal cleanup: it is
released before a completed handle becomes observable, including failed,
cancelled, and timed-out attempts.

This is a host safety envelope, not another node engine, durable budget ledger,
or hidden cross-operation lock. Hosts may leave the pair unset to preserve
legacy direct Program dispatch, and may share one controller across Program
runtimes, Engine tool dispatch, and other local work.

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

### Tool execution admission

Every Engine and standalone `Agent` tool call enters the same
`ToolExecutionController`. A host can share one controller across engines so
resource limits remain process-wide instead of silently becoming per-engine
limits. An unclassified tool defaults to one keyed-exclusive resource,
`tool:{tool}`: calls to one tool serialize, while distinct tools may overlap.

`ToolExecutionPolicy` is a versioned host value. It selects the native
awaitable entry or the bounded blocking-thread bridge, one of `reentrant`,
`keyed-exclusive`, or finite `capacity` concurrency, a pending limit, queue
deadline, result-size limit, and a canonical resource-key template. Templates
accept only `{tool}`, `{owner}`, `{root}`, `{thread}`, and scalar
`{arg:name}` substitutions; missing, structured, or malformed components fail
closed rather than collapsing two resources into the same key.

Non-reentrant calls acquire an RAII `ResourceLease` from the shared FIFO
`ResourceArbiter` before invoking the tool. Queue cancellation and expiry
remove the waiter; scope exit returns a granted slot. The blocking bridge uses
one bounded worker pool and posts completion back to the caller executor, so a
legacy synchronous `Tool::execute` never pins an I/O or Core scheduler thread.
The lease spans only one tool invocation and is released before dispatch
returns. Composite spawn, handoff, and join operations must own their complete
lifecycle explicitly; the arbiter never hides a cross-operation lock.

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
and source coordinates. New sources use trusted C++ builder values or sealed
JavaScript. Canonical JSON remains a storage identity for already-retained
legacy artifacts, but is not a source constructor or a compilation frontend.
JavaScript source is sealed in a canonical envelope that pins its QuickJS
engine, language, and host-API versions. `define()` lowers through the
compile-time graph-builder boundary above; an optional generator `main(input)`
remains sealed as runtime control source and yields only durable typed Program
commands. YAML or model-specific syntax may be added later only by lowering to
the same typed source model.

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
- Remote descriptors, generated source, retrieved Harnesses, children, and
  candidates request authority but cannot grant it. Any stronger successor
  requires an explicit control-plane grant and new immutable admission.

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

The controller extension additionally correlates descriptor, behavioral
fingerprint, evolution proposal, candidate, evaluation, authority grant,
effective guarantee, promotion, and activation identities. These fields add
lineage; they do not replace the base run and effect coordinates.

## Post-cutover controller extension

After the one-language QuickJS cutover, NeoGraph may close an outer controller
loop around this architecture:

```text
observe capability and outcome evidence
  -> synthesize bounded immutable candidate Programs
  -> compile and admit through the same Program path
  -> evaluate in simulation, shadow, and authorized canary modes
  -> publish and compare-and-swap activate a passing successor
```

The extension supports explicit developer grants for filesystem, network,
process, environment, credential, provider/model, dynamic-child, native, and
unmanaged effects. Default programs retain no ambient authority. Effective
execution guarantees are labeled `strict`, `recorded`, or `unmanaged` and
degrade to the weakest reachable closure; broad authority never silently
inherits a strict replay claim.

OpenAPI, A2A, MCP, and JSON Schema descriptions enter as untrusted declarations.
Generated adapters and Harnesses use the same sealed JavaScript source,
capability/effect closure, budget, admission, journal, catalog, and activation
contracts as handwritten Programs.

Self-evolution creates new versions only. It never mutates a running Program,
live Core generation, journal history, effect record, budget ledger, or active
version in place. The complete protocol and falsifiable claim ladder are defined
in [`SELF_EVOLVING_AGENT_CONTROLLER.md`](SELF_EVOLVING_AGENT_CONTROLLER.md).

## Compatibility and cutover

Strict Core documents, retained artifacts, and MCP methods remain compatibility
surfaces, while source authoring has one public language: JavaScript.

- Existing strict Core JSON remains validated low-level interchange and
  canonical serialization, not a second programming language.
- The bounded Core topology elaborator, Harness `mode: "dsl"`, and their
  source-authoring tests/examples are deleted. New requests using that mode
  fail with an explicit migration diagnostic rather than selecting a fallback.
- New Core graph definitions use JavaScript `define()` evaluation or the
  trusted C++ embedding API; new Programs use JavaScript generator `main()`.
  Both retain the same validation, admission, activation, durability, effect,
  and GraphEngine boundaries.
- Program-v2/v3/v4 operation-tree authoring and Harness `mode: "program"` remain
  frozen migration surfaces under the separate Program drain plan.
- Existing MCP method names may remain as transport compatibility, but adapters
  do not own another compiler or runtime.
- The Core elaborator is deleted; legacy Program parser/dispatcher removal
  remains governed by the stored-version drain plan.
- Superseded DSL and Control-VM studies are removed from the live documentation;
  repository history remains the historical record.

## Acceptance gates

The architecture is implemented only when all are true:

1. A Core-only installed consumer builds and runs without Program enabled.
2. A developer can build and run the same static graph through Core directly
   and through bounded JavaScript `define()`, with canonical Core and observable
   behavior equivalence.
3. A developer can compile and run a JavaScript generator containing functions,
   closures, branches, loops, checkpoints, and child commands through pinned
   native and Core bindings.
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
13. QuickJS and every other dependency change has the separate decision and
    evidence required by the package-boundary policy.
14. Two independently owned Program runtimes can collaborate through the A2A
   adapter with explicit consent, owner isolation, task/artifact correlation,
   cancellation semantics, restart-safe duplicate handling, and no capability
   leakage.
15. NeoCode and NeoProtocol consumers declare a current NeoGraph Program
   contract revision and pass the legacy-integration rebase/conformance gate
   before claiming v1 compatibility.

## Rejected alternatives

### VM-owned node execution or durability

Rejected. QuickJS is accepted only as a Program control interpreter behind a
sealed yielded-command boundary. It does not replace `GraphEngine`, schedule
Core nodes, own journals/checkpoint stores, or commit effects. The earlier sole
Control VM plus separate Durable Kernel design duplicated scheduling,
checkpoint, and execution ownership; that design remains rejected.

### One enlarged GraphEngine API

Rejected. Adding model-authored source, module registries, activation, tenant
policy, and child Program lifecycle directly to `GraphEngine` would couple every
Core user to control-plane semantics and make the fast path harder to reason
about.

### Harness remains the architecture

Rejected. Harness is a useful application and transport service, but keeping
its compiler, retained artifacts, and lifecycle as MCP-only concepts prevents
normal C++ users from using the same capabilities and encourages duplicate APIs.

### Mutable live graphs or Programs

Rejected. In-place topology or Program mutation makes checkpoint, pending work,
retry, cancellation, effect, budget, lineage, and authority semantics
ambiguous. Immutable generations and Program versions plus explicit migration
or activation are safer and keep ordinary execution lock-free. Self-evolution
therefore proposes, evaluates, publishes, and activates a successor instead of
rewriting an active object.
