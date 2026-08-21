export function define() {
    const graph = ng.graph("main");
    graph.channel("value", {reducer: "semantic.overwrite", initial: ""});
    graph.node("work", {type: "semantic.short-blocking", migration_epoch: 2});
    graph.node("followup", {type: "semantic.followup"});
    graph.entry("work");
    graph.edge("work", "followup");
    graph.exit("followup");
    return graph;
}
