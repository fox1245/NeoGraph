export function define() {
    const graph = ng.graph("capability");
    graph.node("work", {type: "probe.node"});
    graph.entry("work");
    graph.exit("work");
    return graph;
}

export function* main() {
    yield ng.emit({phase: "observed"}, "durable:emit");
    yield ng.checkpoint({cursor: 1}, "durable:checkpoint");
    yield ng.cancelScope("current", "stop", "durable:cancel");
    return {unreachable: true};
}
