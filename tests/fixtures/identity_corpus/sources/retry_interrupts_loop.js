// A retry policy with a fractional multiplier, interrupts before and after
// several nodes, a loop back through a conditional route, a default route and
// a route straight to __end__.
export function define() {
    return ng.graph("loop")
        .channel("attempts", {reducer: "probe.overwrite", initial: 0})
        .node("plan", {type: "probe.node"}).node("work", {type: "probe.node"})
        .node("check", {type: "probe.node"}).node("report", {type: "probe.node"})
        .entry("plan").edge("plan", "work").edge("work", "check")
        .conditionalEdge("check", "probe.more", {again: "work", done: "report",
                                                  stop: "__end__", default: "report"})
        .exit("report")
        .interruptBefore("work", "report")
        .interruptAfter("check")
        .retryPolicy({max_retries: 3, initial_delay_ms: 25, backoff_multiplier: 1.5,
                      max_delay_ms: 400});
}
