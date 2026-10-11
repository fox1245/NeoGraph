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
