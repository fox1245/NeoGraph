// A one-member barrier and a three-member barrier on the same graph. A later
// {any_of: ["left"]} slot must hash the same as the plain "left" member.
export function define() {
    const graph = ng.graph("joins");
    for (const name of ["split", "left", "middle", "right", "gate", "join"])
        graph.node(name, {type: "probe.node"});
    graph.entry("split");
    graph.edge("split", "left").edge("split", "middle").edge("split", "right");
    graph.edge("left", "gate").edge("gate", "join");
    graph.edge("middle", "join").edge("right", "join");
    graph.barrier("gate", ["left"]);
    graph.barrier("join", ["right", "gate", "middle"]);
    graph.exit("join");
    return graph;
}
