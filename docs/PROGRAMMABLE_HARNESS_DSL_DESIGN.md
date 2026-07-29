# Programmable Harness DSL and Control VM Architecture

Status: Target Architecture Design (not yet implemented; execution decision revised)
Date: 2026-07-29
Related Issues: #245, #250, #251, #252

## 1. Purpose

NeoGraph's core vision extends beyond models improvising from natural-language
instructions. The goal is for models to first write an inspectable Agent Program
that NeoGraph then compiles, validates, executes, and records.

This document answers the following questions:

1. What is the current expressive range of the Harness DSL and core topology JSON?
2. Why does the current Harness not accommodate the full NeoGraph topology?
3. How can runtime-determined behaviors such as `Send`, `Command.goto`, and
   dynamic interrupt be expressed safely in a declarative DSL?
4. How is the claim of expressing all permitted topologies defined and verified?
5. Which extension boundary lets developers add new topology concepts while
   retiring the legacy scheduler and keeping checkpoint and Harness invariants
   in one durable kernel?

This document is not user-facing documentation of currently supported features.
It records design criteria and completeness contracts for future implementation.

## 2. Conclusion

The Programmable Harness evolves into a **unified control-execution architecture
that combines one typed metered Control VM with one small durable kernel**.

- The existing strict core topology JSON is retained as the canonical
  analyzable representation of a static graph and as a backward-compatible
  authoring and inspection mode. It is not a permanent second execution backend.
- Every admitted program, including an existing strict-core graph, is lowered
  deterministically to verified typed control bytecode before execution.
- Every bundle carries a hashed semantic projection for validation,
  visualization, provenance, and source-to-bytecode equivalence. A canonical
  strict-core topology is included when the source has one.
- The Control VM is the only control executor. It decides what transition to
  propose but does not directly commit state, dispatch effects, or own recovery.
- The durable kernel is the only owner of task, state, event, effect, budget,
  journal, replay, and checkpoint invariants.
- The long-term canonical executable artifact is not a single strict core
  document but a content-addressed **Program Bundle** that bundles the static
  topology, bytecode module, imports, schemas, and source map.
- `GraphEngine` remains temporarily as the legacy behavior oracle and becomes a
  compatibility facade over Program compilation and execution after cutover.

```text
natural-language directive
  -> normalized directive contract
  -> high-level Program dialect
  -> validate and deterministic semantic lowering
       -> inspectable strict-core topology, when statically representable
       -> verified typed control bytecode module for all executable control
  -> sealed Program Bundle
  -> immutable admission receipt
  -> Control VM
       -> proposed TransitionBatch
  -> durable transition kernel
       -> task scheduling
       -> state commit/checkpoint
       -> effect/capability mediation
       -> journal/replay
```

The goal of this decision is not Turing completeness itself. The goal is to
provide a **stable extension boundary** so that when a developer introduces a
future concept such as `race`, `quorum`, speculative branch, structured
cancellation, or a topology notion not yet named, it can be expressed as a
program or library without repeatedly modifying the existing `Scheduler`,
`GraphEngine`, and checkpoint backends.

The unified Control VM does not eliminate all runtime changes. Features that
alter kernel invariants such as new global fairness, transaction isolation,
distributed commit, or event-time models require kernel and checkpoint protocol
changes in any architecture. The architecture's purpose is to absorb common new
concepts as bytecode libraries and compiler extensions, leaving only the cases
that genuinely require a kernel change.

### 2.1 Final Decision on Turing Completeness

"Turing-complete?" is not treated as a single boolean for the entire DSL.
Termination and expressiveness contracts are separated per layer.

| Layer | Decision | Rationale |
|---|---|---|
| directive normalization | total, bounded | Natural-language clause omissions and diagnostics must be closed before execution |
| authoring sugar/elaboration | total, bounded | The same source must always normalize to a unique Program Bundle |
| strict core document | finite, canonical graph component | Preserves static topology review and existing execution compatibility |
| typed control bytecode | idealized semantics is Turing-complete | Expresses new orchestration constructs without adding feature-specific legacy scheduler branches after the transition adapter |
| durable kernel | deliberately non-general-purpose | Owns only scheduling, persistence, effect, and authority invariants |
| admitted production run | finite by non-renewable fuel | Every actual run must have step/time/cost/concurrency/expansion budgets |
| primitive implementation | general computation possible, sealed contract required | Domain computation lives in typed primitives with fixed authority and effect closure |

Therefore the **Elaborator remains total and only the control VM is
Turing-complete**. The VM can express functions, branches, loops, recursion,
and typed local state. Its idealized semantics includes unbounded integers or
equivalent unbounded storage and unbounded allocation, sufficient to simulate a
two-counter machine. Under that idealized assumption of infinite memory and
fuel, it can express general computation in theory. A production VM does not
provide those unbounded resources and is finite-state under its admitted
limits. Every admitted run has
upper bounds on instruction fuel, memory, call depth, task count, wall time,
cost, and concurrency. The VM must yield to the kernel every bounded quantum,
so a tight loop cannot permanently block checkpoint, cancellation, or other
tasks.

Resume, retry, child attachment, replacement, and fork do not replenish fuel.
Budget exhaustion must be a stable terminal outcome distinguishable from
model failure or generic runtime error.

Turing completeness does not grant the following authorities:

- undeclared host function or capability import
- direct access to raw pointers, filesystem, sockets, clock, or random source
- dynamic capability or credential creation
- child authority broader than the parent
- unchecked in-place mutation of an active artifact
- execution of raw C++, Python, or shell source

Internal VM branches and function targets may compute freely within a verified
module. However, external tasks, effects, modules, and capabilities are
referenced only through a sealed import table or an opaque handle issued by
the kernel. When new authority is needed, a separate child or replacement
source is created, then compiled, bound, and admitted again.

### 2.2 Boundary Between Static Guarantees and Runtime Guarantees

When general loops and state are permitted, properties such as termination,
absence of livelock, and reaching a success terminal for all inputs are
generally not statically decidable. The guarantees provided by the compiler
and those enforced by the runtime monitor are separated.

| Guaranteed at compile/admission | Enforced or observed at runtime |
|---|---|
| Closed resolution of modules, imports, and capabilities | Actual task, event, and effect decisions |
| Schema, port, and reducer compatibility | Payload validation and channel evolution |
| Capability, effect, and authority upper bound | Per-call capability check and effect reconciliation |
| Budget existence and child attenuation | Run-wide fuel debit and exhaustion |
| Bytecode type, control-flow, and import verification | Instruction quantum, memory and call-depth enforcement |
| Terminal/output declaration and structural reachability | Actual terminal, non-progress, timeout, and cancellation |
| Deterministic lowering and complete key consumption | Command journal, checkpoint, and replay lineage |

A program that requires stronger guarantees may optionally use a more rigorous
proof profile, such as bounded loops, ranking functions, acyclic effect regions,
or deterministic/confluent reducer certificates. A compiler must not falsely
declare a general program terminating when it lacks such a certificate.

### 2.3 Relationship to Current Issue Contracts

The prohibition in #245 and #252 against Turing-complete code embedded in
Harness JSON must be narrowed to mean no arbitrary host-language source or
**unverified** executable payload. The statement in #252 that `DSL remains
bounded and non-Turing-complete` conflicts with the final decision in this
document. This document chooses the verified typed-bytecode architecture. The
#245 and #252 issue descriptions and acceptance criteria must be updated to the
following layered contract before implementation begins; until they are
updated, the contract conflict is an explicit implementation blocker:

> Authoring elaboration is total and bounded. A sealed Program Bundle may
> contain verified, typed, metered control bytecode with computationally
> universal idealized semantics, while every admitted run is bounded by
> non-renewable fuel and closed authority.

The host model executor in #250 and the adopted MCP capability in #251 are
orthogonal to this computation model. They supply sealed primitive and
capability bindings only; they do not extend the DSL's target closure or
authority.

### 2.4 Consensus from Independent Research Lanes and Cross-Examination

Three positions competed in the role-based independent review, cross-examination,
and legacy-coupling re-examination:

| Position | Strongest Argument | Final Conclusion |
|---|---|---|
| finite-control HIR | Only a total language keeps static analysis and durable migration stable | Adopted for normalization, elaboration, and per-step expressions |
| metered bytecode | Expressing all executable control through one instruction model prevents permanent scheduler forks | Adopted as the sole admitted control-execution substrate |
| evolved strict core | Existing topology carries valuable static analysis, visualization, and compatibility information | Retained as hashed semantic IR and graph-to-bytecode input, not as a permanent runtime |
| unrestricted VM-owned runtime | Handling control, state commit, effects, and recovery inside one general-purpose VM | Rejected because it weakens replay, migration, capability boundaries, and crash safety |

The points on which all lanes agreed were stronger:

- One durable kernel must own scheduling, commit, checkpoint, and effects.
- Lowering between source/HIR and the Program Bundle must be deterministic and
  total.
- Runtime data must not invent capabilities, imports, or authority.
- Every effect must go through typed mediation, durable identity, and
  reconciliation.
- Child and replacement operations are new artifact admission, not active graph
  mutation.
- Resume, fork, and repair must not increase budget or authority.

The final choice is **VM-unified control execution, not VM-owned durability**.
Every executable control path, including a static strict-core graph, eventually
runs as verified bytecode in the same Control VM. When a new concept can be
expressed with existing kernel transitions, only a bytecode compiler or library
is added. When a new concept changes state visibility, atomic commit, global
scheduling, durability, or security invariants, it requires a kernel release and
checkpoint migration. Not hiding this boundary is the key to long-term
compatibility.

This decision deliberately separates two meanings of "hybrid":

- **retained:** a Program Bundle may contain both inspectable topology IR and
  executable bytecode
- **rejected as the final state:** a graph scheduler and a Control VM remain as
  two independently evolving execution engines

The P3 adapter may temporarily run both implementations to prove observable
equivalence. It has an explicit removal gate and must not become a permanent
semantic compatibility profile.

### 2.5 Decision Matrix

| Criterion | Graph runtime only | VM owns control and durability | Unified Control VM + durable kernel |
|---|---|---|---|
| Add static graph notation | requires compiler/core schema work | usually a compiler/library change | topology and graph-to-bytecode lowering change |
| Add a new orchestration combinator | often touches scheduler and checkpoints | library change if host API is sufficient | bytecode library change if kernel transition is sufficient |
| Add global scheduling semantics | graph runtime change | VM host/runtime change | explicit kernel change |
| Static topology analysis | strongest | weakest unless rebuilt in verifier metadata | preserved before lowering and committed in the bundle |
| Deterministic replay | clear but specialized state grows | VM must recreate durable command history | one kernel journal for every control program |
| Checkpoint migration | schema grows per core feature | VM owns an unsafe amount of durable state | stable kernel state plus versioned VM continuation |
| Capability enforcement | direct in compiler/runtime | unsafe unless every environment access is imported | sealed imports admitted and enforced by the kernel |
| Source-level debugging | direct graph visualization | poor without disassembly/source maps | graph view plus bytecode disassembly and unified provenance |
| Legacy modification for ordinary future concepts | medium to high | low after a large initial rewrite | low after graph-to-bytecode cutover |
| Risk of semantic forks | concentrated in one graph runtime | concentrated in one oversized VM runtime | controlled by one VM instruction model and one kernel |
| Initial implementation cost | lowest | highest and riskiest | medium, staged behind existing behavior |

The graph-runtime-only option is preferable when the feature set is stable and
static analysis dominates extensibility. A VM that also owns persistence and
effects is preferable only for an isolated compute sandbox with few durability
requirements. NeoGraph requires durable graph compatibility and open-ended
orchestration extension without two control runtimes, so one Control VM above
one durable kernel best matches the stated objective.

## 3. Current Compilation Pipeline

The current Harness `dsl` mode is not a separate workflow grammar.

```text
harness.definition
  -> generic Elaborator
  -> strict core topology JSON
  -> GraphCompiler
  -> translation validation
  -> GraphValidator
  -> Harness binding validation
  -> retained artifact
