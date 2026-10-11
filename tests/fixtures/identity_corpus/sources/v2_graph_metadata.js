// #362: graph metadata. Keys are written out of order and include a non-ASCII
// value; the sealed definition sorts them by UTF-8 bytes. "orphan" is
// unreachable, so the current rules warn (E7), but a definition that uses a v2
// key seals no validator warnings.
export function define() {
    const graph = ng.graph("plan");
    graph.metadata({
        requirements: ["REQ-12", "REQ-7"],
        owner: "agent:planner",
        limits: {max_depth: 2, max_total: 4},
        note: "검토 필요",
    });
    graph.node("draft", {type: "probe.node"});
    graph.node("orphan", {type: "probe.node"});
    graph.entry("draft");
    graph.exit("draft");
    return graph;
}
