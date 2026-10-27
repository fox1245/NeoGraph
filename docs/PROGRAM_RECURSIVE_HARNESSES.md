# Recursive agents with independent Harness generations

**Languages:** [English](PROGRAM_RECURSIVE_HARNESSES.md) | [한국어](PROGRAM_RECURSIVE_HARNESSES.ko.md) | [日本語](PROGRAM_RECURSIVE_HARNESSES.ja.md) | [简体中文](PROGRAM_RECURSIVE_HARNESSES.zh-CN.md)

The reference scenario runs one logical session as a parent/child/grandchild tree
under one owner. Each agent has its own ProgramVersion, Core topology, state,
budget, and lineage. A child can propose JavaScript for its own child through the
same host-owned synthesis boundary used by the main orchestrator.

The reference test executes four distinct Core plans: one node in the main
Harness, two in the original child, three in the replacement child, and four in
the grandchild. The child is replaced while its grandchild is still alive. The
main orchestrator receives the replacement child's result through its existing
await. SQLite and PostgreSQL process-exit tests reopen the complete tree after
the nested replacement commit and finish without rerunning completed work.

## Identity and storage

`ProgramRunRecord::logical_run_id()` and `ProgramHandle::logical_run_id()` expose
the stable agent identity. Each replacement still creates a distinct physical
run and immutable ProgramVersion. The lineage selects the active generation.
Use `reconnect(owner, logical_run_id)` to obtain a handle for that generation.

Run-record schema 4 carries `logical_run_id` when it differs from the
physical run. Child relations continue to address their original child identity;
they retain the first admitted link and invocation. When a replacement supplies
the terminal result, the relation records `terminal_generation` alongside that
result's actual run/version identity. The transition backend checks this against
the committed child lineage, generation, and terminal run record.

Schema 2 and 3 records remain readable, and ordinary records without the new
fields keep their schema 3 representation.

## Replacement with descendants

A live family replacement uses the owning runtime and a held, completed
top-level checkpoint in that agent's generator. This checkpoint can belong to a
nested agent. The target must already be admitted.

The same transaction publishes the new generation and copies the existing child
relations and committed descendant budget. Parent identity and absolute child
depth cannot change. The target must retain the children's capability/effect
grants and guarantee floor. Nested replacement must preserve the parent's result
contract. Retained children require unique binding names; an interrupted child
requires an explicit disposition before the parent can be replaced.

The old execution retires without emitting a logical-agent terminal hook or
cancelling transferred children. Logical child-concurrency and quota cleanup
move to the successor; attempt-specific cleanup still runs. A parent already
waiting on the child follows its successor result, and cancellation through that
child handle follows the same chain. Root-generation handles retain their
existing generation semantics; use the returned replacement handle or reconnect
the logical identity for the active root.

Only children present in the immutable generation-creation publication are
inherited bindings. Existing children can be rejoined through an exact matching
binding and input:

```javascript
// Original child Harness:
const worker = yield ng.spawn("grandchild", {}, "child:spawn");
yield ng.checkpoint(
  {child: worker.child_run_id, replacement: "child-v2.json"},
  "child:swap"
);
```

```javascript
// Replacement Harness: this resolves the inherited child invocation.
const result = yield ng.await(
  ng.spawn("grandchild", {}, "replacement:join"),
  30000,
  "replacement:await"
);
return {generation: 2, result};
```

The replacement does not recreate that child, rewrite its invocation, or acquire
another child grant. Unattached synthesis bindings from the old generation do
not become new authority. Fresh bindings still use the normal synthesis and
admission path. Source proposals must match independently reviewed host template
instances; the reference host does not approve a source merely because an agent
included it in a checkpoint.

## Recovering pre-admitted children after process loss

A host that restores its admitted child bindings can explicitly allow unfinished
JavaScript `spawn`/`await` commands to reconnect to already-published children:

```cpp
config.recover_existing_child_commands = true; // default: false
```

