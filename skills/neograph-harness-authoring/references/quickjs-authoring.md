# QuickJS source authoring

Use the native capability manifest from this build. It is the authority for API
signatures; registered application nodes and their configuration come from the
host's separate registry description. A general JavaScript library/API is not
automatically available inside QuickJS.

## Source contract

`export function define()` runs at compile time. Construct exactly one graph
with `ng.graph(name)`, use its mutators, then return that builder. A graph JSON
object is an interchange artifact, not a substitute for the builder.

```javascript
export function define() {
  const graph = ng.graph("example");
  graph.channel("value", {reducer: "probe.overwrite", initial: 0});
  graph.node("step", {type: "probe.node"});
  graph.entry("step");
  graph.exit("step");
  return graph;
}
```

This example uses the **capability probe's** registry. Use the actual host's
identifiers elsewhere. In particular, `node(name, {type, ...config})` takes an
object, channel initialization uses `initial`, and reducers are registered names.
Graph mutators return the graph builder, not node handles.

An optional `export function* main(input)` controls runtime work by yielding
sealed `ng` commands. Use the exact Core name returned by `define()`:

```javascript
export function* main(input) {
  const result = yield ng.callCore("example", input, "work:1");
  yield ng.checkpoint({result}, "handoff:1");
  return result;
}
```

Keep source-site labels deterministic. The generator's return must satisfy its
admitted output contract; do not assume its result has the same wrapper as a
declaration-only Core result. Use ordinary JavaScript for pure computation and
yield supported commands for effects. No `require`, ambient network/filesystem,
`eval`, dynamic imports, timers or native handles are granted by this DSL.

## Compile and repair

1. Satisfy the host's requested output envelope. A source-evaluation bridge may
   require exactly `{"source":"...complete module..."}`. Do not add fences.
2. Submit the complete source to the provided compiler bridge. Under Harness MCP,
   discover `neograph_schema`, then use `neograph_compile` with
   `harness.mode="javascript"`, `source_id`, `source`, and the other required
   task/worker/budget/policy fields. Source alone is not a complete MCP request.
3. Read diagnostics by code, path and source site. Repair the complete module
   within the host's existing attempt/compile allowance. Do not replace a
   compiler error with a claim that the code is valid, broaden the registry, or
   start rejected source.
4. Confirm topology/control-flow and output behavior against the acceptance
   criteria, then request admission through the supplied host surface.

In the NeoGraph checkout the deterministic capability bridge is:

```text
program_dsl_capability_probe --manifest
program_dsl_capability_probe graph_basics source.js
```

Build target: `program_dsl_capability_probe`. Its named cases use a fixed test
registry and validate specific capabilities. It is not a compiler for arbitrary
application nodes and its success is not production admission. The repository's
`scripts/run_dsl_capability_eval.ts` feeds model output and compiler diagnostics
through this bridge, with a finite `--repair-attempts` limit.
