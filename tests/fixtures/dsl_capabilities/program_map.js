export function define() {
    const graph = ng.graph("capability");
    graph.node("work", {type: "probe.node"});
    graph.entry("work");
    graph.exit("work");
    return graph;
}

export function* main(input) {
    const commands = input.items.map((item, index) =>
        ng.callCore("capability", {item}, `map:${index}`));
    const results = yield ng.all(commands, {max_in_flight: 2}, "map:all");
    return {count: results.length};
}