```

The `Elaborator` provides the following compile-time sugar:

- acyclic `vars`
- non-recursive `templates`
- prefixed `use` instantiation
- boolean `when`
- template expansion source map

All sugar is removed before `GraphCompiler` runs. For a strict core document
that has no DSL keys, elaboration is the identity operation.

Evidence:

- `include/neograph/graph/elaborator.h:3-16`
- `include/neograph/graph/elaborator.h:65-89`
- `src/mcp/harness.cpp:1959-2003`

## 4. Current Expressiveness of Strict Core Topology

The static topology surface that `GraphCompiler` currently accepts is as follows:

| Feature | Core JSON representation | Additional runtime dependency |
|---|---:|---|
| graph name | yes | none |
| channels and initial value | yes | none |
| built-in reducer | yes | `overwrite`, `append` |
| custom reducer | name only | registered reducer implementation required |
| registered node type/config | yes | node factory and executable node implementation required |
| static edge | yes | none |
| static fan-out | yes | affected by executor worker count |
| cycle/back-edge | yes | runtime budget such as `max_steps` required |
| conditional route | yes | registered condition implementation required |
| barrier/AND-join | yes | barrier state maintained at runtime |
| static interrupt before/after | yes | checkpoint/resume runtime required |
| graph-wide retry policy | yes | exception classification is runtime semantics |
| inline subgraph | yes | child graph compile and runtime context required |
| external-file subgraph | path only | depends on filesystem access and mutable files |
| dynamic `Command.goto` | runtime decision cannot be declared directly | node returns a target among existing compiled nodes |
| dynamic `Send` fan-out | runtime multiplicity/payload cannot be declared directly | node returns invocations targeting existing compiled nodes |
| dynamic interrupt | runtime decision/payload cannot be declared directly | node raises the interrupt during execution |

Evidence:

- `src/core/graph_compiler.cpp:337-528`
- `src/core/graph_loader.cpp:183-306`
- `include/neograph/graph/types.h:208-260`
- `src/core/scheduler.cpp:28-176`

## 5. Core Limitations of the Current Harness

Strict core can express a broad topology, but the Harness binding restricts it
again.

Currently, only two node types are permitted in the Harness:

- `neograph_harness_worker`
- `neograph_harness_judge`

Furthermore, every request worker must be bound to exactly one topology node,
and there must be exactly one judge node. Any other node type is rejected with
`H_NODE_TYPE`.

Evidence: `src/mcp/harness.cpp:1429-1469`

This restriction was appropriate for the read-only fan-out/judge MVP provided
in #147. However, it is not suitable for the general topology of a Programmable
Agent.

### 5.1 Mismatch Between Schema and Actual Admission

`neograph_schema` returns the process-global `NodeFactory::export_schema()` as
`node_palette`. This can include `llm_call`, `tool_dispatch`,
`intent_classifier`, `subgraph`, and custom nodes.

However, the Harness binding rejects these nodes. This means the surface
advertised by the current schema differs from the actual Harness admission
surface.

Evidence:

- `src/mcp/harness.cpp:2421`
- `src/core/graph_loader.cpp:183-306`
- `src/mcp/harness.cpp:1444-1451`

### 5.2 Worker Dataflow Is Fixed

The current worker node is hardcoded to read `task` and write to
`worker_results`.

```text
task -> harness worker -> worker_results
```

The judge reads `worker_results` and writes to `final_result`.

```text
worker_results -> harness judge -> final_result
```

Evidence: `src/mcp/harness.cpp:1184-1213`

Therefore, the current structure does not naturally express the following:

- passing worker A's output as worker B's input
- reusing the same worker contract across multiple phases
- selecting different input/output channels per invocation
- writing worker output to different schemas or channels
- composing multiple judges or acceptance gates
- a topology without a judge
- exporting arbitrary channels as the final program output
- typed child/module input/output mapping

### 5.3 Permitting Runtime Primitives Directly Can Bypass Authority

Simply removing the `H_NODE_TYPE` check and allowing all `NodeFactory` types
is not safe. For example, `tool_dispatch` uses the `NodeContext` tool set and
does not automatically follow the Harness worker's tool allowlist and
capability policy.

Full topology support must be provided through a sealed primitive palette
admitted within the Harness, not through wholesale allowance of the
process-global palette.

### 5.4 Current Legacy Coupling of New Control Concepts

In the current architecture, new node computations are isolated in the registry,
but new scheduler semantics are not.

| Concept kind | Current change scope |
|---|---|
| domain node, condition, reducer | `GraphRegistry` registration and schema/effect contract |
| static topology field | types, compiler parse/link/to_json/canon, validator |
| ready-set or synchronization semantics | scheduler, engine loop, tests |
| scheduler state crossing a pause | checkpoint model, coordinator, all persistent backends |
| partial execution or cancellation semantics | executor, engine, checkpoint/replay, Harness lifecycle |

The current code and history demonstrate this:

- The scheduler directly defines command precedence, signal fan-in, and barrier
  accumulation: `include/neograph/graph/scheduler.h:144-229`.
- The checkpoint schema directly preserves `next_nodes`, `barrier_state`,
  pending write/command/send: `include/neograph/graph/checkpoint.h:29-158`.
- The barrier introduction commit `75d8345` changed the scheduler and engine,
  and the durable barrier commit `149f1ef` changed the checkpoint schema and
  restore path again.
- The signal dispatch commit `e5f34ff` changed ready-set semantics at the
  engine level.
- The dynamic `Send` routing fix `94f1515` modified both executor and engine,
  and `dbdaebe` extended the checkpoint from a single next node to the full
  `next_nodes` set for resume correctness.
- The partial failure recovery commit `d358719` shows the cost of pending
  execution state spreading across checkpoint and engine.

This history is not evidence that the current structure is wrong. Each feature
correctly extended the core semantics of its time. However, adding every future
topology combinator in the same fashion would cause the scheduler and
persistence layer to keep growing by special case. The VM and generic
transition protocol are the next extension boundary to reduce this recurring
cost.

## 6. Topology Completeness Definition

"Every NeoGraph topology" must not mean serializing arbitrary executable
callbacks into JSON.

It is defined as follows:

> Given an explicitly approved sealed registry `R`, every finite strict-core
> topology **document** that references only the node, reducer, and condition
> primitives from `R` can be expressed in the Harness DSL and, after
> elaboration, compiled to canonical core without semantic loss.

After the VM is introduced, control completeness is defined relative to the
kernel:

> Given a sealed kernel ABI `K` and an admitted module set `M`, every finite
> program source that can be described using the transitions of `K` and the
> pure/typed functions of `M` can be lowered deterministically to verified
> control bytecode, with a canonical topology projection wherever one exists.

No architecture can be unconditionally complete for all future global
semantics not yet defined. For example, a new transaction isolation level may
not be expressible exactly as a combination of existing transitions. In that
case, control completeness is not falsely claimed; a kernel ABI extension is
required.

Finiteness here refers to the artifact syntax and the set of nodes and edges,
not to the length of an execution trace. A finite graph containing a cycle can
run indefinitely without fuel.

Completeness is divided into three kinds:

| Completeness | Requirement |
|---|---|
| Topology completeness | Nodes, channels, edges, routes, barriers, cycles, interrupts, retries, and subgraphs are preserved in core without loss |
| Control completeness | Every executable source construct is expressed in typed bytecode, any static topology projection is bound to it, and required kernel transitions and imports are fixed |
| Behavior completeness | Primitive, bytecode, and kernel versions, implementation identity, schemas, effects, and control contract are fixed |
| Authority completeness | Capabilities, providers, workspace, credential scope, and budget are not extended after admission |

Compiler validity does not prove answer correctness. These four completeness
properties are guarantees about program structure and execution authority.

## 7. Serialization Boundaries

### 7.1 What Is Included in the DSL and Artifact

- Program Bundle schema version and manifest
- mandatory semantic projection and digest
- strict-core topology component and digest when present, otherwise an explicit
  canonical absence marker
- typed bytecode module, format version, and content digest
- bytecode import table and required kernel ABI
- VM state schema and supported migration coordinates
- primitive ID and version
- node config
- channel and reducer reference
- static edge and route
- barrier and interrupt declaration
- input/output schema and port mapping
- dynamic target allowlist
- dynamic fan-out upper bound
- effect and idempotency contract
- child/module interface
- timeout, concurrency, cost, and fuel envelope
- primitive, config, schema, control, and effect digest

### 7.2 What Is Included in the Runtime Trace and Journal

- actually executed module/version and instruction range
- VM yield, event wait/wakeup, and transition batch
- stable ID of task, subscription, and cancellation handle
- actually selected route target
- actual number of `Send` invocations and payloads
- actual interrupt request and resume value
- actual child artifact ID
- actual budget and fuel consumed
- primitive and control contract digest at execution time
- state or state hash used for dynamic decisions
- per-instruction, per-memory, per-call-depth, per-task, and per-effect fuel
  debit

### 7.3 What Is Not Serialized

- arbitrary C++/Python callback and lambda
- `GraphNode`, `Provider`, `Tool`, `Store`, and checkpoint objects
- API key, OAuth token, bearer header
- socket, file descriptor, thread pool
- live cancellation/stream callback
- arbitrary shell/script source
- raw plugin pointer or dynamic-library handle
- JIT compiler pointer, native code address, or process-local VM handle

Runtime objects are supplied by the scoped binding layer. Credentials do not
appear in the artifact or DSL.

### 7.4 Mandatory Trace Data Policy

Durable replay and privacy are separate contracts. A Program Bundle or run
profile must declare a persistence mode before execution:

| Mode | Persisted payload | Replay capability |
|---|---|---|
| `METADATA_ONLY` | event kind, identity, hash, timing, budget debit | control audit only; no payload replay |
| `REDACTED` | policy-filtered payload plus hashes and schema metadata | replay only where redacted values are not semantically required |
| `FULL` | exact approved event/effect payload | recorded replay permitted subject to key and retention policy |

The policy must classify input, VM local state, channel values, model output,
tool arguments/results, effect records, and source-map metadata independently.
Redaction occurs before persistence. Known credentials, bearer headers, auth
cookies, credential references, and configured secret patterns are never
eligible for `FULL` storage.

Persistent payloads require encryption at rest, tenant-scoped key ownership,
authenticated access control, retention deadlines, deletion/tombstone
propagation, and audit logs. A run that cannot retain a value required for exact
replay must record that limitation rather than claiming replayability. Hashes
support integrity and comparison but cannot reconstruct discarded content.

## 8. Program Bundle and Durable Kernel

### 8.1 Canonical Program Bundle

Instead of trying to express every future control semantics in strict-core JSON
alone, an immutable bundle with the following logical shape is used as the
executable identity:

```json
{
  "bundle_version": 1,
  "manifest": {
    "program_id": "org.example.implement_and_verify",
    "kernel_abi": "neograph.transition.v1",
    "source_semantics": "program-v1",
    "entry": "controller.main"
  },
  "semantic_projection": {
    "format": "neograph-control-graph-v1",
    "entry": "controller.main",
    "nodes": [],
    "edges": [],
    "imports": []
  },
  "topology": {"schema_version": 1, "channels": {}, "nodes": {}, "edges": []},
  "control_modules": [
    {
      "id": "controller",
      "format": "neograph-control-bytecode-v1",
      "digest": "sha256:...",
      "state_schema": "ControllerState@1",
      "imports": ["task.spawn", "event.next", "effect.request"],
      "code": "content-addressed blob or inline canonical bytes"
    }
  ],
  "schemas": {},
  "source_map": {},
  "dependency_root": "sha256:..."
}
```

`semantic_projection`, at least one admitted `control_module`, and an executable
bytecode entry are required in the final profile. The projection is a canonical,
conservative control graph that records entry points, possible control edges,
sealed target sets, imports, and source coordinates; it must not claim that a
dynamic edge is impossible unless the verifier can prove that claim. `topology`
is additionally required when the source has a strict-core representation.

An existing strict-core document must be mechanically compilable as a version-1
bundle containing its canonical topology, semantic projection, and
compiler-generated control bytecode. A topology-only P3 envelope is marked
`migration_only: true`, has no executable bundle identity, and can run only in
the frozen legacy oracle before cutover. It is not a final executable profile.
The final bundle hash includes the manifest, semantic projection, topology,
bytecode, imports, schemas, source map, and dependency root so inspected claims
and executed behavior cannot drift independently.

### 8.2 One Durable Transition Kernel

The target architecture does not implement either a separate graph execution
runtime or a separate VM state commit, checkpoint, effect, and replay path.
After graph-to-bytecode cutover, every admitted program executes in the Control
VM and uses the following abstract transition protocol:

| Kernel transition | Meaning |
|---|---|
| `state.read` | acquire a versioned read-only state view |
| `state.write` | propose a pending write with reducer/mode |
| `task.spawn` | create a durable task with a sealed target and typed input |
| `event.subscribe` | create a durable subscription for task, timer, input, or message events |
| `event.next` | consume the next matching event in journal order |
| `task.cancel` | record a cancellation intent for a task or structured scope |
| `timer.schedule` | schedule a logical deadline event |
| `effect.request` | request an external effect with capability checking |
| `checkpoint.yield` | submit the VM continuation and pending transitions to a durable boundary |
| `complete/fail` | terminate the invocation with a typed output or failure |

One VM quantum proposes a `TransitionBatch`. For a strict-core source, the
quantum executes compiler-generated bytecode whose source map points back to the
topology node, edge, route, barrier, or reducer that produced it. The kernel
validates the entire batch for schema, authority, budget, conflict, and
idempotency, then appends the journal and commits the state. It must not execute
some transitions first and reject the remainder.

```text
event batch + checkpointed continuation
  -> deterministic Control VM
  -> proposed TransitionBatch
  -> validate authority/schema/budget/conflict
  -> append durable decision
  -> commit state, event cursors, and durable outbox records
  -> dispatch committed tasks/effects
  -> next checkpoint
