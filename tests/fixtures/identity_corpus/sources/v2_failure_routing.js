// #364: the design example's failure routing keys. retryPolicy(node, ...)
// lowers to node_retry_policies and failure_edges; failureExit writes
// failure_exits. The graph-level retry policy is separate and unchanged.
export function define() {
    const graph = ng.graph("fix");
    graph.channel("failures", {reducer: "append", initial: []});
    for (const name of ["IMPLEMENT", "TEST", "ASK_USER", "ABORT"]) graph.node(name, {type: "probe.node"});
    graph.entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST");
    graph.conditionalEdge("ASK_USER", "probe.route", {retry: "IMPLEMENT", abort: "ABORT"});
    graph.retryPolicy("TEST", {max_retries: 2, non_retryable: ["app.compile_error", "NG.Timeout"],
                               max_visits: 3, on_failure: "ASK_USER", error_channel: "failures"});
    graph.interruptBefore("ASK_USER");
    graph.failureExit("ABORT", {code: "plan.user_abort"});
    graph.retryPolicy({max_retries: 1, initial_delay_ms: 10, backoff_multiplier: 2, max_delay_ms: 100});
    return graph;
}
