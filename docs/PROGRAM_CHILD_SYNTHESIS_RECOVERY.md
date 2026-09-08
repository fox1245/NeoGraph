# Child synthesis recovery qualification

N3 qualifies the reviewed-source, single-child checkpoint scenario on SQLite and
PostgreSQL. Recovery now distinguishes a recorded child operation from an
unclassified external effect, while retaining explicit reconciliation when a
Core dispatch has no durable checkpoint or result.

## Recovery rules

The parent command journal must match the admitted Program, command ordinal,
payload, and effect identity before recovery considers dispatch. Only a `spawn`
or an `await` chain ending in a synthesized `spawn` can take this recovery path.
The host revalidates the grant, original generation, actual compile debit, and
generated binding. A previously recorded dispatch must also match the exact
child run ID and canonical input.

Recovery reuses the command's existing resource reservation and operation debit.
It does not bill the same command again or grant another dynamic compilation.
Child reservation reconstruction includes resources held by the pending command;
that reconstruction changes the process-local accounting view, not the durable
budget. Existing child records and terminal results remain authoritative.

An equal, already-present `Compiling` or `Validating` publication is not evidence
that a new caller owns that work. Only the writer receiving `Published` may start
the claimed stage. Concurrent cold recovery tests synchronize two independent
processes before this publication and require one successful worker, one
conflict, and one semantic validation.

## Process-exit matrix

Each case exits the process without destructors after the selected publication,
then opens fresh Catalog, transition, and checkpoint stores. Both databases run
the same cases.

| Interruption point | Required recovery |
|---|---|
| `Reserved` | Compile once from the existing reservation |
| `Compiling` | Preserve the debit; require reconciliation |
| `Compiled` | Reuse the bundle |
| `Validating` | Preserve the debit; require reconciliation |
| `Validated` | Reuse semantic evidence |
| `Admitting` | Reuse frozen admission |
| `Admitted` | Bind the admitted version |
| `Bound` | Execute the recorded parent command |
| `Dispatching` | Reuse its child identity and input |
| `Spawned` | Recover the existing child or explicitly reconcile an uncertain Core dispatch |
| Child relation `Publishing` | Finish the existing child's initial publication |
| Child relation `Dispatched` | Reconcile if non-replay-safe Core work has no checkpoint/result |
| Child terminal result | Reuse the result; do not execute the completed child again |
| Parent child-result attachment | Reuse the existing join result |
| Parent command result | Replay the recorded command result |
| Parent terminal result | Return the same terminal result identity |

These are 16 process-exit boundaries per database, plus one two-process recovery
race per database. Tests also verify the original child identity, one durable
child relation, the single compile debit, and retained command-operation counts.
Execution markers verify that completed child work is not repeated across a
process exit.

## Uncertain child execution

A `Dispatched` relation alone cannot establish whether Core work ran before the
process disappeared. If the child is still recorded as running, has no exact
checkpoint, and its plan is not safe to replay without one, parent reconnect
fails closed. `recover_child_synthesis` records `ReconciliationRequired` with
`P_CHILD_SYNTHESIS_CHILD_UNCERTAIN`, retaining the child ID and input.

This inactive-parent recovery check does not misclassify a reconnect to a live
parent as a lost execution. Live reconnect returns the existing attempt.

The uncertainty disposition applies even when the synthesis record already says `Spawned`. The record
can append that reconciliation disposition without changing earlier artifacts.
It does not mint a replacement child or fabricate a successful result. No
automatic reconciliation approval API is supplied by this change.

## Additional checks

- A lost reservation acknowledgement followed by unreadable synthesis history
  cannot refund the compile unit. Once reads recover, the original request is
  reused.
- PostgreSQL tests terminate the database connection from inside the reservation
  transaction. A fresh connection observes the original parent and no partial
  reservation; the subsequent attempt debits once.
- Cancellation during semantic validation cannot reach admission or child
  dispatch.
- Catalog activation cannot redirect a generated binding to another version.
- Retention pins preserve the required versions. If a host removes the child
  version, cached synthesis evidence cannot execute it. Automatic collection of
  retention roots from all synthesis histories remains a host responsibility.
- Program replacement retains the compile debit and does not transfer the old
  run's unattached generated binding into the successor generation. Attached
  descendants can now be retained through the
  [recursive Harness contract](PROGRAM_RECURSIVE_HARNESSES.md).
- PostgreSQL tests share the existing CTest database resource lock, including
  the process-exit matrix and connection-termination case.

## Limits of this qualification

This is process and connection failure qualification for the stated vertical
scenario. It is not a claim of universal exactly-once external effects, power-loss
qualification, database failover across hosts, or automatic resolution of
uncertain Core/provider outcomes. General structured joins containing mixed host
effects retain their existing reconciliation behavior. Multiple generated child
trees, cancellation during arbitrary external validators, full graph-migration
combinations, automatic retention-root discovery, and synthesis performance or
storage-growth qualification require separate coverage.

See [the persistence contract](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md) for host
integration and [the authorization contract](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)
for the unchanged reviewed-source and authority boundary.