```

The kernel alone owns the following invariants:

- stable task, effect, and event identity
- event ordering and atomic state visibility
- checkpoint, fork, and replay compatibility
- cancellation ownership and propagation
- capability and effect mediation
- run-wide budget accounting
- tenant and resource isolation

`GraphEngine` does not remain a second owner of these invariants. During
migration it serves as a behavior oracle for equivalence fixtures. After
cutover, its public construction and execution APIs delegate to Program Bundle
compilation and the Control VM runtime. New scheduling, checkpoint, replay,
cancellation, or effect semantics must not be added only to the legacy engine.

The compatibility adapter has the following mandatory exit criteria:

1. admission and lowering use the same sealed registry manifest, and admission
   succeeds only when every referenced node, reducer, condition, dynamic-control
   contract, and checkpoint shape has a verified lowering
2. a machine-readable compatibility matrix classifies every legacy behavior as
   `equivalent`, `versioned_change`, or `drain_only`; no behavior is unclassified
3. property tests over the sealed registry plus the strict-core corpus prove
   GraphEngine/VM equivalence for every `equivalent` class, including outputs,
   transition order, interrupts, retry, checkpoints, and causal journal records
4. every `versioned_change` requires an explicit source semantic profile and
   migration diagnostic; no compiler silently changes observable behavior
5. restart, resume, replay, and fork pass with kernel state plus a versioned VM
   continuation for all VM-admitted classes
6. existing legacy checkpoints are exactly converted or drained under the
   resume-only policy below, and no resumable legacy checkpoint remains before
   the legacy scheduler binary is removed
7. Harness routes new runs through the VM profile by default; public
   `GraphEngine` APIs compile to that profile and cannot start a legacy runtime

### 8.3 Normative Transition and Concurrency Semantics

The transition protocol must define behavior, not merely data shapes.

| Concern | Kernel-owned rule |
|---|---|
| state snapshot | one invocation reads a versioned snapshot fixed at the start of its event batch |
| writes | writes remain pending until batch validation; read versions are revalidated before commit |
| reducer order | conflicting writes are ordered by recorded logical task ID and transition index, never wall-clock completion order |
| route commands | zero or one distinct external route target is permitted per batch; multiple distinct targets reject the entire batch before commit |
| spawned task visibility | a VM-spawned task receives typed input plus an isolated state snapshot and returns a typed result event; it does not mutate parent state directly |
| event delivery | `event.next` advances a durable per-subscription cursor in the same commit as the consuming continuation |
| crash before commit | the same event may be presented again with the same event ID; no cursor advance is visible |
| cancellation race | journal order is authoritative; an already committed completion is not undone by a later cancellation intent |
| retry | every attempt has a deterministic `(logical_task_id, attempt)` identity and preserves the logical effect idempotency lineage |
| completion/failure race | the first kernel-committed terminal transition wins; later conflicting terminal events are retained as diagnostics |

The current graph runtime is not yet uniform with these target rules. In
particular, current single-`Send` and multi-`Send` execution have different
state-isolation paths, and current barrier and pending-write state are embedded
in scheduler/checkpoint structures. The P3 compatibility specification must
map every current rule, including `Command.goto` precedence, signal fan-in,
barriers, `Send`, pending-write replay, interrupts, and retry, to the transition
protocol before equivalence is claimed.

The final `program-v1` source semantics use the uniform rules in the table. A
separate `graph-compat-v1` **lowering profile**, executed by the same VM and
kernel, may preserve an existing strict-core behavior when that behavior can be
expressed without weakening kernel invariants. For example, current
`Command.goto` routing-order precedence is resolved inside generated compatibility
bytecode so the kernel still receives at most one external route target. A
single- or multi-`Send` shape is admitted for VM execution only after the
compatibility matrix proves an exact lowering for its state-visibility contract;
otherwise it is `drain_only` and cannot start as a new VM run.

During migration, `precutover-graph-engine-v1` explicitly names the current
new-run Harness path through `GraphEngine`. It is migration-only, does not claim
VM equivalence, and must stop admitting new runs before default VM cutover.

The pinned `legacy-graph-v1-runtime` profile names the old GraphEngine execution
path, not the compatibility lowering profile. It is frozen except for correctness
fixes required to drain existing runs, receives no new orchestration features,
and cannot start new runs after VM cutover. `graph-compat-v1` may continue to
compile old source semantics into new VM bytecode without retaining the old
runtime. These distinct names prevent backward-compatible source semantics from
becoming an excuse for a permanent second engine.

#### 8.3.1 Resolved VM Integration Decisions

The first VM integration phase uses the following versioned decisions. Changing
one of them requires a new Kernel ABI or source-semantics profile; an
implementation detail must not silently redefine it.

| Decision | Version-1 rule |
|---|---|
| snapshot and read-your-writes | A quantum receives one immutable versioned snapshot. Ordinary `state.read` observes only that snapshot. Pending writes may be observed only through an explicit proposal-local preview, unavailable to other quanta and never described as committed state. `graph-compat-v1` uses this preview for legacy post-write routing. |
| reducer preview and commit | Preview calls a sealed, deterministic `reducer.preview-v1` Kernel service using the recorded logical task ID and transition index order. The reducer implementation/schema digest is pinned. Commit re-runs or verifies the same reducer computation against the current read versions; a mismatch or stale version rejects/conflicts the whole batch. Verified pure VM reducers may be added later only behind the same identity and translation-validation contract. |
| conflict fuel | Instructions, allocation, and validation work already consumed remain debited after an optimistic-concurrency conflict. Unmaterialized task/effect capacity reservations are released, but the retry receives only the remaining fuel lease. The same logical event IDs and source continuation retry against an explicitly newer snapshot under pinned conflict-count and non-progress bounds. No cursor advances and no retry replenishes fuel. |
| task/effect boundary | An existing `GraphNode` invocation is a durable task target. A pure node has an empty effect closure. A node that performs external I/O is VM-admissible only when every nested external operation is mediated by a declared `effect.request` adapter with stable identity and reconciliation. Coarse task retry identity never makes undeclared nested effects safe; such nodes are `versioned_change`, `drain_only`, or blocked. |
| stream events | Control-relevant events, task/effect results, interrupts, terminal outcomes, and any value later consumed by bytecode are durable journal events. Token/progress callbacks are best-effort by default and are not replay promises. A profile may opt into durable chunks only with stable event IDs, declared payload policy, and journal commit; recorded replay then emits the recorded chunks and performs no live callback-producing work. Live callback objects never enter a bundle or continuation. |
| public `GraphEngine` surface | Every public construction, execution, administration, and mutable-policy entry point is inventoried before cutover. Declarative construction and execution remain blocked from an `equivalent` claim until oracle fixtures pass. Live `CompiledGraph` injection and behavior-changing post-admission setters require descriptor-backed lowering or an explicit `versioned_change`; Kernel-owned administration uses explicit transactions and active-run race policy. |

The machine-readable forms of these decisions are
`spec/programmable-harness-kernel-abi-v1.schema.json`,
`spec/programmable-harness-compatibility-matrix-v1.schema.json`, and
`spec/programmable-harness-graph-engine-inventory-v1.json`. The schemas are
admission contracts, not evidence that the VM or Durable Kernel already exists.

### 8.4 External Effect State Machine

Atomic `TransitionBatch` commit does not imply exactly-once external effects.
The kernel atomically commits state changes and an outbox record, then dispatches
the effect outside the local transaction.

```text
proposed
  -> outbox_committed
  -> leased_for_dispatch
  -> dispatched
       -> completed(result)
       -> failed(definitive)
       -> ambiguous
