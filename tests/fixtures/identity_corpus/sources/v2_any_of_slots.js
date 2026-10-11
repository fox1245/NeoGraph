// #363: the #357 shape with an any_of slot. B routes "pass" to the join D and
// "fail" to FIX; FIX stands in for B, so either route fills the slot. The
// alternatives are written out of order; the sealed slot sorts them, and the
// slots are ordered by their smallest name.
export function define() {
    const graph = ng.graph("review");
    for (const name of ["A", "B", "C", "FIX", "D"]) graph.node(name, {type: "probe.node"});
    graph.entry("A");
    graph.edge("A", "B").edge("A", "C");
    graph.conditionalEdge("B", "probe.check", {pass: "D", fail: "FIX"});
    graph.edge("FIX", "D").edge("C", "D");
    graph.barrier("D", ["C", {any_of: ["FIX", "B"]}]);
    graph.exit("D");
    return graph;
}
