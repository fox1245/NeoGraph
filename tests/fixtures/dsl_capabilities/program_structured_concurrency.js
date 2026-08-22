export function define() {
    const graph = ng.graph("capability");
    graph.node("work", {type: "probe.node"});
    graph.entry("work");
    graph.exit("work");
    return graph;
}

function calls(site, count = 2) {
    return Array.from({length: count}, (_, index) =>
        ng.callCore("capability", {index}, `${site}:${index}`));
}

export function* main() {
    yield ng.all(calls("all"), {max_in_flight: 2}, "all");
    yield ng.parallel(calls("parallel"), {max_in_flight: 2}, "parallel");
    yield ng.join(calls("join"), "all", {max_in_flight: 2}, "join");
    yield ng.race(calls("race"), {max_in_flight: 2}, "race");
    yield ng.quorum(calls("quorum", 3), {required_successes: 2, max_in_flight: 3}, "quorum");
    return {completed: true};
}