```

Every effect carries a stable effect ID, idempotency key, capability identity,
attempt identity, request digest, and result schema. Dispatch is at-least-once.
Exactly-once external behavior is claimed only when the backend provides a
matching transactional or idempotent contract.

A crash after remote execution but before result persistence produces an
`ambiguous` effect unless the backend can answer a status query or safely accept
the same idempotency key. An ambiguous non-idempotent effect is never
automatically redispatched. It blocks dependent progress until authoritative
reconciliation records `completed`, `failed`, or `unknown`, matching the
existing Harness ambiguity contract.

Task dispatch follows the same durable outbox and lease discipline, but pure
or idempotent tasks may be retried according to their declared contract.
Result acceptance is deduplicated by task/effect identity and schema before it
becomes an event visible to the VM.

### 8.5 Atomic Budget Reservation

Checking a remaining budget without reserving it is unsafe under parallel child
admission. The kernel atomically reserves instruction fuel, cost, concurrency,
task count, memory, and expansion depth before admitting a task, child, effect,
or replacement.

| Outcome | Reservation treatment |
|---|---|
| validation rejection before admission | release the full reservation |
| admitted computation | debit actual use and release only explicitly refundable remainder |
| cancellation | charge committed work/effects; release unstarted capacity |
| failed dispatch with proof of no external action | apply declared retry/refund policy |
| ambiguous effect | retain the reservation until reconciliation |
| fork/replacement | transfer, never duplicate, the remaining reservation |

Reservation and settlement decisions are journaled. Parent and child checks use
the same serialized reservation transaction, so concurrent children cannot each
observe and spend the same remaining envelope.

### 8.6 Change Boundaries by Extension Kind

| New concept | Implementation location | Existing kernel change |
|---|---:|---:|
| syntax sugar, template, fixed subgraph | Program dialect lowering | none |
| new worker/domain computation | registered primitive or bytecode function | none |
| retry, race, quorum, speculative search | bytecode library using task/event/cancel | none if required hostcall exists |
| actor-like loop, mailbox protocol | bytecode + durable event source | none if mailbox event source exists |
| new external API | sealed capability adapter | no scheduler change |
| global priority/fairness | kernel scheduler | required |
| transaction isolation/commit model | kernel state/effect protocol | required |
| event-time watermark/window ordering | kernel event/time model | required |
| tenant authority or sandbox model | kernel security boundary | required |

The purpose of this table is not to hide everything behind a plugin. If a
global invariant change is disguised as an extension, different modules would
acquire different scheduler, consistency, and replay semantics, creating a
semantic fork. Such changes are handled by an explicit kernel ABI and
checkpoint schema release.

## 9. Typed Metered Control VM

### 9.1 Role and Non-Goals

The control VM is not an arbitrary application sandbox but a portable control
machine for durable orchestration. It exists to express new topology concepts
as functions and libraries, and to make their execution inspectable, metered,
checkpointable, and replayable.

The following are non-goals:

- replacing the native C++/Python ABI
- executing arbitrary filesystem or network processes
- performing LLM inference or large-scale numerical computation
- directly accessing the kernel scheduler, checkpoint store, or capability
  policy
- using a JIT compiler as the trust basis for admission or replay

Heavy domain computation and external I/O are separated into primitives, tasks,
and effects. The VM handles only control decisions and small deterministic
transformations.

### 9.2 Instruction Model

The exact stack, register, or SSA encoding is determined after prototyping.
The public contract first fixes semantic instruction classes rather than the
encoding.

| Instruction class | Minimum functionality |
|---|---|
| typed values | null, bool, integer, string, bytes, array, object, opaque handle |
| local state | local load/store, immutable argument, typed return |
| pure compute | construct/project, compare, boolean, bounded numeric/string operation |
| control flow | block, branch, switch, call, return, loop, recursion |
| durable call | sealed kernel import invocation |
| exception | typed throw/catch or explicit error result |
| yield | submit continuation to a checkpointable boundary |

The VM bytecode format must include:

- namespaced opcode and format version
- canonical binary encoding
- exact module content hash
- typed function signature
- declared maximum initial memory and stack/call-depth request
- sealed import table
- source location map
- verifier profile version

An unknown executable opcode must not be silently ignored like an annotation.
Without an exact implementation or a registered dialect definition, both
compile and resume fail closed.

### 9.3 Durable Hostcall

The VM does not see ambient APIs. Every hostcall is declared in the bundle
import table and resolved at admission to an exact kernel or extension identity.

```text
import task.spawn(target_ref, input) -> TaskHandle
import event.subscribe(handles, event_kinds) -> SubscriptionHandle
import event.next(subscription) -> Event
import task.cancel(scope_or_handle) -> CancelResult
import state.read(channel_ref) -> VersionedValue
import state.write(channel_ref, value, mode) -> PendingWrite
import effect.request(capability_ref, operation, payload) -> EffectHandle
import timer.schedule(logical_deadline) -> TimerHandle
import checkpoint.yield() -> ResumeEvent
```

Hostcall names, targets, and capabilities are not computed from arbitrary
strings. The compiler binds a module-local import index to a sealed reference.
Only opaque handles issued by the kernel are passed as runtime values. Handles
are scoped to a tenant, run, module, and generation, and cannot be moved to
another execution.

### 9.4 Metering and Scheduling

Every instruction and hostcall follows a deterministic fuel schedule. The fuel
schedule version is included in the admission receipt.

- pure instructions consume a fixed base fuel
- allocation consumes memory fuel proportional to the requested bytes or
  elements
- creation of tasks, effects, timers, and subscriptions consumes separate
  resource fuel
- at the end of a bounded quantum, the VM is forced to yield
- cancel, timeout, and process shutdown are observed not only at yield points
  but also at interpreter safe points
- retry, resume, fork, and module replacement inherit the remaining fuel
- a fuel schedule change can alter the replay result of the same module, so
  the profile and version are pinned

Fuel is not a termination proof. It is a runtime limit that forces a finite
production execution. Deployments that require static termination may request
loop bounds or ranking certificates through a separate proof profile.

### 9.5 Determinism, Effects, and Replay

The VM does not directly read the wall clock, OS random, environment, thread
scheduling, filesystem, or network. Time, random seed, model output, external
input, and effect results arrive as kernel events and effects, replayed in the
stable order of the journal.

During replay, the VM must produce the same `TransitionBatch` from the same
checkpoint and event prefix. If the produced batch differs from the recorded
batch, execution stops with `NONDETERMINISTIC_CONTROL`. External effects are
not re-executed during replay; the recorded result is consumed.

This guarantee does not imply determinism of the model answer. In a live run,
the LLM output is journaled as an effect result, and during a recorded replay
that result is reused.

### 9.6 Checkpoint and Version Migration

The checkpoint preserves the following VM continuation in addition to the graph
state:

- bundle, module, bytecode format, and verifier profile digest
- function, block, or program counter
- typed call stack, local and value stack
- bounded linear memory or canonical heap snapshot
- outstanding task, event, timer, and effect handles
- pending transition batch
- remaining fuel and logical clock
- extension-local state schema version

A resumable checkpoint is compatible only when the following tuple matches or
has an approved migration:

```text
bundle/module hashes
topology and channel/reducer schemas
kernel ABI and transition/event journal schemas
bytecode format, verifier profile, and fuel schedule
task/effect/handle/subscription schemas and in-flight states
registry and capability manifest digests
policy snapshot and authority envelope
VM continuation state schema
```

A running execution is pinned to the exact module content hash. A new module
must not interpret a past checkpoint based on the same name or a semver range.
Upgrade is permitted in only two ways:

1. The existing execution runs the old module bundle to completion.
2. A pure deterministic migration transforms the old VM state to the new state
   schema, then creates an immutable fork after compatibility validation.

A migration function must not call effects and must have a golden fixture for
every supported predecessor schema. Without an exact old module or an approved
migration, resume fails closed.

Before the sandboxed native-extension boundary exists, migration runs only as a
verified function in the built-in typed control VM with an empty hostcall import
table. The migration input and output use canonical deterministic encoding; the
output is validated against the target state schema and compatibility tuple.
Migration has dedicated instruction, memory, allocation, call-depth, and output
limits. Timeout, fuel exhaustion, malformed output, nondeterminism, or schema
mismatch fails closed without modifying the source checkpoint. Native migration
callbacks are prohibited until the P9 sandbox contract is implemented.

Pending and ambiguous effects, leased tasks, unconsumed events, and active
subscriptions are part of compatibility, not opaque VM-local details. A
migration must preserve their stable identities and cursors. An in-flight
non-idempotent effect that cannot be mapped exactly is non-migratable and keeps
the source run blocked for reconciliation.

Legacy GraphEngine checkpoints require a separate cutover policy because they
do not contain a VM continuation. Each checkpoint backend must inventory and
classify every resumable record before VM cutover:

- `convertible`: a pure, backend-tested converter maps scheduler state,
  `next_nodes`, barriers, pending writes, interrupts, retries, and in-flight
  identities to a sealed Program Bundle plus kernel state and VM continuation
- `drain_only`: the frozen `legacy-graph-v1-runtime` may resume the exact old run
  to completion, but it may not start, fork, replace, or attach new work
- `blocked`: ambiguous effects or an unsupported checkpoint shape fail closed
  and require operator reconciliation; they are never silently restarted

New runs stop using the legacy runtime at the default VM cutover. The
resume-only runtime remains isolated and version-pinned until every retained
legacy checkpoint is converted, completed, expired under the documented
retention policy, or explicitly tombstoned after operator approval. The legacy
scheduler binary is removed only after a store-wide audit reports zero resumable
legacy checkpoints. The public `GraphEngine` facade never routes new work to the
resume-only runtime.

### 9.7 Example of a New Topology Concept

A quorum join becomes a library function rather than a kernel opcode:

```text
fn quorum(target, inputs, required):
  handles = map(inputs, input -> task.spawn(target, input))
  subscription = event.subscribe(handles, [completed, failed])
  successes = []

  while len(successes) < required:
    event = event.next(subscription)
    if event.kind == completed:
      successes.append(event.value)
    else if remaining_possible(handles, event) < required:
      fail QuorumImpossible

  for handle in unfinished(handles):
    task.cancel(handle)

  return successes
