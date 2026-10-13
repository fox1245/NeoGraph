# Program graph features

**Languages:** [English](program-graphs.md) | [한국어](program-graphs.ko.md) | [日本語](program-graphs.ja.md) | [简体中文](program-graphs.zh-CN.md)

This page describes graph features a `define()` program can use beyond nodes, edges, barriers and the graph-level retry policy. Each feature is a key of the strict Core definition (`schema_version` 1) and has a flag in `javascript_authoring_capability_manifest()["features"]`. A definition that does not use a feature keeps exactly the bytes, `definition_hash` and bundle id it had before the feature existed.

A definition that uses any of these keys seals no validator warnings into its bundle. Warnings are still returned from compilation; see the [diagnostics guide](diagnostics.md).

## Authoring rules

A Program is one JavaScript module that the host compiles. The rules below are checked when the module is compiled or run, and a violation is reported with the diagnostic named in each item (see the [diagnostics guide](diagnostics.md)).

- **Module shape.** The module exports a synchronous `define()` that returns exactly one graph builder (`P_JS_DEFINE_MISSING`, `P_JS_DEFINE_ASYNC`, `P_JS_DEFINE_VALUE`). A module that exports only `main()` is rejected. `main()` is optional; when exported it must be a synchronous generator (`P_JS_CONTROL_MAIN`, `P_JS_CONTROL_ASYNC`). Without it, a run executes the graph directly. A module with a top-level `await` is rejected.
- **Inside `define()`.** Ordinary deterministic JavaScript (loops, helper functions, template strings) builds the graph through `ng.graph()`. No command can be yielded and no node runs. Builder arguments must be canonical JSON data (`P_JS_GRAPH_VALUE`), and the builder cannot be used after `define()` returns (`P_JS_GRAPH_SEALED`).
- **Inside `main()`.** Loops, recursion, closures, exceptions and local state work as usual, but `main()` acts only by yielding commands built with `ng` (`ng.callCore`, `ng.all` and the others). The runtime checks each command, records it in the journal, runs it and resumes the generator with the result. Source code requests an action and never grants itself one: every node type, condition and reducer must be in the host's registry, and the host's budgets bound the run.
- **Rejected.** `Date`, `eval` and the `Function` constructors fail with `P_JS_AMBIENT_DENIED`. `Math.random()` always returns 0, so a Program has no randomness. The globals `std`, `os`, `process`, `require`, `fetch`, `crypto`, `performance`, the timer functions, `Worker`, `SharedArrayBuffer`, `Atomics`, `WebAssembly`, `WeakRef` and `FinalizationRegistry` are undefined. Dynamic `import()` and `require()` fail with `P_JS_IMPORT_DYNAMIC`, and a static `import` must name a module that was sealed together with the source (`P_JS_IMPORT_UNAVAILABLE`).
- **Not source languages.** JavaScript is the only language a Program is written in. The strict Core JSON that `define()` produces, bundles, journals and records are stored artifacts, and NeoGraph never accepts them as Program source. The older Core DSL and Program JSON operation trees have been removed.

## Graph metadata

Flag: `graph.metadata`.

```js
export function define() {
  return ng.graph("plan")
    .metadata({owner: "agent:planner", requirements: ["REQ-12"]})
    .node("draft", {type: "my.task"})
    .entry("draft").exit("draft");
}
```

`.metadata(object)` may be called once per graph. The Core definition stores it as a top-level `"metadata"` object, and an empty object is the same as none.

- **Limits.** At most 8 KiB of canonical JSON, nesting depth 4 below `metadata`, 64 object members in total, and keys of 1 to 128 bytes. Top-level keys starting with `neograph.` are reserved.
- **Text.** Keys and strings may not contain invisible characters: C0 controls other than tab, newline and carriage return; the bidi controls U+061C, U+200E, U+200F, U+202A to U+202E and U+2066 to U+2069; the zero-width characters U+200B to U+200D, U+2060 and U+FEFF; and the tag characters U+E0000 to U+E007F.
- **Identity.** Metadata is part of the definition, so it changes `definition_hash` and the bundle id. Key order does not.
- **Readers.** Only hosts read it, from the sealed definition: `bundle.sealed_core_definitions()[0].definition["metadata"]` in C++, or `bundle.sealed_core_definitions[0]["definition"]["metadata"]` in Python, for example on the bundle `LocalProgramHost.compile()` returns. Node factories, run state, checkpoints, outputs and events never see it.
- **Authority.** Metadata records what the author claims. It grants nothing: delegation limits stay in the host-admitted `RunBudget` (`max_child_depth`, `max_total_children`, `max_concurrency`).
- **Migration.** Metadata is outside the topology shape of a `GraphSemanticMigrationAdapter`, so changing it does not block a migration. `metadata_changes()` lists the added, removed and changed pointers for the host that approves it.

