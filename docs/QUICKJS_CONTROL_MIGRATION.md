# QuickJS Control Runtime Migration Plan

Status: Base runtime implemented; final platform qualification and Q7 legacy removal remain pending
Date: 2026-08-08
Architecture: `QUICKJS_CONTROL_ARCHITECTURE.md`
Public authoring boundary: [`QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md`](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
Source baseline: `61661e9ad1fc386b5142139c48c327ede7464633`
Executable gates: `../spec/quickjs-control-runtime.sdd.yaml`
Post-cutover controller extension:
[`SELF_EVOLVING_AGENT_CONTROLLER.md`](SELF_EVOLVING_AGENT_CONTROLLER.md)
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

- direct C++ graph and runtime embedding APIs;
- strict Core JSON as canonical serialization and low-level interchange, not a
  standalone public source mode;
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
- standalone Core JSON and Program JSON public source endpoints;
- additions to `ProgramOperationKind`; and
- new general computation in `branch`, `loop`, `map`, `parallel_map`, or
  `expand_task_graph` descriptors.

Frozen surfaces receive only correctness, data-loss, security, and migration
fixes required to drain existing versions.

### Replace

- all user-authored graph and Program source with UTF-8 JavaScript while
  retaining direct C++ APIs for trusted embedding;
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
- standalone public Core JSON and Program JSON source constructors, schemas,
  and transport routes;
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

- expose JavaScript `define()` for Core graph definition and generator `main()`
  for Program control through direct and Harness transports;
- retain the direct C++ embedding API while documenting JavaScript as the sole
  user-authored source language;
- preserve strict Core JSON as validated canonical serialization and low-level
  interchange, not a public source mode;
- update public schemas, examples, API references, Harness transport, and source
  maps;
- stop accepting new Core DSL, standalone Core JSON, and Program JSON
  operation-tree source at an announced boundary;
- classify every stored legacy Core definition and Program version as
  `translated`, `drain_only`, or `rejected`;
- keep existing in-flight runs pinned; and
- provide explicit diagnostics for every legacy source submission.

Exit gate:

- all new user authoring uses JavaScript and no adapter silently selects legacy
  semantics.

### Q7 — Legacy removal

Deliverables:

- verify no active or recoverable stored definition or version requires legacy
  authoring or runtime code;
- delete the Core DSL parser/elaborator, legacy Program
  schema/parser/compiler/dispatcher branches, public JSON authoring routes,
  examples, and tests;
- retain canonical serialization and the trusted C++ embedding API;
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

Strict Core documents remain supported only as validated low-level interchange
and canonical storage, not as a public programming-language submission.


### Final drain proof procedure

Q7 is gated by a **frozen storage snapshot**, not by the absence of legacy
source files in a checkout. The repository supplies
[`scripts/audit_legacy_drain.py`](../scripts/audit_legacy_drain.py), which
reads only explicitly enumerated, read-only snapshot targets and emits a
canonical `neograph-legacy-drain-proof` record with a content identity.

```sh
python3 scripts/audit_legacy_drain.py \
  --inventory /secure-export/legacy-drain-inventory.json \
  --root /secure-export \
  --output /secure-export/legacy-drain-proof.json \
  --require-final
```

#### Capture handoff

The archive must represent one cutover boundary. Either place legacy
publication/resume and writes to the captured stores behind a temporary write
fence, or use the storage platform's consistent snapshot/export mechanism.
The service need not stay offline after an immutable export exists, but the
legacy write fence must remain in place through the removal deployment.

For a SQLite target, use that mechanism to materialize a standalone database
copy. Do **not** raw-copy a live WAL database. The auditor rejects any target
with sibling `-wal`, `-shm`, or `-journal` files and does not open it; their
presence means the snapshot is not an acceptable final-drain input.

The stock Harness server maps
`$NEOGRAPH_HARNESS_STATE_DIR/runs.db` to one `harness_sqlite` inventory entry.
Its sibling `checkpoints.db` is not a `harness_sqlite` source-artifact store;
retain it for the separate migration/replay evidence. If an embedding can
resume a legacy-authoring run solely from a checkpoint store or another durable
store, it is outside the current auditor's supported set and must receive a
dedicated scanner before final proof.

For an actual PostgreSQL `ProgramStore`, capture a consistent database export
rather than bind-mounting a raw `PGDATA` directory. The isolated restore target
[`tests/fixtures/q7-postgres/compose.audit.yaml`](../tests/fixtures/q7-postgres/compose.audit.yaml)
mounts `NEOGRAPH_Q7_POSTGRES_SNAPSHOT_DIR` at `/snapshot` read-only, refuses
to create a missing host path, exposes no host port, and keeps only the restore
target itself in tmpfs. It does not connect to a live deployment, create a
snapshot, or make an empty database evidence. A PostgreSQL export still needs
a dedicated, tested scanner or conversion to a supported snapshot format before
it can satisfy `--require-final`.

Before creating `inventory_complete: true`, the operator records the immutable
archive location, capture mechanism/backup identity, cutover ID, and every
durable store covered by the deployment. Those records are release evidence;
the audit hashes what it reads but cannot infer an omitted store.

The inventory accepts no implicit defaults. A minimal snapshot declaration is:

```json
{
  "format": "neograph-legacy-drain-inventory",
  "schema_version": 1,
  "cutover_id": "quickjs-control-<announced-boundary>",
  "captured_at": "2026-08-10T00:00:00Z",
  "inventory_complete": true,
  "stores": [
    { "id": "program", "kind": "program_sqlite", "path": "program.db" },
    { "id": "transitions", "kind": "program_transitions_sqlite", "path": "transitions.db" }
  ],
  "legacy_artifacts": [
    {
      "artifact_id": "program/version/<legacy-version-id>",
      "kind": "legacy_program_version",
      "classification": "rejected",
      "reason": "pre-release state intentionally discarded"
    }
  ]
}
```

The tool emits `store/version/<id>` for legacy Program versions,
`store/bundle/<id>` for orphaned legacy bundles, and `store/artifact/<id>`
for Harness artifacts. A translated record instead supplies
`replacement_artifact_id` and `equivalence_proof`; a drain-only record
supplies `legacy_runtime_identity`.

The inventory is versioned, has an operator-attested
`inventory_complete: true` boundary, and names every persisted
`program_sqlite`, `program_transitions_sqlite`, `harness_sqlite`, and
`harness_file` snapshot. The tool fingerprints every input before and after
the read, rejects symlinks, unknown schemas, malformed records, unscanned
references, and mutable snapshots, and records the exact content identities it
did inspect. A deployment with another durable source store does **not** have
a final proof until it is exported into a supported snapshot format or the
auditor is extended and tested for that format.

Every discovered legacy source must have one inventory classification:

- `translated` requires a distinct scanned replacement artifact and a
  SHA-256 equivalence-evidence identity;
- `rejected` requires an explicit terminal reason; and
- `drain_only` always blocks `--require-final`: it must be purged after its
  last pinned run drains, before parser/runtime deletion.

An active run, a pending/recoverable run, an interrupted Program result, an
unknown status, or a run pointing at an unscanned legacy bundle is a final-gate
blocker regardless of classification. A successful synthetic CTest verifies
the audit contract only; it is not a deployment proof. Q7 remains pending
until a retained production/pre-release snapshot produces a passing proof and
the remaining platform gates are accepted.

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

The base cutover ships the default `strict` authority profile. Post-cutover
developer-authorized `recorded` or `unmanaged` profiles may weaken only the
claims explicitly identified by their immutable effective guarantee floor; they
do not alter strict-profile recovery semantics or allow an in-flight child to
broaden its parent's authority.

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
and repository history until the legacy runtime is removed. Base cutover work is
tracked only by this plan, the QuickJS architecture, the
[public authoring boundary](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md), the
executable SDD, and their registered GitHub issues.

## 9. Post-cutover controller extension

The self-evolving general-agent controller is a dependent product extension, not
a second migration path. Its canonical architecture is
[`SELF_EVOLVING_AGENT_CONTROLLER.md`](SELF_EVOLVING_AGENT_CONTROLLER.md), and
its delivery/evaluation tree is tracked by
[#29](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/29) through
[#34](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/34).

The extension may start compatible implementation work after its direct base
dependencies are available, but it does not change cutover acceptance and
cannot delay deletion of the legacy Core and Program authoring DSLs. It must
reuse the one JavaScript runtime, sealed compilation path, `ProgramRuntime`,
`GraphEngine`, capability registry, journal/outbox, version catalog, and
activation CAS.

Extension delivery order is:

1. add immutable developer-authority profiles and explicit
   `strict`/`recorded`/`unmanaged` guarantee labels;
2. compile untrusted OpenAPI, A2A, MCP, and JSON Schema descriptors into
   separately admitted capability candidates;
3. store reusable Harnesses, behavioral fingerprints, failures, evaluation
   evidence, and version lineage;
4. synthesize bounded immutable candidates, evaluate them in simulation,
   shadow, and canary modes, and promote through authorized CAS activation; and
5. execute the preregistered falsification program before raising the public
   claim above the evidence-supported claim-ladder level.

Rollback of this extension deactivates or retires successor versions and
restores a previously admitted future-run activation. It never mutates
in-flight runs, erases effects or negative evidence, reopens legacy authoring,
or creates another runtime stack.
