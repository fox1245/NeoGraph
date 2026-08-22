export function define() {
    const graph = ng.graph("capability");
    for (const name of ["split", "left", "right", "join"]) {
        graph.node(name, {type: "probe.node"});
    }
    graph.entry("split");
    graph.edge("split", "left");
    graph.edge("split", "right");
    graph.edge("left", "join");
    graph.edge("right", "join");
    graph.barrier("join", ["left", "right"]);
    graph.exit("join");
    return graph;
}
