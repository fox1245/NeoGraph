// Send-capable and interrupting node types, a nested subgraph definition in a
// node config, and channels with built-in and registered reducers.
export function define() {
    const inner = {
        schema_version: 1,
        name: "inner",
        channels: {notes: {reducer: "append", initial: []}},
        nodes: {step: {type: "probe.node"}},
        edges: [{from: "__start__", to: "step"}, {from: "step", to: "__end__"}],
    };
    return ng.graph("outer")
        .channel("results", {reducer: "append", initial: []})
        .channel("summary", {reducer: "overwrite", initial: {count: 0, ok: true, tags: ["a", "b"]}})
        .channel("score", {reducer: "probe.overwrite", initial: 0.25})
        .node("fanout", {type: "probe.dynamic-send", targets: ["worker"], limit: 3})
        .node("worker", {type: "probe.node", weight: 1.5, flags: {strict: true}})
        .node("nested", {type: "probe.subgraph", definition: inner,
                         input_map: {results: "notes"}, output_map: {notes: "results"}})
        .node("review", {type: "probe.dynamic-interrupt", prompt: "approve?"})
        .entry("fanout").edge("fanout", "nested").edge("nested", "review")
        .edge("worker", "review")
        .exit("review");
}
