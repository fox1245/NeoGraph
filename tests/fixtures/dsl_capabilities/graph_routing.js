export function define() {
    const graph = ng.graph("capability");
    for (const name of ["router", "left", "right"]) {
        graph.node(name, {type: "probe.node"});
    }
    graph.entry("router");
    graph.conditionalEdge("router", "probe.route", {left: "left", right: "right"});
    graph.exit("left");
    graph.exit("right");
    return graph;
}
