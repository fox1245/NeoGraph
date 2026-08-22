export function define() {
    const graph = ng.graph("capability");
    graph.node("dispatch", {type: "probe.dynamic-send"});
    graph.node("approval", {type: "probe.dynamic-interrupt"});
    graph.entry("dispatch");
    graph.edge("dispatch", "approval");
    graph.exit("approval");
    return graph;
}
