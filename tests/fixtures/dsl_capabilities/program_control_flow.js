export function define() {
    const graph = ng.graph("capability");
    graph.node("work", {type: "probe.node"});
    graph.entry("work");
    graph.exit("work");
    return graph;
}

export function* main(input) {
    for (let attempt = 0; attempt < 2; ++attempt) {
        const review = yield ng.callCore("capability", {task: input.task, attempt}, `review:${attempt}`);
        if (review.accepted) return {attempts: attempt + 1};
    }
    return {attempts: 2};
}
