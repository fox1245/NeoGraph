export function define() {
    const graph = ng.graph("model-synthesized");
    graph.channel("value", { reducer: "probe.overwrite", initial: 0 });
    graph.channel("path", { reducer: "probe.overwrite", initial: "" });
    const nodePairs = [["seed", "probe.seed"], ["double", "probe.double"], ["finish", "probe.finish"]];
    for (const [name, type] of nodePairs) {
        graph.node(name, { type });
    }
    graph.entry("seed");
    graph.edge("seed", "double");
    graph.edge("double", "finish");
    graph.exit("finish");
    return graph;
}

export function* main(input) {
    return yield ng.callCore("model-synthesized", input, "model-generated:1");
}