```

With the same primitives, `race` selects the first completion, `await_all`
waits for all completions, and a speculative branch commits the winner and
cancels the loser. After the transition adapter exists, the core value of the
VM is that these semantic libraries do not add concept-specific branches to the
legacy C++ scheduler.

### 9.8 Declarative Surface and Lowering

Dynamic results are not pre-recorded in the source DSL. The program that
produces the dynamic results, along with its permitted imports and budget, is
recorded in the Program Bundle.

```text
Program Bundle = possible computation + sealed imports + budget
Runtime trace = events, transitions, and effects realized in this run
```

The high-level DSL can provide the following constructs:

- channel and JSON Pointer reads
- constant and object/array construction
- comparison and boolean operations
- enum switch
- array map, filter, and fold
- functions and typed local bindings
- loops and recursion
- task spawn and event wait
- cancellation scopes
- schema-validated input projection

Features not directly permitted in the DSL source:

- undeclared file, network, or process access
- raw host-language or native code
- runtime capability or import name generation
- direct checkpoint store access
- ambient credential or tenant context access

Simple constructs produce both strict-core topology and compiler-generated
bytecode; general control constructs produce bytecode plus conservative topology
metadata where useful. A conformance test verifies that generated bytecode
preserves every semantic claim made by its topology projection.

### 9.9 Closed Switch

```yaml
flow:
  - switch:
      value:
        channel: review
        pointer: /status
      cases:
        approved: merge
        changes_requested: repair
      default: fail
```

This structure can be lowered to named closed conditions and `conditional_edges`.
It is easier to statically validate and check for route completeness than
`Command.goto`.

### 9.10 Foreach/Dispatch

```yaml
flow:
  - run: discover

  - foreach:
      items:
        channel: discovery_result
        pointer: /items
      target: investigate
      max_items: 16
      input:
        item: {$item: true}
        objective:
          channel: task
          pointer: /objective
      collect:
        channel: findings
```

With a static bound and target, this is represented as a strict-core control node
and then lowered to control bytecode. When a runtime-dependent loop body, early
break, cancellation, or nested spawn is required, it is lowered directly to
control bytecode.

```text
foreach DSL
  -> neograph_control_map core node
  -> graph-to-bytecode lowering
  -> task.spawn/event.next transitions
```

The target is fixed as a sealed import. The payload, invocation count, event
order, and cancellation decision vary at runtime.

### 9.11 Typed Await

```yaml
flow:
  - await:
      id: merge_approval
      interaction: approval
      request_schema: MergeApprovalRequest
      resume_schema: MergeApprovalResult
      timeout: 24h
      on_approved: merge
      on_rejected: stop
```

This structure is lowered to the Harness's durable `INPUT_REQUIRED` and a
checkpoint/resume boundary. It is distinguished from an opaque static interrupt
and explicitly declares the waiting reason and resume schema.

### 9.12 Verifier Contract

Metering alone does not make bytecode safe. Before admission, the verifier
checks at least the following:

- type correctness of bytecode and function signatures
- valid terminator and branch target for every basic block
- absence of use-before-definition, invalid stack/local access, and malformed
  exception regions
- declared memory, call-depth, and initial resource request upper bounds
- import index exists within the sealed manifest
- hostcall argument and result schema compatibility
- non-forgeability of opaque handle type and scope
- effect and capability closure is a subset of the parent authority
- safe-point instrumentation on paths that can exceed the permitted quantum
  without yielding
- fail-closed handling of unknown opcode, format, verifier profile, or kernel
  ABI

The verifier does not claim to prove termination or semantic correctness of a
general program. It proves structural safety and authority closure; execution
volume is bounded by fuel and the runtime monitor.

### 9.13 Extension Packaging and ABI

Most new topology concepts are shipped as source compiler and bytecode
libraries, without requiring a native plugin ABI. When a native extension is
needed, public C++ virtual classes or STL objects are not used as a long-term
plugin ABI, because vtable, exception, allocator, and ownership ABIs can break
across compiler and runtime versions.

Long-term extension boundary candidates are a versioned C ABI, local RPC, or a
sandboxed Wasm component. The following logical contract must be provided
regardless of the specific transport:

```text
extension.manifest_v1
extension.validate_v1(source_or_op) -> diagnostics
extension.lower_v1(source_or_op) -> topology projection + typed bytecode
extension.migrate_state_v1(old_state) -> new_state
extension.explain_v1(runtime_event) -> source provenance
```

A content digest proves byte identity, not publisher trust. Before any bundle
or extension can be admitted, the deployment must verify a configured trust
root, publisher identity and scope authorization, signature or attestation,
revocation/quarantine status, and policy compatibility. Key rotation must not
silently re-authorize an old unsigned artifact. Offline deployments require a
pinned trust snapshot and explicit freshness policy.

Native extensions receive host-process authority and therefore require a
stricter allowlist or process sandbox than bytecode modules. An extension that
is revoked while a run is active prevents new calls. Continuing or migrating
an in-flight call depends on the effect ambiguity and compatibility rules; it
must not silently load replacement code.

A running bundle pins the exact content hash and host ABI, not an extension
name or semver range. The registry must be an engine-scoped immutable snapshot
and must not permit mutation after admission.

## 10. Contracted Dynamic Primitive

Domain computation that is unsuitable for the control VM
remains as a registered primitive.

The DSL carries references and contracts instead of implementation code.

Primitives handle LLM calls, database access, filesystem operations, large
numerical computation, or host integration that is unsuitable for bytecode
processing. Primitives execute behind a task or effect boundary and do not
directly manipulate the control VM or checkpoint store.

```yaml
nodes:
  classify:
    primitive: org.example.risk_classifier@2.1.0
    config:
      threshold: 0.8
```

Example primitive manifest:

```yaml
primitive: org.example.risk_classifier
version: 2.1.0
implementation_digest: sha256:...
config_schema: RiskClassifierConfig
reads: [candidate]
writes: [risk]
control:
  goto_targets: [accept, reject]
  send_targets: []
effects: []
deterministic: true
```

### 10.1 `Command.goto` Contract

A primitive that emits `Command.goto` must declare the permitted targets.

```yaml
control_contract:
  goto_targets: [repair, escalate, done]
