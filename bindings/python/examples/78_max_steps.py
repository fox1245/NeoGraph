"""78 — max_steps stops a loop and says so.

A graph with a cycle can run for ever if its exit condition never holds. `RunConfig(max_steps=)`
(default 50) caps the number of super-steps. When the cap stops a run that still has work
ready, `run()` does not raise: it returns the state reached so far, with
`max_steps_exhausted == True` and `status == RunStatus.StepLimit`. A run that finishes on its
own, even on the last allowed step, is not flagged.

The loop is `count -> count` while the `counter` channel is below the `target` input.

Run:
    pip install neograph-engine
    python 78_max_steps.py

smoke-expect: target=1000 max_steps=5: steps run: 5, max_steps_exhausted: True, status: StepLimit, counter: 5
smoke-expect: target=3 max_steps=5: steps run: 3, max_steps_exhausted: False, status: Completed, counter: 3
smoke-expect: target=5 max_steps=5: steps run: 5, max_steps_exhausted: False, status: Completed, counter: 5
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng


@ng.node("count")
def count(state):
    return [ng.ChannelWrite("counter", (state.get("counter") or 0) + 1)]


# The condition decides between another lap and the exit.
ng.ConditionRegistry.register_condition(
    "below_target",
    lambda state: "again" if (state.get("counter") or 0) < state.get("target") else "done",
)

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "max_steps",
    "channels": {"counter": {"reducer": "overwrite"}, "target": {"reducer": "overwrite"}},
    "nodes": {"count": {"type": "count"}},
    "edges": [
        {"from": ng.START_NODE, "to": "count"},
        {"from": "count", "condition": "below_target", "routes": {"again": "count", "done": ng.END_NODE}},
    ],
}
engine = ng.GraphEngine.compile(definition, ng.NodeContext())

for target, max_steps in [(1000, 5), (3, 5), (5, 5)]:
    result = engine.run(ng.RunConfig(input={"target": target}, max_steps=max_steps))
    print(f"target={target} max_steps={max_steps}: steps run: {len(result.execution_trace)}, "
          f"max_steps_exhausted: {result.max_steps_exhausted}, status: {result.status.name}, "
          f"counter: {result.output['channels']['counter']['value']}")
