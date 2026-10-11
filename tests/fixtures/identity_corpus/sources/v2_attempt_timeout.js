// #364 (WP12): an attempt timeout. attempt_timeout_ms lowers into
// node_retry_policies with the other timing fields; programs that do not use
// it keep their identities.
export function define() {
    const graph = ng.graph("timed");
    graph.channel("failures", {reducer: "append", initial: []});
    for (const name of ["FETCH", "FALLBACK"]) graph.node(name, {type: "probe.node"});
    graph.entry("FETCH").exit("FETCH").exit("FALLBACK");
    graph.retryPolicy("FETCH", {attempt_timeout_ms: 30000, max_retries: 2, initial_delay_ms: 100,
                                non_retryable: ["NG.Timeout"], on_failure: "FALLBACK",
                                error_channel: "failures"});
    return graph;
}
