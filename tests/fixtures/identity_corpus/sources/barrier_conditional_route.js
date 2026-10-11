// The #357 shape: B reaches the joined exit D directly (pass) or through FIX
// (fail), and D waits for B and C. Pins the legacy barrier bytes that an
// equivalent any_of spec must keep.
export function define() {
    return ng.graph("plan")
        .channel("trace", {reducer: "append", initial: []})
        .node("A", {type: "probe.node"}).node("B", {type: "probe.node"})
        .node("C", {type: "probe.node"}).node("D", {type: "probe.node"})
        .node("FIX", {type: "probe.node"})
        .entry("A").edge("A", "B").edge("A", "C")
        .conditionalEdge("B", "probe.check", {pass: "D", fail: "FIX"})
        .edge("FIX", "D").edge("C", "D")
        .barrier("D", ["C", "B"])
        .exit("D");
}