Errors: `P_JS_GRAPH_ARGUMENT` for a second call or an argument that is not an object, `P_CORE_GC_FIELD_TYPE`, `P_CORE_GC_METADATA_LIMIT`, `P_CORE_GC_METADATA_TEXT` and `P_CORE_GC_FIELD_VALUE` (an empty or reserved key).

## Join slots with any_of

Flag: `barrier.any_of`.

```js
graph.conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
  .edge("FIX", "D").edge("C", "D")
  .barrier("D", [{any_of: ["B", "FIX"]}, "C"]);
```

An entry of `barrier(node, waitFor)` is a node name or a slot `{any_of: [...]}`. The join is still an AND join over its slots:

- A slot is filled when any of its alternatives signals the join. The join runs when every slot is filled, and then all signals clear, as for a plain barrier.
- Signals from nodes outside the slots are recorded and never count. `Command.goto` and `Send` bypass the join, as before.
- Two alternatives that arrive in the same super-step, or before the join runs, change nothing. An alternative that arrives after the join ran opens a new round. When the join has other slots, that round can never complete, so the run ends `P_CORE_BARRIER_STALLED` (fail-closed). When the join has one slot, the late alternative fills the new round at once and the join runs again. Alternatives should therefore be route-exclusive; `P_CORE_E17` warns when two of them can both run.
- In the Core definition a slot prints as `{"any_of": [...]}` with its names sorted by UTF-8 bytes, and slots are ordered by their smallest name. A slot with one name prints as that name, so `[{any_of: ["B"]}, "C"]` has the same bytes and ids as `["B", "C"]`.
- A stalled slot join lists slots in its witness: `wait_for` and `missing` hold names and `{"any_of": [...]}` objects.
- The validator checks each slot: `P_CORE_E8` when no alternative routes into the join, and `P_CORE_E15` when a member can route elsewhere; a route to another alternative of the same slot is not a bypass.

Errors: `P_JS_GRAPH_ARGUMENT` for an entry of another shape, and `P_CORE_GC_BARRIER` for an empty or non-string `any_of`, a key next to `any_of`, a nested `any_of`, or a node repeated within or across slots.

## Witness paths and the route table

- **Bypass paths.** A `P_CORE_E15` witness has `paths`: for each bypass route, the route sequence from the member through it, ending at the join when it comes back. When it comes back, the node before the join can stand in for the member: the witness adds `substitute` and `suggested_wait_for`, and the returned diagnostic suggests replacing `wait_for` with it (applicability `maybe`). For the #357 shape the suggestion is `[{"any_of": ["B", "FIX"]}, "C"]`. A node that already fills another slot is never suggested.
- **Stall paths.** A stalled join's witness (`output["_neograph"]["barrier_stalled"]`, and `stalled_barriers` in a Program failure) adds `paths`, the routes from each missing member that ran to a node that signaled the join, taken from the trace and the routed successor map, and the same `suggested_wait_for` when one exists.
- **Route table.** `ProgramCompilerConfig::echo_routes` (Python: `ProgramCompiler(..., echo_routes=True)`) returns a `P_CORE_ROUTES` note whose `witness.routes` maps each node to the targets the scheduler can route it to: the routes of its first conditional edge, or else its static edges. It is never sealed, so the bundle is the same either way.

## Failure routing

Flags: `retry_policy.per_node`, `failure_edges`, `failure_exits` and `retry.attempt_timeout`.

```js
ng.graph("fix")
  .channel("failures", {reducer: "append", initial: []})
  .node("IMPLEMENT", {type: "t.task"}).node("TEST", {type: "t.task"})
  .node("ASK_USER", {type: "t.review"}).node("ABORT", {type: "t.task"})
  .entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST")
  .conditionalEdge("ASK_USER", "t.decision", {retry: "IMPLEMENT", abort: "ABORT"})
  .retryPolicy("TEST", {max_retries: 2, non_retryable: ["app.compile_error"], max_visits: 3,
                        on_failure: "ASK_USER", error_channel: "failures"})
  .interruptBefore("ASK_USER")
  .failureExit("ABORT", {code: "plan.user_abort"});
```

