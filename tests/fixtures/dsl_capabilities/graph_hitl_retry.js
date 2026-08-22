export function define() {
    const graph = ng.graph("capability");
    graph.node("work", {type: "probe.node"});
    graph.entry("work");
    graph.exit("work");
    graph.interruptBefore("work");
    graph.interruptAfter("work");
    graph.retryPolicy({
        max_retries: 2,
        initial_delay_ms: 10,
        backoff_multiplier: 2,
        max_delay_ms: 100,
    });
    return graph;
}
