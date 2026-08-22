export function define() {
    const graph = ng.graph("capability");
    graph.node("work", {type: "probe.node"});
    graph.entry("work");
    graph.exit("work");
    return graph;
}

export function* main(input) {
    const child = ng.spawn("worker-child", {task: input.task}, "child:spawn");
    return yield ng.await(child, 5000, "child:await");
}