This works for ordinary static bindings and children inherited through a
replacement, without a synthesis gateway. The parent relation, link receipt,
owner, exact invocation, child depth, and persisted child run must agree. Static
commands must derive the same child ID from their recorded command coordinate;
inherited children retain the ID admitted in the generation-creation
publication. The historical Program versions and host grants must still be
available. A configured host admission resolver continues to admit the child
attempt normally.

Recovery uses a dedicated reconnect path: a child that disappears after the
check cannot fall through to new child creation. Missing runs, changed inputs,
or unavailable bindings leave the existing command pending for reconciliation.
The command reservation and descendant accounting survive recovery. The flag
does not authorize replay of unknown provider, tool, native, or other external
effects.

Independently of this flag, replay of a running attempt completes an unfinished
sealed `ng.checkpoint` from its exact journaled value and original command
reservation. It performs no external dispatch.
Other unknown effects still pause the family for reconciliation: an
interrupted child remains a dispatched, resumable relation, and its parent
retains the in-flight command reservation instead of cancelling the relation
or crediting that reservation twice.

If result publication fails while storage remains available, the dispatched
command's outcome and pending effect are both published as `Ambiguous`;
cancellation cannot erase that uncertainty. Optimistic command-head read
collisions are retried, while a permanent rejection against an unchanged parent
head cannot block child
completion notifications indefinitely.

## Answering an interrupted child

A child Harness that pauses on an interrupt (a node of its Core graph raises `NodeInterrupt`, or the graph declares `interruptBefore`) is a resumable run of its own. There is one way to answer it: resume the child by its run id, `ProgramRuntime::resume(owner, child_run_id, ...)` (Python `host.resume(child_run_id, value)`). The id is in the parent's durable relation, `ProgramRunRecord::children()` (Python `host.run_record(parent_run_id).children`), where the paused child is `Dispatched` and carries its `Interrupted` result. This holds for a child that the host started with `start_child`, for one that `main()` spawned and does not await, and for one that `main()` awaits.

A parent whose `main()` awaits the child (`ng.await(ng.spawn(...))`) ends `Interrupted`, never `Failed`: its result shows the child's question, the relation stays `Dispatched`, and the in-flight reservation of the await stays with the parent. A run releases the reservation of its pending command only when it stops in a Core call of that command, at a checkpoint the call wrote; an await of a child waits on the child's run and keeps it. The question is the child's, so resuming the parent (`resume`) or reconciling it (`reconcile`) is refused with `P_CHILD_INTERRUPTED`; the witness `child_run_ids` names the children to resume instead, and nothing is consumed.

When the child ends after its answer, the runtime continues the parent. A new attempt replays `main()` from the command journal, rejoins the child it already dispatched (the relation becomes `Completed`), and the await receives the child's result as if it had never paused. The attempt rejoins the child with or without `recover_existing_child_commands`, in this process or after a restart: the parent's relation already holds the result of a child that paused, and rejoining a child that ran dispatches nothing. That flag stays the opt-in for a child whose dispatch was not recorded. The continuation spends what the journal holds for the run, its remaining budget and the retained reservation, and never a new grant. It repeats up the tree: the answer of a grandchild continues the child, and the end of the child continues the root. The continuation is asynchronous; a caller that wants the parent's result waits for its record to end and then reads it with `reconnect`. A host that restarts, and finds a parent whose child ended while no process continued it, starts the continuation by calling `reconnect(parent_run_id)` (or `recover_children(parent_run_id)`, which also returns the child's handle and never dispatches the child again). The continuation is published once: the child's end, a `reconnect` and another process can all reach the paused parent, one publication decides, and the others leave the run to it. A continuation that cannot start (the store refuses the publication, a limit is exhausted) leaves the parent `Interrupted`: the handle of the parent in the process that held it carries the notice `P_CHILD_CONTINUATION_FAILED`, and `reconnect` of the parent reports the cause. A child that pauses again after its answer stays resumable in the same way.

The deadline of an await (`timeout_ms`) is absolute from the first start of the await, as for every recovered await: an answer that arrives after it ends the parent `TimedOut` with `P_AWAIT_TIMEOUT`, so give an await that waits for a person a timeout that covers the wait. Cancelling a parent, or any ancestor, while its child waits cancels the child and its descendants durably: through their live controls where this process executes them, else from their records. A descendant that another process resumed and still runs is ended by its `Cancelled` record, and that process finds its publications refused.