```

If the runtime result falls outside this set, it fails before dispatch.

When multiple parallel branches emit `Command.goto` simultaneously, the current
scheduler uses last-writer-wins based on the supplied routing order. In a
`program-v1` profile, multiple identical targets are coalesced and multiple
distinct targets reject the entire transition batch before state commit. The
same-VM `graph-compat-v1` lowering preserves legacy routing-order precedence in
generated bytecode and proposes only the selected target to the kernel. The
old behavior remains in `legacy-graph-v1-runtime` only for resume-only checkpoint
drain and is included in P3 oracle traces.

Evidence: `src/core/scheduler.cpp:114-129`

### 10.2 `Send` Contract

```yaml
control_contract:
  sends:
    targets: [investigate]
    max_count: 16
    input_schema: InvestigationInput
    effects: [data_read]
```

Before dispatching the entire batch, the runtime validates:

- whether the target is in the allowlist
- whether the send count is at or below the upper bound
- whether each payload satisfies the input schema
- whether the remaining concurrency and fuel budgets are sufficient
- whether the effects and capability authority are within the permitted range

No individual send is executed before the entire batch is validated.

## 11. Runtime Topology Synthesis

Changes in dynamic target count or payload can be handled by `Send`. However,
when a new node kind or topology structure itself is required, the active graph
must not be modified.

```text
runtime evidence
  -> child DSL proposal
  -> Program Bundle compile and bytecode verification
  -> registry/capability/policy binding
  -> budget/authority validation
  -> immutable child artifact
  -> child_call or checkpoint-compatible fork
```

Example:

```yaml
flow:
  - synthesize_child:
      proposal_from: diagnosis.child_program
      interface:
        input: PlatformFailure
        output: VerifiedFix
      limits:
        max_nodes: 24
        max_depth: 2
        max_runtime: 20m
      authority:
        subset_of: parent
```

A child program cannot execute any node or capability before compilation and
admission are complete. The child authority and budget must not exceed the
parent's remaining envelope. Resume, replacement, and child attachment must not
replenish fuel.

## 12. Sealed Primitive Registry

When the current `GraphRegistry` does not find an entry in the local overlay,
it falls back to the process-global singleton registry.

Evidence:

- `include/neograph/graph/registry.h:12-20`
- `src/core/graph_loader.cpp:542-582`

For tenant and shared modules and Programmable Harness admission, the ambient
global fallback must not be used. A sealed mode is required that provides:

- an immutable registry snapshot
- node, reducer, and condition primitive identity
- semantic version and implementation digest
- config, input, and output schema digest
- channel effect contract
- control contract
- capability and effect closure
- registry manifest digest
- no mutation after compile or admission

The current Harness node types are also registered in the process-global
`NodeFactory`. These must be moved to a scoped Harness primitive registry.

## 13. Generic Worker Invocation

The worker contract and topology invocation are separated.

```yaml
nodes:
  implement:
    type: harness.worker
    worker_ref: implementer
    input_map:
      task: objective
      code_map: context
    output_map:
      patch: candidate_patch
    tools: [repo.read, patch.apply]
```

Required semantics:

- reuse of a single worker contract from multiple nodes
- typed input construction from channels
- selection of output channel and write mode/reducer
- per-invocation tool subset application
- no override broader than the worker base authority
- selection of repair feedback source and retry contract
- verification of compatibility between worker result schema and output port
  schema

## 14. Program Output and Terminal Semantics

The mandatory single judge is removed; the topology declares outputs and
terminals.

```yaml
outputs:
  result:
    channel: final_review
    schema: ReviewResult

terminals:
  success: done
  failure: failed
  waiting: approval_wait
```

The judge becomes an optional primitive. Programs may have multiple acceptance
gates and reviewers, and a program without a judge is valid.

The compiler and binding check that a declared success terminal does not
terminate without an output or acceptance contract, and that each terminal is
structurally reachable. When dynamic control is present, the analysis includes
the possible target sets from primitive control contracts and the external
imports declared by bytecode modules. The compiler
does not claim that every execution of a program with general cycles actually
reaches success. That liveness property is the domain of a bounded-loop proof
profile or a runtime fuel and non-progress monitor.

## 15. Subgraph and Module

The current `subgraph` node accepts an inline topology object or an external
file path and compiles a child `GraphEngine`.

Evidence: `src/core/graph_loader.cpp:239-306`

In a programmable artifact, mutable external file paths are not permitted.
Only the following three forms are allowed:

- inline strict-core child topology
- inline typed control bytecode module
- immutable module coordinate and digest

A module uses a private namespace and connects to the parent only through
typed public ports. The current Elaborator template channel performs a global
merge, so it is not used as the basic mechanism for module isolation.

```yaml
module_use:
  coordinate: org/verified.parallel_research@3.1.0
  digest: sha256:...
  inputs:
    objective: task.objective
  outputs:
    evidence_bundle: research.evidence
