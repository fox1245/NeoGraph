// Node names outside ASCII and outside the BMP. UTF-16 code-unit order puts
// U+1F600 before U+FF5A; UTF-8 byte order puts it after. The sealed barrier
// and the object keys show which order the identity uses today.
export function define() {
    const graph = ng.graph("unicode");
    const members = ["\u{1F600}", "ｚ", "é", "z", "Z"];
    graph.node("start", {type: "probe.node"});
    graph.node("join", {type: "probe.node"});
    for (const name of members) {
        graph.node(name, {type: "probe.node", label: name});
        graph.edge("start", name).edge(name, "join");
    }
    graph.entry("start");
    graph.barrier("join", members);
    graph.exit("join");
    return graph;
}
