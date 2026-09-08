# Recursive agents with independent Harness generations

The verified scenario runs one logical session as a parent/child/grandchild tree
under one owner. Each agent has its own ProgramVersion, Core topology, state,
budget, and lineage. A child can propose the DSL for its own child through the
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

New run-record schema 4 carries `logical_run_id` when it differs from the
physical run. Child relations continue to address their original child identity;
they retain the first admitted link and invocation. When a replacement supplies
the terminal result, the relation records `terminal_generation` alongside that
result's actual run/version identity. The transition backend checks this against
the committed child lineage, generation, and terminal run record.

Existing schema 2/3 records remain readable and ordinary records without the new
fields retain their schema 3 representation. Do not mix old Program binaries
with the new C++ record layout; rebuild Program consumers together.

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

## JSON artifacts and accounting

JSON is a serialized, validated Program bundle. Loading a different JSON selects
a candidate immutable version; changing the file does not mutate a live engine.
The host admits the version and switches the agent's generation at the held
checkpoint. Generator heap/stack state is not transplanted: application state
crosses through the explicit `handoff` JSON, while child relationships remain in
the durable tree.

The example's replacement bundle is compiled and admitted before the session
starts. The main and child agents propose their descendant DSL during execution,
and those compilations consume their respective grants. Preparing a newly
generated self-replacement remains a separate compilation/admission operation;
the `replace` API itself selects an already-admitted target.

Delegated compile budget is unavailable for another parent synthesis request.
Replacement transfers the exact remaining budget and committed descendants.
Child recovery also retains its first-attempt deadline. The SQLite Catalog now
uses a bounded busy timeout, matching the transition store's ability to share a
database with concurrent agent publications.

## Running the reference

Build Program and QuickJS; enable both persistence backends for their cases.
Run `neograph_program_tests --gtest_filter="*Recursive*"`.
PostgreSQL cases use the existing disposable `NEOGRAPH_TEST_POSTGRES_URL` fixture
and CTest database resource lock. Native WSL Docker is suitable for this setup.

Set `NEOGRAPH_RECURSIVE_ARTIFACT_DIR` when running the successful topology test to
export `main.json`, `child-v1.json`, `child-v2.json`, `grandchild.json`, and
`session.json`. These use the test registry/compiler identities and are review
fixtures, not standalone production configuration.

This qualification covers the three-level tree, a nested checkpoint replacement,
live descendant retention and cancellation, generation-result integrity, budget
attenuation, and whole-session process recovery on SQLite/PostgreSQL. It does
not introduce an LLM source-generation service, a Python/transport facade, a
cross-owner session sharing policy, or arbitrary in-place Core mutation. Native
Core migration retains its existing boundary. Independent multi-host live
ownership transfer and every context/hook failure combination require separate
qualification.

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