TEST is tried three times. If it still fails, the run does not fail: TEST routes to ASK_USER, which pauses for a human with the failure record. Answering `retry` runs IMPLEMENT and TEST again; answering `abort` runs ABORT, a failure exit, so the run ends `Failed` with `plan.user_abort` and never reports `Completed`.

`retryPolicy(node, {...})` lowers to two Core keys: the timing and policy fields go to `node_retry_policies`, and `on_failure` and `error_channel` go to `failure_edges`. `failureExit(node, {code})` writes `failure_exits`. Each key is absent when unused, and the graph-level `retryPolicy({...})` is unchanged.

```json
"node_retry_policies": {"TEST": {"max_retries": 2, "non_retryable": ["app.compile_error"], "max_visits": 3}},
"failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
"failure_exits": {"ABORT": {"code": "plan.user_abort"}}
```

### Keys

- **Per-node policy.** Only the fields written are stored: `max_retries` 0 to 10, `initial_delay_ms` 10 to 60 000, `backoff_multiplier` 1.0 to 10.0, `max_delay_ms` from `initial_delay_ms` to 300 000, `non_retryable` (at most 16 unique codes), `max_visits` 1 to 100, `error_text`, and `attempt_timeout_ms` 100 to 3 600 000. A bound out of range is `P_CORE_GC_FIELD_VALUE` with `{min, max, actual}`.
- **Codes.** Author codes match `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+`; the `NG.` prefix is reserved. `non_retryable` may also list the classifier's codes (`NG.TaskFailed`, `NG.Timeout`, `NG.SubgraphFailed` and the others in the reference's node failure contract).
- **Failure edges.** At most one per node, sorted by `from`. The target may not be `__start__`, `__end__`, the node itself, a join, or a node without edges or routes unless it is a failure exit (`P_CORE_GC_FAILURE_EDGE`). `error_channel` must use the `append` reducer.
- **Failure exits.** The code defaults to `NG.Aborted`. A failure-exit node has no edges or routes.
- **Validation.** Unknown nodes and channels are `P_CORE_E3`. Failure edges join the routed successor map, so reachability, termination and barrier bypass (`P_CORE_E15`, route `on_failure`) see them, and an `error_channel` counts as written. A `retry_policy` key inside node config that the node type does not declare is reported as `P_CORE_E18`. Writing `on_failure` in the graph-level `retryPolicy({...})` is `P_CORE_GC_UNKNOWN_FIELD` with a hint to the per-node form.
- **Migration.** `failure_edges` and `failure_exits` are part of a migration adapter's topology shape; `node_retry_policies` is not.

### At run time

