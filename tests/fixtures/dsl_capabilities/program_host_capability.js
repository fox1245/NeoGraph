export function define() {
    const graph = ng.graph("capability");
    graph.node("work", {type: "probe.node"});
    graph.entry("work");
    graph.exit("work");
    return graph;
}

export function* main() {
    return yield ng.hostCapability(7, {operation: "audit"}, "host:audit");
}
