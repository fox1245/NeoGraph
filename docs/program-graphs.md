# Program graph features

**Languages:** [English](program-graphs.md) | [한국어](program-graphs.ko.md) | [日本語](program-graphs.ja.md) | [简体中文](program-graphs.zh-CN.md)

This page describes graph features a `define()` program can use beyond nodes, edges, barriers and the graph-level retry policy. Each feature is a key of the strict Core definition (`schema_version` 1) and has a flag in `javascript_authoring_capability_manifest()["features"]`. A definition that does not use a feature keeps exactly the bytes, `definition_hash` and bundle id it had before the feature existed.

A definition that uses any of these keys seals no validator warnings into its bundle. Warnings are still returned from compilation; see the [diagnostics guide](diagnostics.md).

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
- Two alternatives that arrive in the same super-step, or before the join runs, change nothing. An alternative that arrives after the join ran opens a new round that can never complete, so the run ends `P_CORE_BARRIER_STALLED` (fail-closed). Alternatives should therefore be route-exclusive; `P_CORE_E17` warns when two of them can both run.
- In the Core definition a slot prints as `{"any_of": [...]}` with its names sorted by UTF-8 bytes, and slots are ordered by their smallest name. A slot with one name prints as that name, so `[{any_of: ["B"]}, "C"]` has the same bytes and ids as `["B", "C"]`.
- A stalled slot join lists slots in its witness: `wait_for` and `missing` hold names and `{"any_of": [...]}` objects.
- The validator checks each slot: `P_CORE_E8` when no alternative routes into the join, and `P_CORE_E15` when a member can route elsewhere; a route to another alternative of the same slot is not a bypass.

Errors: `P_JS_GRAPH_ARGUMENT` for an entry of another shape, and `P_CORE_GC_BARRIER` for an empty or non-string `any_of`, a key next to `any_of`, a nested `any_of`, or a node repeated within or across slots.

## Witness paths and the route table

- **Bypass paths.** A `P_CORE_E15` witness has `paths`: for each bypass route, the route sequence from the member through it, ending at the join when it comes back. When it comes back, the node before the join can stand in for the member: the witness adds `substitute` and `suggested_wait_for`, and the returned diagnostic suggests replacing `wait_for` with it (applicability `maybe`). For the #357 shape the suggestion is `[{"any_of": ["B", "FIX"]}, "C"]`. A node that already fills another slot is never suggested.
- **Stall paths.** A stalled join's witness (`output["_neograph"]["barrier_stalled"]`, and `stalled_barriers` in a Program failure) adds `paths`, the routes from each missing member that ran to a node that signaled the join, taken from the trace and the routed successor map, and the same `suggested_wait_for` when one exists.
- **Route table.** `ProgramCompilerConfig::echo_routes` (Python: `ProgramCompiler(..., echo_routes=True)`) returns a `P_CORE_ROUTES` note whose `witness.routes` maps each node to the targets the scheduler can route it to: the routes of its first conditional edge, or else its static edges. It is never sealed, so the bundle is the same either way.

## Failure routing keys

Flags: `retry_policy.per_node`, `failure_edges` and `failure_exits`. The compiler accepts, checks and seals these keys now; the flags turn on when the runtime that acts on them lands. Until then a node failure fails the run as before.

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

`retryPolicy(node, {...})` lowers to two Core keys: the timing and policy fields go to `node_retry_policies`, and `on_failure` and `error_channel` go to `failure_edges`. `failureExit(node, {code})` writes `failure_exits`. Each key is absent when unused, and the graph-level `retryPolicy({...})` is unchanged.

```json
"node_retry_policies": {"TEST": {"max_retries": 2, "non_retryable": ["app.compile_error"], "max_visits": 3}},
"failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
"failure_exits": {"ABORT": {"code": "plan.user_abort"}}
```

- **Per-node policy.** Only the fields written are stored: `max_retries` 0 to 10, `initial_delay_ms` 10 to 60 000, `backoff_multiplier` 1.0 to 10.0, `max_delay_ms` from `initial_delay_ms` to 300 000, `non_retryable` (at most 16 unique codes), `max_visits` 1 to 100 and `error_text`. `attempt_timeout_ms` is not accepted yet. A bound out of range is `P_CORE_GC_FIELD_VALUE` with `{min, max, actual}`.
- **Codes.** Author codes match `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+`; the `NG.` prefix is reserved. `non_retryable` may also list the classifier's codes (`NG.TaskFailed`, `NG.Timeout`, `NG.SubgraphFailed` and the others in the reference's node failure contract).
- **Failure edges.** At most one per node, sorted by `from`. The target may not be `__start__`, `__end__`, the node itself, a join, or a node without edges or routes unless it is a failure exit (`P_CORE_GC_FAILURE_EDGE`). `error_channel` must use the `append` reducer.
- **Failure exits.** The code defaults to `NG.Aborted`. A failure-exit node has no edges or routes.
- **Validation.** Unknown nodes and channels are `P_CORE_E3`. Failure edges join the routed successor map, so reachability, termination and barrier bypass (`P_CORE_E15`, route `on_failure`) see them, and an `error_channel` counts as written. A `retry_policy` key inside node config that the node type does not declare is reported as `P_CORE_E18`. Writing `on_failure` in the graph-level `retryPolicy({...})` is `P_CORE_GC_UNKNOWN_FIELD` with a hint to the per-node form.
- **Migration.** `failure_edges` and `failure_exits` are part of a migration adapter's topology shape; `node_retry_policies` is not.

## Soundness check

Flag: `diagnostics.soundness_check`. `ProgramCompilerConfig::soundness_check` (Python: `ProgramCompiler(..., soundness_check=True)` or `LocalProgramHost(..., soundness_check=True)`) explores every run of the root graph that its routes allow and returns what it finds. It is off by default and, like the route table, never sealed: the bundle is the same either way. A C++ caller of `GraphValidator` sets `ValidationOptions::soundness_check`.

The check takes super-steps the way the scheduler does. A state is the set of ready nodes, the signals each join holds (`any_of` slots included) and the visit count of each node with `max_visits`. A conditional edge may take any of its routes, a node with a failure edge may take that edge instead, and a failure exit ends the run. Each finding is a warning whose `witness.trace` is the shortest run that shows it, one entry per super-step: the nodes that ran (`ready`) and the routes they took (`routes`, with `on_failure` for a failure edge).

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