- **Retry timing** is resolved per field: the host default (`EngineConfig::retry_policy`, `set_retry_policy`), otherwise the graph's `retryPolicy`, otherwise the built-in default; then the node's own fields. A host per-node policy (`EngineConfig::node_retry_policies`, `set_node_retry_policy`) replaces the result for a node without a per-node policy, as before. For a node with one, the host may only tighten each field: the smaller `max_retries`, the larger delays and multiplier; such a node never retries more than 10 times. `GraphEngine::resolved_retry_policy(node)` returns the result.
- **What is retried and routed** is the classifier's decision (the reference's node failure contract). A code listed in `non_retryable` is not retried. A Program diagnostic raised inside a node (`ProgramDiagnosticError`, or a compile or admission error) is `NG.ConfigError`: never retried, never routed.
- **A handled failure.** When a node gives up with a routable failure and has a failure edge, the run goes on. The node's failed attempts write nothing; one record is appended to its `error_channel`, when it has one, in the node's super-step; and the node routes to the edge's target only, without signaling its own edges or routes. Siblings in the same super-step apply their results as usual, and a `Command.goto` in that super-step still preempts every route, the failure route included. A failure arrival never fills a join slot; a handler that runs can stand in for the member, as in the #357 shape. The node counts as run for the trace and for `interruptAfter`, and its pending write records the handled outcome, so a resume after a crash replays the route without running the node again. Static nodes, fan-out branches, one or many `Send` targets and subgraph nodes behave the same. A failure that is not routable, or one of a node without a failure edge, fails the run as before.
- **The record** holds structured fields only:
  ```json
  {"node": "TEST", "kind": "exhausted", "code": "NG.TaskFailed", "attempts": 3, "retryable": true,
   "step": 1, "routed_to": "ASK_USER", "effects": []}
  ```
  `kind` is `exhausted`, `non_retryable`, `effect_committed`, `ceiling`, `visit_limit` or `deadline` (a backoff would have crossed the run's deadline). `effects` lists the final attempt's mediated Tool effects as `{tool, ordinal, status}`.
- **Free text** is opt-in per node with `error_text: true`, and needs the host's redactor: `EngineConfig::failure_text_redactor`, `CatalogConfig::failure_text_redactor`, or `failure_text_redactor=` on Python's `GraphEngine.compile` and `LocalProgramHost`. Its only source is a typed public message (the `public_message` of a `NodeFailureError` or a Python exception's `public_message` attribute), never the exception text. The redactor returns the text to keep; the engine strips C0, bidirectional, zero-width and tag characters, cuts the text to 512 bytes on a UTF-8 boundary and stores it as `"detail": {"untrusted_text": "..."}`. Without a redactor no text is written.
- **Interrupts.** When the target has `interruptBefore`, the interrupt value carries `{"reason": "failure_route", "failure": record}`. In a Program it is `result.interrupt.value` and `result.interrupt.pending_input.core_interrupt_value`.
- **Events.** The `ERROR` event of a node that gave up says `handled` (true when the failure was routed) and, when routed, `routed_to`.
- **Loop bound: `max_visits`.** A node that declares it runs at most that many times; its next activation does not run and fails with `NG.VisitLimit` (`kind` `visit_limit`, 0 attempts), routed along the node's failure edge, or failing the run without one. The counter resets when the failure edge's target completes. It lives in checkpoint metadata as `_neograph.node_visits`, only for declaring nodes. The step budget stays the global backstop.
- **Failure exits.** When a failure-exit node completes, the run ends after that super-step with `output["_neograph"]["failure_exit"] = {node, code}` and `RunStatus::FailureExit` (Python `RunStatus.FailureExit`; a `RunInvocation` reports `Error`). A Program run ends `Failed` with the declared code and `core_node` set to that node.
- **Retry ceiling.** `RunBudget::max_node_retries` (Python `ProgramRunBudget.max_node_retries`) caps node retries across the whole run: every Core call, node, branch, `Send` and subgraph. Reaching it stops retrying with `kind` `ceiling`. The retries taken are `ProgramUsage::node_retries`, and what is left is `remaining_budget.max_node_retries`. Absent means no ceiling, and both fields are serialized only when set. A child started by `ng.spawn` inherits its parent's ceiling. Outside a Program, pass a `NodeRetryCeiling` as `RunMetadata::node_retry_ceiling`.
- **Unrouted failures in a Program.** A node failure without a failure edge ends the Program `Failed` with the classified code (`NG.TaskFailed`, an author code, `NG.ProviderFailed`, ...) rather than `P_RUNTIME_CORE_FAILURE`, and `failure.witness.node_failure` holds `{node, code, kind, attempts, retryable, routable}`. The `retry` operation of an orchestration plan asks the same classifier: it runs its body again only after a retryable failure, never after a committed effect, a class that is not retried, a reached retry ceiling or a stop at the deadline.

### Attempt timeouts and the deadline

Flag: `retry.attempt_timeout`.

```js
graph.retryPolicy("TEST", {attempt_timeout_ms: 30000, max_retries: 2, on_failure: "ASK_USER"});
```

- **A timed-out attempt.** `attempt_timeout_ms` limits each attempt of the node. When it passes, the attempt is cancelled through its own cancel token (`ctx.cancel_token` of the node, a child of the run's token) and fails with `NG.Timeout`, which is retried and routable like an ordinary failure; list `NG.Timeout` in `non_retryable` to stop at the first timeout. The first terminal event wins: an attempt that completes at or after its timeout has timed out, whether or not the timer has fired yet.
- **The late completion is discarded.** Whatever a timed-out attempt does afterwards counts for nothing: its writes and result are dropped, the events it emits never reach the run's callback, and a mediated Tool call it starts is refused, because its token is cancelled. The next attempt starts only after the cancelled one has stopped, so two attempts of a node never overlap.
- **The stop deadline.** A cancelled attempt has as long to stop as it had to run, and at least one second. One that has not stopped by then may still be performing effects: the node fails with `NG.EffectUncertain`, which is neither retried nor routed, so the run fails. The attempt goes on in the background and keeps its node alive until it returns; `wait_for_node_attempts()` waits for such attempts, and the Python binding calls it when the interpreter exits. An attempt that stopped after committing a non-idempotent effect fails with `NG.EffectCommitted` as before, and one that left a mediated effect uncertain pauses the run for reconciliation.
- **Every path.** Static nodes, fan-out branches, one or many `Send` targets and subgraph nodes take the same path; a subgraph node's child run is cancelled with its attempt. A crash after timed-out attempts resumes in the attempt that was running, and a resume never applies the result of a timed-out attempt.
- **Host timeouts.** `GraphEngine::set_node_attempt_timeout_ms(node, ms)` (Python `set_node_attempt_timeout_ms`, `None` clears it) sets a timeout for a node whose definition has none. For a node that has one, the host may only shorten it: the shorter of the two applies. `resolved_attempt_timeout_ms(node)` returns the result.
- **Where node bodies run.** The coroutine that drives a run never runs a body that has a timeout, and in a Program run it runs no node body at all: such a body runs on a process-wide worker pool (`NEOGRAPH_NODE_ATTEMPT_THREADS`, 1 to 1024, default 32), so a body that blocks its thread holds neither its own timeout nor the Program scheduler that join members and `await` timers share. This covers C++ nodes and Core subgraphs as well as Python nodes. Other `GraphEngine` runs keep running untimed bodies on the calling thread.
- **Deadline-aware backoff.** When the backoff before the next attempt would cross the run's deadline (a Program's `wall_time_ms`; `RunMetadata::deadline` outside a Program), the node stops retrying at once with `kind` `deadline` instead of sleeping until the budget ends the run. The failure keeps its code (`NG.TaskFailed`, `NG.Timeout`, ...) and is routed along the node's failure edge, or fails the run without one.

## Soundness check

Flag: `diagnostics.soundness_check`. `ProgramCompilerConfig::soundness_check` (Python: `ProgramCompiler(..., soundness_check=True)` or `LocalProgramHost(..., soundness_check=True)`) explores every run of the root graph that its routes allow and returns what it finds. It is off by default and, like the route table, never sealed: the bundle is the same either way. A C++ caller of `GraphValidator` sets `ValidationOptions::soundness_check`.

The check takes super-steps the way the scheduler does. A state is the set of ready nodes, the signals each join holds (`any_of` slots included) and the visit count of each node with `max_visits`, which resets, as at run time, only when that node's failure-edge target completes. A conditional edge may take any of its routes, a node with a failure edge may take that edge instead, and a failure exit ends the run. Each finding is a warning whose `witness.trace` is the shortest run that shows it, one entry per super-step: the nodes that ran (`ready`) and the routes they took (`routes`, with `on_failure` for a failure edge).

| Code | Finding |
|---|---|
| `P_CORE_S1` | From some point no run can complete: every continuation ends barrier-stalled, fails at a visit limit or loops. |
| `P_CORE_S2` | A run ends while a join still holds signals: barrier-stalled, or through an explicit end that discards them. |
| `P_CORE_S3` | A join never fires on any run. |
| `P_CORE_S4` | A run can enter a loop with no exit and then stops only at the step limit. |
| `P_CORE_S5` | Two nodes that write one overwrite channel can run in the same super-step (beyond the fan-out siblings `P_CORE_E5` reports). |

A `P_CORE_SOUNDNESS` summary says how far the check got: a note when it explored every run, a warning when it did not. It never claims what it did not explore:

- A node type that may return `Command.goto` or `Send` declares it in its effect contract, `"control": ["goto"]` or `["send"]`. Runs after such a node is ready are not analyzable: the summary lists those nodes, `P_CORE_S1` and `P_CORE_S4` are reported only where every continuation was explored, and `P_CORE_S3` not at all. The check trusts the contracts, so a node type that returns `Command.goto` without declaring it is analyzed by its declared routes.
- `soundness_state_cap` (default 100 000 states) bounds the work. When the check reaches it, the summary says `not_checked` and only the `P_CORE_S2` and `P_CORE_S5` findings found before it are returned.

For the #357 shape with `barrier("D", ["B", "C"])`:

```python
compiler = ng.ProgramCompiler(registry, soundness_check=True)
bundle = compiler.compile(ng.ProgramSource.from_javascript("plan.js", source))
stall = [d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_S2"][0]
stall["witness"]["trace"]
# [{"ready": ["A"], "routes": {}},
#  {"ready": ["B", "C"], "routes": {"B": "fail"}},
#  {"ready": ["FIX"], "routes": {}}]
stall["witness"]["missing"]             # ["B"]
stall["witness"]["suggested_wait_for"]  # [{"any_of": ["B", "FIX"]}, "C"]
```

`P_CORE_S1` reports the same graph from the point where `B` has taken `fail`. With `barrier("D", [{any_of: ["B", "FIX"]}, "C"])` the check finds nothing, and the summary note says it explored every run. The [diagnostics guide](diagnostics.md#soundness-check) describes each witness.
