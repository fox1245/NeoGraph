# Durable child synthesis

Status: N2 implemented for the in-memory reference store, SQLite, and PostgreSQL.
N3 recovery qualification for the single-child scenario is described in
[the recovery matrix](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md).
The host entry point uses an existing top-level checkpoint and the existing
`ng.spawn` / `ng.await` child lifecycle. This is a bounded reviewed-source path;
it does not implement a model generator, template renderer, or a new DSL command.

## Host integration

Configure `RuntimeConfig::child_synthesis_gateway` and
`child_synthesis_grant_resolver`. The resolver must select an exact grant from
trusted host policy using `(owner_scope, parent_run_id, grant_id)`. Loading a
grant from a synthesis record does not establish its authority.

1. Arrange a `ProgramHandoff` at the parent's next top-level checkpoint.
2. Load the parent run, active lineage, and generation and select a reviewed
   source instantiation and grant as described in
   [the authorization contract](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md).
3. Call `ProgramRuntime::prepare_child_synthesis(owner, parent, handoff,
   proposal, grant, binding_name)` while retaining the handoff.
4. Release the handoff after the returned record reaches `Bound`.
5. The parent uses that binding name in its normal `ng.spawn` / `ng.await` path.

For example, the admitted parent can yield:

```javascript
yield ng.checkpoint({request: "reviewed-child"}, "synthesis:request");
return yield ng.await(
  ng.spawn("generated-child", input.childInput, "generated:spawn"),
  5000,
  "generated:await"
);
```

The host decides how the checkpoint request maps to reviewed source and policy.
The checkpoint payload cannot grant itself compilation or execution authority.
The durable path owns reservation through the runtime transaction; it does not
call the gateway's standalone `reserve` or `reserve_child` callbacks.

## Atomic publication

`ProgramTransitionPublication::child_synthesis_records` appends one immutable
record in the same transaction as the parent's run snapshot, journal head, and
lineage. The first `Reserved` record must bind the exact prior parent snapshot,
source lineage, resulting lineage, and a one-unit dynamic-compile debit.
A conflicting request cannot reuse an identical budget transition to fund a
different binding. Exact publication retries return `AlreadyPresent`.

SQLite uses its existing `BEGIN IMMEDIATE` transaction. PostgreSQL uses its
existing transaction and owner advisory lock. Both append to a synthesis log
and reconstruct validated request heads in append order. Failed writes roll
back the parent, lineage, and synthesis record together. The in-memory reference
retains its strong exception guarantee.

Publications carrying synthesis records use storage schema 6. Ordinary
publications continue to serialize as schema 5; readers retain schemas 1–5.
`load_child_syntheses(owner, parent_run)` returns current request heads. The base
implementation fails closed for custom stores that do not support this history.
Dynamically budgeted run recovery requires this read capability.

Records are limited to 8 MiB and 16 revisions. They retain the proposal, original
host grant and parent context, reservation, and cumulative stage outputs.
Canonical identities and stage validation reject changed prior artifacts,
different generations, renamed bindings, and modified evidence. A binding name
is unique within one parent run. The SQL log is append-only; retention and
compaction of large histories are follow-up work.

## Stages and recovery

| Durable state | Next action after recovery |
|---|---|
| `Reserved` | Claim and compile once |
| `Compiling` | Mark `ReconciliationRequired`; do not replay an uncertain compile |
| `Compiled` | Reuse the stored bundle and claim semantic validation |
| `Validating` | Mark `ReconciliationRequired`; do not rerun an uncertain validator |
| `Validated` | Reuse the accepted receipt and evidence; select admission policy |
| `Admitting` | Reuse the frozen admission and idempotent Catalog admission |
| `Admitted` | Link the exact admitted version using a scoped module receipt |
| `Bound` | Resume the parent's ordinary child command |
| `Dispatching` | Reuse the recorded child ID and input through `start_child` |
| `Spawned` | Reuse the child/result, or reconcile Core dispatch without a durable checkpoint |
| `Failed` / `ReconciliationRequired` | Retain the outcome and block automatic parent replay |

Call `recover_child_synthesis(owner, parent_run, proposal_id)` before reconnecting
an inactive parent with unfinished synthesis. A live parent requires the held
checkpoint API. Every recovery reselects host authority and checks the original
generation. Completed semantic validation and frozen admission are reused.
Semantic rejection retains its receipt and evidence and never reaches admission.

Generated bindings are resolved using the actual parent run and generation,
not the static resolver's parent-version scope. Dispatch records a stable child
ID and input before `start_child`; one grant cannot create a second invocation.
Normal child budgeting, publication, execution, and join remain authoritative.

The parent retains the compile debit on completion, retry, and recovery,
including a lost commit acknowledgement. Synthesis debits elapsed wall time;
recovered attempts also retain the original synthesis deadline. Spawn state
publication preserves the existing command's in-flight reservation.

## Qualification and remaining work

The backend conformance tests cover successful spawn/join, canonical round trips,
owner/run isolation, duplicate and concurrent requests, rollback at a backend
write failure, committed stage reuse, semantic rejection, revoked grants, and
ambiguous compile/validation claims. Linux process-exit tests terminate without
destructors after frozen admission and reopen the Program store, transition
store, and checkpoint store on both SQLite and PostgreSQL before joining the
child successfully.

The [N3 recovery matrix](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) extends coverage to
16 process-exit boundaries per database, concurrent process recovery, lost
acknowledgements, actual PostgreSQL connection termination, cancellation, version
retention, activation, and Program replacement. Its limits remain explicit:
there is no automatic reconciliation authorizer, and general mixed-effect joins,
power loss, cross-host failover, and all graph-migration/retention combinations
are not covered by the single-child scenario.

This adds public C++ types, virtual methods, and configuration fields. Rebuild
all Program consumers; do not mix old objects with the new library. It adds no
Core-only dependency or native control C ABI change.