Only an `ng.await(ng.spawn(...))` (or the awaited spawn alone) is rejoined. A child awaited as a member of a join (`ng.all`, `ng.race`, `ng.quorum`) is not: after its answer the parent ends `Interrupted` with its pending `javascript` command as the effect to reconcile, as when a child binding is unavailable. A typed plan's `await` of a `spawn` is continued the same way; the plan replays its operations when it continues and they count against its Program operation budget again, so declare room for them.

## JSON artifacts and accounting

JSON is a serialized, validated Program bundle. Loading a different JSON selects
a candidate immutable version; changing the file does not mutate a live engine.
The host admits the version and switches the agent's generation at the held
checkpoint. Generator heap/stack state is not transplanted: application state
crosses through the explicit `handoff` JSON, while child relationships remain in
the durable tree.

The example's replacement bundle is compiled and admitted before the session
starts. The main and child agents propose their descendant JavaScript during execution,
and those compilations consume their respective grants. Preparing a newly
generated self-replacement remains a separate compilation/admission operation;
the `replace` API itself selects an already-admitted target.

Delegated compile budget is unavailable for another parent synthesis request.
Replacement transfers the exact remaining budget and committed descendants.
Child recovery also retains its first-attempt deadline. The SQLite Catalog uses
a bounded busy timeout, so it can share a database with concurrent agent
publications, as the transition store does.

## Running the reference

Build Program and QuickJS; enable both persistence backends for their cases.
Run `neograph_program_tests --gtest_filter="*Recursive*"`.
PostgreSQL cases use the existing disposable `NEOGRAPH_TEST_POSTGRES_URL` fixture
and CTest database resource lock.

Set `NEOGRAPH_RECURSIVE_ARTIFACT_DIR` when running the successful topology test to
export `main.json`, `child-v1.json`, `child-v2.json`, `grandchild.json`, and
`session.json`. These use the test registry/compiler identities and are review
fixtures, not standalone production configuration.

The reference covers the three-level tree, a nested checkpoint replacement,
live descendant retention and cancellation, generation-result integrity, budget
attenuation, and whole-session process recovery on SQLite/PostgreSQL. It does
not include an LLM source-generation service, a Python/transport facade, a
cross-owner session sharing policy, or arbitrary in-place Core mutation. Native
Core migration keeps its existing boundary. Independent multi-host live
ownership transfer and every context/hook failure combination are not covered.

## Interactive chatbot example

The [evolving Harness chatbot](../examples/cookbook/self_evolving_chatbot/README.md)
extends the reference with two isolated tenants, an OpenRouter adapter, a browser
inspector, per-turn reviewed-template proposals, runtime successor compilation,
and chat/provider ledgers on SQLite or PostgreSQL. Its assistant can synthesize a
reviewer after replacement, while the original orchestrator continues to await
the same logical assistant.

`RuntimeConfig::checkpoint_handler` lets a host enqueue a handle and move-only
checkpoint lease after durable publication. Retaining the lease pauses the
generator without blocking a scheduler thread. The callback must return promptly;
perform compilation and replacement on the host's worker. On reconnect it also
observes replay of the latest completed checkpoint. An explicit `next_handoff`
request takes precedence.

`ProgramRuntime::reserve_synthesis` debits one unallocated dynamic compile against
the expected lineage head and refreshes that held lease's journal reference.
The checkpoint identity and serialized handoff value remain unchanged. A stale
head, foreign owner/runtime, expired wall budget, or compile budget allocated to
descendants is rejected. The host must persist intent before reservation and
record outcomes; an ambiguous acknowledgement cannot authorize a free retry.
The chatbot uses this reservation in the ordinary `ProgramSynthesisGateway`, then
passes the admitted target to `replace`.

The chatbot's template gate is not proof of answer quality, and its model calls
and generator control retain the `Unmanaged` guarantee. Pending provider effects
after process loss require reconciliation instead of automatic redispatch.
