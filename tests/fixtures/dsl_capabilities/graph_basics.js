export function define() {
    const graph = ng.graph("capability");
    graph.channel("value", {reducer: "probe.overwrite", initial: 0});
    for (const name of ["input", "transform", "output"]) {
        graph.node(name, {type: "probe.node", stage: name});
    }
    graph.entry("input");
    graph.edge("input", "transform");
    graph.edge("transform", "output");
    graph.exit("output");
    return graph;
}
