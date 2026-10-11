// define() plus main(): a bounded loop of callCore, a branch on the result,
// structured concurrency, emit and checkpoint.
export function define() {
    return ng.graph("review")
        .node("A", {type: "probe.node"}).node("B", {type: "probe.node"})
        .node("C", {type: "probe.node"}).node("D", {type: "probe.node"})
        .entry("A").edge("A", "B").edge("A", "C").edge("B", "D").edge("C", "D")
        .barrier("D", ["B", "C"])
        .exit("D");
}

export function* main(input) {
    let last = null;
    for (let round = 0; round < 3; ++round) {
        last = yield ng.callCore("review", {round, task: input.task}, `round:${round}`);
        if (last.accepted) break;
        yield ng.emit({round, accepted: false}, `emit:${round}`);
    }
    const checks = yield ng.race([ng.callCore("review", {probe: 1}, "race:a"),
                                  ng.callCore("review", {probe: 2}, "race:b")], {}, "race");
    yield ng.checkpoint({done: true}, "checkpoint");
    return {last, checks};
}