```

After linking, whole-program compilation, validation, effect closure, and
authority binding are re-executed rather than simply merging per-module
validation results.

## 16. Authoring Mode

Two authoring modes are provided.

### 16.1 Core Mode

```json
{
  "harness": {
    "mode": "core",
    "definition": {
      "schema_version": 1,
      "channels": {},
      "nodes": {},
      "edges": []
    }
  }
}
```

Core mode is a topology-complete escape hatch. It accepts all strict-core
topology permitted by the sealed registry, preserves canon-equivalent topology,
and then applies deterministic graph-to-bytecode lowering for execution.

### 16.2 Program Mode

Program mode provides domain constructs such as `worker`, `switch`, `parallel`,
`join`, `foreach`, `retry`, `accept`, `effect`, `await`, `child_call`,
`module_use`, `stop`, and `fail`.

The program-mode compiler produces one executable bytecode program and one
inspectable semantic projection:

- constructs that are statically expressible are represented in strict-core
  topology and lowered to equivalent typed bytecode
- runtime-dependent loops, functions, event waits, and cancellations are lowered
  directly to typed bytecode and conservatively represented in the required
  semantic projection
- heavy or effectful domain operations are lowered to sealed primitive imports

The semantic projection, optional strict-core topology, and bytecode lowering
are versioned rules, not internal compiler heuristics. Topology never selects a
separate execution backend.
The same source, compiler profile, and dependency set must produce a
byte-identical Program Bundle. Users must be able to inspect the emitted
topology, bytecode disassembly, import table, and source map.

## 17. Translation and Provenance Invariants

The existing strict core translation validation is retained.

```text
canon(core) == canon(GraphCompiler::compile(core).to_json())
```

The following invariants are added for Program Bundle compilation:

1. `lower(program)` must be deterministic and bounded.
2. The same input, profile, and dependency set must produce a byte-identical
   bundle.
3. Every semantic DSL key must be consumed.
4. Unknown keys and silently dropped constructs must be a compile error.
5. Every primitive reference must resolve to exactly one implementation
   identity.
6. Every accepted directive clause ID must map to a node, edge, gate, policy,
   or diagnostic.
7. The source map must connect directive spans, high-level constructs, semantic
   projection IDs, bytecode ranges, and module sources. Core JSON pointers are
   additionally required when a strict-core topology is present.
8. Authority and effect declarations must not be hidden in annotation
   namespaces.
9. After module linking, whole-program translation, semantic, and binding
   validation must be re-executed.
10. Turing-complete runtime behavior must not make the lowering itself
    unbounded.
11. Internal VM control transfers must be verified by the verifier within the
    module boundary.
12. External task, effect, module, and capability targets must be sealed
    imports or opaque handles.
13. When a construct has a static topology projection, an observable equivalence
    fixture must prove that its generated bytecode preserves that projection.
14. Unknown bytecode format, opcode, import, or kernel ABI must fail closed at
    compile and resume.

The admission receipt includes at minimum:

- source DSL hash
- normalized directive hash
- Program Bundle hash
- semantic projection hash
- strict-core component hash when present, otherwise its canonical absence
  marker
- bytecode module, format, and verifier profile hash
- kernel ABI and fuel schedule version
- compiler build and profile
- primitive registry digest
- capability manifest digest
- policy snapshot hash
- module dependency root
- source map and provenance root

## 18. Completeness Verification Plan

### 18.1 Core Corpus Passthrough

Every strict-core topology fixture is compiled through Harness core mode. The
returned core lockfile is verified to be canon-equivalent to the input.

Fixtures to include:

- linear graph
- static fan-out
- barrier fan-in
- closed and open conditional route
- cycle
- static interrupt
- retry policy
- inline nested subgraph
- custom reducer and condition through sealed registry
- dynamic control primitive contract

### 18.2 Palette Acceptance

Every primitive that the schema advertises as admissible must have at least one
valid fixture. No primitive visible in the schema may be rejected by the
Harness.

Conversely, non-admissible primitives must be explicitly marked as unavailable
or rejected in the schema.

### 18.3 Direct/Harness Execution Equivalence

Before VM cutover, the same core using only deterministic primitives is executed
through two oracle paths:

```text
GraphEngine direct execution
Harness core-mode execution
```

Channel results, selected routes, barrier behavior, and topology trace (aside
from Harness instrumentation) must be identical.

For VM cutover, only `equivalent` classes admitted under `graph-compat-v1` are
required to match the pinned GraphEngine oracle exactly. A `versioned_change`
does not satisfy equivalence: it requires explicit source migration or opt-in to
`program-v1`, a compile diagnostic naming the changed behavior, and fixtures for
the declared new transition sequence. `drain_only` cases never start as new VM
runs.

### 18.4 Dynamic Control Escape Tests

The following are verified to fail closed:

- undeclared `goto` target
- undeclared `Send` target
- send count exceeded
- invalid send payload
- under `program-v1`, multiple distinct parallel `Command.goto` targets reject
  the full batch; under `graph-compat-v1`, generated bytecode deterministically
  proposes only the legacy routing-order winner
- child authority widened
- child budget or fuel widened
- unregistered dynamic primitive

### 18.5 Registry Sealing Tests

- process-global fallback blocked
- primitive version or digest mismatch
- reducer or condition missing
- schema, effect, or control contract changed
- registry mutation after admission
- revoked or quarantined module

### 18.6 Module/Subgraph Hygiene Tests

- private channel collision prevented
- typed port mismatch rejected
- hidden transitive effect rejected
- dependency cycle rejected
- mutable external-file dependency rejected
- child or module authority broader than parent rejected

### 18.7 Clause Completeness Gate

An LLM-generated program is checked against the normalized directive contract.

- failure if an accepted clause maps to no program element or diagnostic
- failure if an unsupported clause is silently dropped
- zero side-effect dispatches if a policy conflict is unresolved
- completeness is measured not as a claim of perfect understanding of the
  original natural language in full, but against the normalized clauses of
  a versioned labeled corpus

### 18.8 Computational Expressiveness and Termination Gate

- typed bytecode branch, loop, and local state express a counter-machine
  fixture
- the same fixture is executed through the pinned GraphEngine oracle and through
  strict-core-to-bytecode lowering; observable step prefixes are compared
- under finite fuel, execution stops exactly with `budget_exhausted`; fuel
  does not increase after resume or fork
- undeclared imports, capabilities, and forged opaque handles are rejected
  before compile or dispatch
- loop bound or ranking certificate violations in a bounded proof profile are
  rejected
- for general loops and recursion, the compiler diagnostic is verified not to
  overstate termination or reachability of success

### 18.9 Bytecode Verifier Matrix

- invalid opcode, malformed block, invalid branch, and type mismatch rejected
- use-before-definition and invalid local, stack, or memory access rejected
- import signature mismatch and unknown kernel ABI rejected
- declared memory, call depth, and initial resource limit exceeded rejected
- long basic blocks without a safe point rejected at instrumentation or compile
  time
- verifier profile and module digest mismatch rejected before resume
- zero task or effect dispatches confirmed on verifier rejection

### 18.10 Determinism and Replay

- same checkpoint and event prefix produces a byte-identical `TransitionBatch`
- absence of opcodes for direct access to wall clock, OS random, environment,
  and filesystem confirmed
- recorded LLM, tool, and effect results are reused during replay; zero
  external dispatches confirmed
- an adversarial fixture with altered event ordering produces a different
  journal identity or an explicit error
- a module change producing a different command sequence triggers
  `NONDETERMINISTIC_CONTROL`
- different quantum boundaries produce the same committed transitions and final
  state

### 18.11 VM Checkpoint and Migration

- full round trip of PC, function/block, stack, local/memory, handles, and fuel
- after a process restart during a wait, resume from the same subscription
- exact old module hash absent causes resume to fail closed
- approved pure migration transforms old state to the new schema and preserves
  fork lineage
- migration must not call effects or imports
- migration fuel, memory, output, and schema violations leave the source checkpoint unchanged
- native migration callback is rejected before the P9 sandbox profile exists
- golden fixture required for every supported predecessor schema
- side-by-side old and new module execution must not see each other's state or
  registry

### 18.12 Topology Extension Conformance

- `race`, `await_all`, `quorum`, retry, and speculative branch are implemented
  as bytecode libraries
- during migration, a diff gate confirms that adding the above constructs
  requires no new feature-specific branches in the frozen legacy scheduler or
  checkpoint schemas; after cutover, the scheduler is absent from Harness
- loser cancellation, quorum impossibility, and parallel tie-breaking are
  verified with deterministic fixtures
- kernel-level concepts such as global fairness and transaction isolation
  cannot be registered as extension-only; an explicit ABI or schema diagnostic
  must be emitted

### 18.13 Trace Privacy and Retention

- credentials and configured secret patterns are absent from all persistence modes
- `METADATA_ONLY` and `REDACTED` runs do not advertise unsupported payload replay
- `FULL` payload access is tenant-scoped, encrypted, authenticated, and audited
- retention expiry and deletion/tombstone propagation cover journal, checkpoint,
  artifact, replay derivative, and migration output
- hashes detect mismatch but are never accepted as substitutes for missing replay payloads
- policy changes cannot retroactively expose previously redacted content

### 18.14 Effect and Budget Crash Matrix

- crash before outbox commit produces no external dispatch
- crash after outbox commit and before dispatch resumes from the committed record
- crash after remote execution and before result persistence becomes deduplicated
  completion when status/idempotency is available, otherwise `ambiguous`
- unresolved non-idempotent effects are never automatically redispatched
- parallel child admission cannot spend the same remaining reservation twice
- rejection, cancellation, failed dispatch, fork, and replacement follow the
  documented reservation settlement rules
- task/effect result duplicates with the same identity are idempotent; conflicting
  duplicates fail closed and remain auditable

## 19. Phased Implementation Order

### P0: Completeness Contracts and Red Tests

- update #245 and #252 to the layered typed-bytecode contract before implementation
- fix topology, control, behavior, and authority completeness definitions
- write strict-core topology corpus
- add regression tests for current palette and admission mismatch
- prepare direct GraphEngine versus Control VM equivalence harness

### P1: Topology-Complete Core Mode

- inject scoped sealed registry and profile into `HarnessServiceConfig`
- add an admission path without process-global palette fallback
- replace hard-coded worker and judge type restrictions with profile-based
  admission
- add generic program outputs and terminal contract
- guarantee canon-equivalent compile for all admitted strict-core topologies

### P2: Generic Worker Dataflow

- separate reusable worker contract from invocation node
- typed input and output channel mapping
- per-invocation tool attenuation
- schema-compatible sequential and parallel worker composition

### P3: Transition Protocol, Reference Kernel, and Migration Boundary

- version-1 `QuantumInput`, `TransitionBatch`, continuation, provisional-handle,
  and `CommitResult` canonical encodings
- pure in-memory reference Kernel state machine for atomicity, conflict, cursor,
  reservation, and fuel contract tests; it is not a production persistence path
- migration-only Program Bundle envelope capable of binding semantic projection,
  canonical topology, and future executable control bytecode
- `migration_only` topology envelope, manifest, dependency root, and legacy
  oracle trace identity; do not issue a final executable bundle hash or
  admission receipt before bytecode exists
- configured trust root, publisher/scope authorization, signature/attestation,
  and revocation/quarantine checks
- transitional adapter recording current static graph super-steps as the
  `TransitionBatch` equivalence oracle
- task, event, effect, and handle identity with atomic batch validation
  contract
- adapter equivalence test for current checkpoint and replay behavior
- freeze the legacy scheduler against new orchestration features
- at this phase, the scheduler is not yet replaced; existing behavior is
  encapsulated behind the protocol solely to support measured migration
- define the compatibility matrix, legacy checkpoint inventory, and adapter
  removal gate; execution of that gate begins only after P4 and P5 exist

### P4: Minimal Core-to-VM Vertical Slice

- bytecode format, typed function and control flow, canonical encoder and
  disassembler
- validator and verifier with machine-readable diagnostics
- pure instruction interpreter
- deterministic strict-core-to-bytecode lowering for a strict linear topology
  with one or two sealed pure task targets and declared writes
- minimal sealed `task.spawn`, task-result event, `state.write`, reducer preview,
  `checkpoint.yield`, `complete`, and `fail` Kernel imports
- required semantic projection, topology-to-bytecode source maps, and canonical
  equivalence fixtures
- final executable Program Bundle assembly, digest, and admission receipt only
  after the minimal hostcalls and atomic commit path execute end to end
- registry-coupled lowering completeness check and machine-readable
  `equivalent`/`versioned_change`/`drain_only` compatibility matrix
- instruction, memory, and call-depth fuel with bounded quantum
- PC, stack, local/memory, and remaining fuel checkpoint
- no-effect counter-machine, loop, recursion, and cancellation responsiveness
  tests
- the interpreter is first implemented in reference mode; JIT is a separate
  decision

### P5: Static Expansion, Remaining Durable Hostcalls, and Orchestration Library

- expand strict-Core lowering in oracle-proven order: reducers, fan-out,
  conditional routes, cycles, barriers, subgraphs, retry, interrupt, `Send`, and
  `Command.goto`
- add remaining `event.subscribe/consume`, `task.cancel`, `timer.schedule`, and
  `effect.request` sealed imports
- opaque handle type and scope verification
- transition-before-dispatch batch validation
- dynamic decision journal, replay, and nondeterminism detection
- `race`, `await_all`, `quorum`, retry, and speculative branch standard library
- extension conformance gate that adds no feature-specific branch to the frozen
  legacy scheduler during migration and no new kernel checkpoint semantics
  after cutover
- route new Harness runs through Control VM by default after the equivalence,
  checkpoint, replay, and fork gates pass
- convert `GraphEngine` execution APIs to compatibility facades that cannot
  start the legacy runtime
- keep the frozen legacy runtime resume-only until the store-wide checkpoint
  inventory reaches zero, then remove it from Harness and distribution builds

### P6: High-Level Program Dialect

- `worker`, `switch`, `parallel`, `join`, `foreach`, `race`, `quorum`, `retry`,
  `accept`, `effect`, `await`, `cancel_scope`, `stop`, `fail`
- per-construct topology projection and bytecode lowering rule
- deterministic lowering and strict consumed-key accounting
- directive-to-core and directive-to-bytecode source map
- pinned GraphEngine oracle versus graph-to-bytecode VM equivalence fixtures

### P7: Module and Child Harness

- immutable module coordinate and typed port
- topology-bearing and bytecode-only module private namespace linking
- exact extension and module hash pinning with metered, no-import bytecode state
  migration
- whole-program validation
- child proposal, compile, admission, and attachment
- authority and fuel attenuation

### P8: Directive Compiler

- natural-language directive normalization
- stable clause ID
- conflict and unsupported diagnostics
- bounded model repair
- completeness corpus and conformance matrix

### P9: Extension Distribution Hardening

- versioned C ABI, local RPC, and Wasm component candidate comparison prototype
- engine-scoped immutable extension registry
- validate, lower, migrate, and explain extension contract
- untrusted module memory, CPU, output, and hostcall sandbox
- sandboxed native migration with canonical I/O and the same verifier/resource limits
- registry publication, key rotation, offline trust snapshot, transparency, and
  revocation propagation hardening

## 20. First Implementation Milestone

The completion criteria for the first implementation are defined in a single
sentence:

> Every strict-core topology permitted by the sealed registry compiles through
> Harness core mode in a canon-equivalent manner and, on a deterministic
> fixture, exhibits the same topology behavior as direct GraphEngine execution.

After this criterion is satisfied, the high-level `worker`, `foreach`,
`accept`, and `child_call` syntax and the VM are added. This prevents the VM
migration from becoming a large-scale rewrite that inadvertently alters
existing graph semantics.

The first completion criterion for VM introduction is:

> Every strict-core topology admitted as `equivalent` under `graph-compat-v1`
> lowers to a verified bytecode controller whose execution emits the same
> versioned `TransitionBatch` sequence and observable result as the pinned
> GraphEngine oracle. Every `versioned_change` requires explicit migration to
> `program-v1`. Adding a reference `quorum` library introduces no
> quorum-specific change to the durable kernel or checkpoint semantics while
> preserving deterministic replay, cancellation, authority, and fuel accounting.

Here, "every admitted" is enforced mechanically rather than inferred from a
finite fixture corpus. The sealed registry entry for every node, reducer,
condition, and dynamic-control primitive must name a verified lowering and
compatibility class. A strict-core document containing an unclassified or
`drain_only` entry can still be parsed and diagnosed, but it is not admitted as
a new VM run. A `versioned_change` entry is not admitted under
`graph-compat-v1`; it may enter `program-v1` only after explicit source migration
or opt-in. The corpus supplements this registry-wide contract with regression
coverage; it does not stand in for lowering completeness.

Before this gate is passed, the strict-core scheduler is not removed and
existing topologies are not bulk-converted to bytecode. After the equivalence,
checkpoint, replay, resume, and fork gates pass, Harness must route new runs
through the Control VM and stop evolving the legacy scheduler. The public
`GraphEngine` API then becomes a compatibility facade over Program Bundle
compilation and execution. The old runtime remains available only to drain
inventoried checkpoints and is removed after the zero-resumable-checkpoint gate.
This staged replacement preserves the currently working runtime without
allowing the migration adapter to become a permanent second engine.

## 21. Main Risks

- Allowing all global node types can bypass the Harness capability policy.
- Allowing `Send` and `Command.goto` without a dynamic control contract
  nullifies static reachability, acceptance path, and authority analysis.
- If the VM directly owns state commit, effects, or checkpoint, it becomes a
  second runtime, splitting replay and recovery semantics.
- If the GraphEngine scheduler remains an independently evolving execution path
  after VM cutover, every scheduling, retry, cancellation, checkpoint, and
  replay change requires permanent cross-engine equivalence work.
- Treating local outbox commit as exactly-once external execution can duplicate
  or lose effects across crash boundaries; unsupported outcomes must remain
  ambiguous until reconciliation.
- Checking parent budget without an atomic reservation lets parallel children
  oversubscribe the same fuel, cost, or concurrency envelope.
- Implementing only metering while omitting the verifier, sealed imports, and
  memory/handle safety makes a Turing-complete payload a surface for authority
  and availability attacks.
- Preserving bytecode as an opaque blob only degrades static validation, code
  review, source-level debugging, and provenance compared to the current strict
  core.
- Not pinning the VM checkpoint to the exact module hash causes a long-running
  run to produce a different command sequence under new code, triggering a
  nondeterministic replay failure.
- Permitting all new semantics as plugins splits per-module scheduler and
  consistency semantics, making whole-program reasoning impossible.
- Treating a content digest as publisher authorization can execute correctly
  hashed but untrusted native code with host authority.
- Without versioning the fuel schedule itself, the exhaustion point and replay
  result of the same artifact can vary across runtime releases.
- Claiming static guarantees of termination or reachability of all success
  paths while permitting general cycles overstates an undecidable property.
- External-file subgraphs and mutable packages are not reproducible artifacts.
- Using global channel merge in module templates can cause namespace collisions
  and tenant leakage.
- Compiler validity must not be expressed as answer correctness.
- Replay and privacy/redaction can conflict depending on the stored payload
  level.
- Core mode provides topology completeness but must not become an escape hatch
  that bypasses authority admission.

## 22. Related Code

- `include/neograph/graph/elaborator.h`
- `include/neograph/graph/compiler.h`
- `include/neograph/graph/registry.h`
- `include/neograph/graph/types.h`
- `include/neograph/graph/node.h`
- `include/neograph/graph/scheduler.h`
- `include/neograph/graph/checkpoint.h`
- `include/neograph/graph/coordinator.h`
- `src/core/elaborator.cpp`
- `src/core/graph_compiler.cpp`
- `src/core/graph_loader.cpp`
- `src/core/graph_validator.cpp`
- `src/core/scheduler.cpp`
- `src/core/graph_executor.cpp`
- `src/core/graph_coordinator.cpp`
- `src/core/sqlite_checkpoint.cpp`
- `src/core/postgres_checkpoint.cpp`
- `src/grpc/grpc_checkpoint.cpp`
- `src/mcp/harness.cpp`
- `src/mcp/harness_provider.cpp`
- `docs/HARNESS_MCP.md`
- `tests/test_elaborator.cpp`
- `tests/test_compiler.cpp`
- `tests/test_compiler_strict.cpp`
- `tests/test_validator.cpp`
- `tests/test_harness_service.cpp`
- `tests/test_scheduler.cpp`
- `tests/test_multi_send_routing.cpp`

## Appendix A. Research Evidence and Design Application

The materials below are not intended to be cloned as specific implementations.
They were used to verify the boundary decisions in this document.

| Material | Core content verified | Conclusion applied to NeoGraph |
|---|---|---|
| Lattner et al., *MLIR: A Compiler Infrastructure for the End of Moore's Law*, arXiv:2002.11054 and [MLIR Defining Dialects](https://mlir.llvm.org/docs/DefiningDialects/) | Multiple abstraction levels, progressive lowering, dialect verifier, source location/traceability, runtime-extensible operation/type definition | Provide a high-level Program dialect with per-extension verifier and lowering, and preserve source-to-core/bytecode provenance |
| Lin et al., *WaveCert: Translation Validation for Asynchronous Dataflow Programs via Dynamic Fractional Permissions*, arXiv:2312.09326 | Canonical schedule simulation alone is insufficient; confluence/race freedom of asynchronous schedules require separate proof | The current canon round-trip is structural translation validation. It does not overstate proof of behavioral equivalence for parallel primitives and separately verifies reducer/effect conflicts |
| Burckhardt et al., *Serverless Workflows with Durable Functions and Netherite*, arXiv:2103.00033 | Lowering a host-language workflow to a small task/instance model, using history replay, deterministic orchestration, and causally consistent commit | Separate rich source expressiveness from a small durable runtime model; do not hide nondeterminism and effects behind the journal boundary |
| [Temporal Workflow Definition](https://docs.temporal.io/workflow-definition) | General-language workflows must produce the same command sequence on replay; changing running workflow code requires versioning/patching | VM module hash pinning, command/transition replay comparison, and side-by-side versioning are required |
| [Durable orchestrator code constraints](https://learn.microsoft.com/en-us/azure/azure-functions/durable/durable-functions-code-constraints) | Direct time, random, network, or thread access breaks replay; I/O must be separated into durable activity/context APIs | Control VM ambient I/O is prohibited; time, random, model, and tool results are supplied only through journaled hostcalls |
| [WebAssembly Core Specification 3.0 Introduction](https://webassembly.github.io/spec/core/intro/introduction.html) | A portable validated ISA restricts environment access to embedder-provided imports; the core spec does not define host interaction | Separate bytecode safety from host authority; use the sealed import table as the capability boundary |
| [Linux eBPF verifier documentation](https://docs.kernel.org/bpf/verifier.html) | Bytecode programmability maintains kernel safety through CFG, type, range, and resource verifier plus a restricted helper API | Metering alone is insufficient; a verifier and typed hostcall contract are required |
| Debenedetti et al., *Defeating Prompt Injections by Design*, arXiv:2503.18813 | Extract control and data flow from trusted queries, enforce capability policy at tool call time | Prevent untrusted runtime data from creating targets or capabilities; combine value provenance with call-site policy |
| Murray et al., *CIEL: A Universal Execution Engine for Distributed Data-Flow Computing*, NSDI 2011 | Implement a Turing-complete Skywriting language and runtime-dependent task graph on top of a fault-tolerant dataflow engine | Separating a Turing-complete control language from a dataflow execution kernel is a viable architecture; NeoGraph constrains this with typed, metered, durable contracts |
| van der Aalst et al., *Workflow Patterns*, Distributed and Parallel Databases 14, 2003, DOI:10.1023/A:1022883727209 | A recurring taxonomy of workflow control-flow patterns and a comparison framework across implementations | Use sequence, split/join, synchronization, multiple instances, and cancellation as a conformance corpus; do not confuse pattern coverage with Turing completeness |

Strong claims not present in these materials are also distinguished:

- MLIR does not prescribe NeoGraph's exact IR schema.
- WaveCert's proof is not currently performed by the NeoGraph translation
  validator.
- Wasm's memory safety does not automatically provide NeoGraph's determinism,
  durability, or capability policy.
- The eBPF verifier architecture does not automatically validate the NeoGraph
  bytecode verifier.
- The replay model of Temporal and Durable Functions is not identical to the
  NeoGraph checkpoint model; only the command-history comparison principle is
  used as a design basis.
- None of the cited workflow systems removes the need for idempotency or
  reconciliation at external effect boundaries.
- CaMeL's prompt-injection security guarantee does not automatically apply to
  the entire NeoGraph system.
- CIEL does not claim that Turing completeness guarantees safety or
  termination.
- Workflow pattern coverage is not proof of semantic completeness or answer
  correctness.
