# QuickJS Control Runtime Migration Plan

Status: Accepted execution plan; implementation not started
Date: 2026-08-08
Architecture: `QUICKJS_CONTROL_ARCHITECTURE.md`
Source baseline: `61661e9ad1fc386b5142139c48c327ede7464633`
Executable gates: `../spec/quickjs-control-runtime.sdd.yaml`
Tracking epic: [#23](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/23)
Delivery issues: [#24](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/24),
[#25](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/25),
[#26](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/26),
[#27](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/27), and
[#28](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/28)


## 1. Starting point

NeoGraph already has valuable runtime infrastructure that the language cutover
must preserve:

- `GraphEngine` remains the only Core/application-node executor;
- Program publication, admission, activation, invocation, journal, effect,
  child, fork, replay, and transition-store contracts exist;
- executable identity, capability/effect closure, binding receipts, budgets, and
  owner/tenant isolation exist;
- Program-v2/v3/v4 operation trees implement bounded orchestration, parallel
  child maps, and part of dynamic task-graph expansion; and
- Harness currently translates bounded Core DSL or Program JSON through Program
  admission.

The problem is the authoring direction. Core DSL composition and each new
Program construct spread language semantics across elaborator, source schema,
parser, compiler, typed plan, runtime, serialization, migration, diagnostics,
tests, and documentation. NeoGraph will stop growing both languages and move
Core graph composition and general control computation to standard JavaScript
on embedded QuickJS.

This migration is a replacement, not a second permanent language stack.

## 2. Keep, freeze, replace, delete

### Keep

- strict Core JSON as canonical serialization and low-level interchange;
- `GraphEngine`;
- immutable Program versions and bundles;
- catalog, admission, activation, owner/tenant, capability, effect, budget,
  child, checkpoint, replay, and transition-store contracts;
- existing executable manifests and exact binding receipts; and
- current Core-only optional-build boundary.

### Freeze

- bounded Core DSL authoring, including `vars`, interpolation, `templates`,
  `use`, and `when`;
- the Core `graph::Elaborator` authoring path and Harness `mode: "dsl"`;
- Program JSON operation-tree authoring and Program-v2/v3/v4 source schemas;
- Harness `mode: "program"`;
- additions to `ProgramOperationKind`; and
- new general computation in `branch`, `loop`, `map`, `parallel_map`, or
  `expand_task_graph` descriptors.

Frozen surfaces receive only correctness, data-loss, security, and migration
fixes required to drain existing versions.

### Replace

- all user-authored graph and Program source with UTF-8 JavaScript;
- Core DSL elaboration with bounded `define()` evaluation, a sealed graph
  builder, typed validation, and canonical strict Core lowering;
- operation-tree compilation with QuickJS generator compilation plus sealed
  module and host-binding resolution;
- in-memory operation continuation with yielded typed commands;
- operation-tree crash recovery with deterministic source replay; and
- ad hoc native extension paths with one versioned C ABI and C++ wrapper backed
  by existing executable manifests and binding receipts.

### Delete after cutover

- legacy Core DSL and Program DSL schemas and authoring documentation;
- the Core DSL parser/elaborator and Harness `mode: "dsl"` translation;
- operation-specific Program source parsing and source-only plan descriptors not
  needed for stored-version drain;
- Harness Program JSON translation;
- compatibility runtime branches after the final stored version drains; and
- duplicate tests and examples whose only contract is legacy authoring syntax.

Deletion occurs only after stored data is classified and the announced rebuild
boundary is complete. Legacy code is not relabeled as the new JavaScript
runtime.

## 3. Delivery sequence

### Q0 — Dependency and runtime qualification

Deliverables:

- select an exact QuickJS release and source digest;
- record license and provenance;
- define build options that exclude unwanted standalone facilities;
- add an optional, default-off build target that does not leak into Core;
- prove Linux, macOS, Windows, static/shared, installed-consumer, sanitizer,
  binary-size, startup, and allocation behavior; and
- retain a minimal C embedding smoke program.

Exit gate:

- exact dependency evidence is accepted and a Core-only consumer remains
  bit-for-bit free of QuickJS link requirements.

### Q1 — JavaScript authoring contexts and sealed compilation

Deliverables:

- add owned UTF-8 JavaScript source with `define()` and generator `main()`
  profiles;
- expose a sealed graph-builder API that produces typed graph data only;
- evaluate `define()` in a dispatch-free context with instruction, memory, and
  time limits and lower its result to canonical strict Core IR;
- define entry point, module manifest, source-map, and profile fields;
- construct allowlisted QuickJS contexts without `std`, `os`, ambient module
  lookup, or arbitrary FFI;
- compile only sealed source/module identities;
- store exact runtime/compiler/profile identity in Core and Program bundles; and
- map syntax, graph-validation, and module diagnostics to stable source
  coordinates.

Exit gate:

- valid JavaScript graph and Program sources seal immutably; malformed,
  unsealed, or invalid graph source performs zero dispatch.

### Q2 — Generator command protocol

Deliverables:

- define the canonical yielded-command schema;
- implement `ng.callCore`, `ng.spawn`, `ng.await`, `ng.emit`, `ng.checkpoint`,
  `ng.cancelScope`, and approved host-capability constructors;
- resolve source names to immutable import slots at admission;
- validate command kind, arguments, result schema, capability, effect, and
  budget before dispatch; and
- resume generators with canonical success/failure values.

Exit gate:

- representative branch, loop, closure, child, parallel, and error workflows run
  without adding a new NeoGraph control syntax or bypassing `ProgramRuntime`.

### Q3 — Metering, cancellation, and isolation

Deliverables:

- enforce QuickJS runtime memory limits;
- interrupt infinite computation at bounded host quanta;
- account instruction, replay, wall, allocation, child, effect, token, and
  monetary budgets without renewal;
- propagate cancellation into live host calls and stop new dispatch;
- reject clocks, random values, files, network, process, environment, workers,
  dynamic modules, and unregistered C functions; and
- fault-test native callback ownership and VM teardown.

Exit gate:

- adversarial fixtures terminate with stable classifications and no forbidden
  external access.

### Q4 — Deterministic replay and recovery

Deliverables:

- record canonical command coordinates and outcomes;
- restart pinned JavaScript from its entry point;
- consume recorded results without redispatch;
- compare replayed command kind, slot, source coordinate, arguments, and effect
  identity against the journal;
- stop on nondeterminism or runtime/profile mismatch;
- resume at the pending journal head; and
- support explicit canonical application-state checkpoints without raw VM heap
  images.

Exit gate:

- crash injection before and after every command publication boundary neither
  loses nor duplicates work or effects.

### Q5 — Versioned native control ABI

Deliverables:

- publish a versioned C ABI for pure intrinsics and asynchronous host bindings;
- provide a C++ convenience wrapper;
- extend existing executable manifests and catalog bindings rather than adding a
  second registry;
- define ownership, allocator, cancellation, completion, destruction, exception,
  and thread rules; and
- validate semantic version and implementation digest at admission and replay.

Exit gate:

- native ABI conformance passes across supported compiler/build combinations and
  malformed plugins fail closed.

### Q6 — Authoring cutover

Deliverables:

- expose one JavaScript authoring mode for Core graph definitions and Programs;
- update public schemas, examples, API references, Harness transport, and source
  maps;
- stop accepting new Core DSL and Program JSON operation-tree source at the
  announced boundary;
- classify every stored legacy Core definition and Program version as
  translated, drain-only, or rejected;
- keep existing in-flight runs pinned; and
- provide explicit diagnostics for every legacy source submission.

Exit gate:

- all new user authoring uses JavaScript and no adapter silently selects legacy
  semantics.

### Q7 — Legacy removal

Deliverables:

- verify no active or recoverable stored definition or version requires legacy
  authoring or runtime code;
- delete the Core DSL parser/elaborator and legacy Program
  schema/parser/compiler/dispatcher branches, examples, and tests;
- remove compatibility build/link dependencies;
- run installed-consumer, graph-equivalence, storage migration, replay, fault,
  sanitizer, and performance gates; and
- update the architecture and changelog only after the cutover smoke test works.

Exit gate:

- one user-authored language, one canonical Core IR, and one Program
  durability/effect model remain.

## 4. Stored data and compatibility

Every admitted legacy Core DSL definition and Program version is classified
before authoring cutover:

| Classification | Rule |
|---|---|
| `translated` | A deterministic translator and equivalence corpus prove the JavaScript form preserves observable behavior and identities that must remain stable. |
| `drain_only` | Existing runs may resume on the pinned legacy runtime, but no new run, definition, or version may be published. |
| `rejected` | Exact semantics, executable identity, authority, or stored state cannot be preserved; migration fails explicitly. |

Translation is not assumed. Core topology source maps, dynamic task-graph
proposals, child handles, checkpoints, and effects require individual
compatibility evidence. A source that merely looks similar does not qualify as
equivalent.

Strict Core documents remain supported as validated low-level interchange, not
as a second user programming language.

## 5. Runtime invariants

The cutover must preserve:

1. `GraphEngine` is the only Core/application-node executor.
2. Core does not link or branch on Program or QuickJS when disabled.
3. Raw or unadmitted JavaScript performs zero dispatch.
4. Published source, runtime, module, executable, and policy identities are
   immutable.
5. Child and replacement authority can only attenuate.
6. Resume, replay, retry, child, fork, and replacement do not replenish budget.
7. External effects cross the existing durable journal/outbox boundary.
8. Replay consumes recorded outcomes and never repeats a completed
   non-idempotent effect.
9. Runtime/profile/source mismatch and nondeterministic replay fail closed.
10. Transport adapters do not own alternate compilation or runtime semantics.

## 6. Validation matrix

### Language

- bounded `define()` graph construction, helper composition, and canonical Core
  equivalence;
- nested loops, closure capture, recursion, generator delegation, exceptions;
- deterministic object/array handling and canonical bridge conversion;
- unsupported value types, cyclic values, symbols, functions, and host objects;
- syntax, module, entry-point, graph-validation, and source-map diagnostics.

### Isolation

- `std`/`os`, dynamic modules, FFI, file, network, process, environment;
- clock, random, workers, atomics, shared memory, and host-global mutation;
- heap exhaustion, stack exhaustion, infinite loop, and interrupt races.

### Durability

- command replay, pending input, child spawn/await, cancellation, timeout;
- checkpoint, fork, migration, late completion, duplicate completion;
- idempotent and non-idempotent effects around every crash boundary;
- runtime/source/profile/manifest mismatch.

### Native ABI

- owned and borrowed byte lifetimes;
- completion after cancellation or runtime destruction;
- plugin exception/panic containment;
- duplicate callback and missing callback;
- allocator mismatch and malformed result;
- static/shared and supported compiler combinations.

### Performance

- compile-context creation, graph definition evaluation, and source compilation;
- warm generator step and yielded-command overhead;
- deterministic replay as journal length grows;
- memory per runtime and concurrent invocation;
- direct Core versus JavaScript Program wrapper;
- Program disabled versus enabled-but-unused binary and hot path; and
- repeated cold/warm distributions with explicit regression thresholds rather
  than one-off latency claims.

## 7. Rollback

Before authoring cutover, QuickJS support is optional and default-off; removing it
restores the pinned legacy path without changing stored authority.

After cutover begins, rollback means:

- stop new JavaScript admission;
- retain immutable published JavaScript versions and their exact runtime build;
- continue only runs whose runtime and dependency closure are available;
- never translate effects backward implicitly; and
- publish a new activation only after compatibility checks.

Rollback does not reopen Core DSL or Program JSON authoring and does not create
a permanent dual-stack product.

## 8. Documentation retirement

The following DSL-direction documents are removed by the architecture cutover
commit because they are no longer authoritative and retaining them would
recreate the design fragmentation this decision rejects:

- `AGENT_SYNTHESIS_PROGRAM_DSL.md`;
- `DSL_COMPOSITION_LIMITS.md`;
- `PROGRAMMABLE_HARNESS_DSL_DESIGN.md`; and
- `../spec/programmable-harness-vm-integration.sdd.yaml`.

Current implementation details remain discoverable from code, schemas, tests,
and repository history until the legacy runtime is removed. New work is tracked
only by this plan, the QuickJS architecture, the executable SDD, and their
registered GitHub issues.
