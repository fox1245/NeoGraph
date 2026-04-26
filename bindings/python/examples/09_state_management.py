"""09 — State management: get_state and fork.

Demonstrates the LangGraph-style "checkpointer API" the engine
exposes from Python:

  - engine.set_checkpoint_store(store)  — wire persistence in
  - engine.get_state(thread_id)         — latest snapshot or None
  - engine.fork(src, new[, cp_id])      — branch a thread

Each `engine.run(thread_id=...)` saves a checkpoint at the end
(with the store wired). `get_state` then returns that snapshot;
`fork` copies it under a new thread_id so subsequent runs on the
fork don't disturb the source.

(`engine.update_state(...)` is bound but its semantics are subtle
— it merges channel writes into the latest checkpoint via each
channel's reducer. Skipped here to keep the example honest about
what runs predictably.)

Run:
    pip install neograph-engine
    python 09_state_management.py
"""

import neograph_engine as ng


class IncrementNode(ng.GraphNode):
    """count += 1 each run."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        n = state.get("count") or 0
        return [ng.ChannelWrite("count", n + 1)]


ng.NodeFactory.register_type(
    "incr",
    lambda name, config, ctx: IncrementNode(name),
)

definition = {
    "name": "counter",
    "channels": {"count": {"reducer": "overwrite"}},
    "nodes": {"i": {"type": "incr"}},
    "edges": [
        {"from": ng.START_NODE, "to": "i"},
        {"from": "i",           "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())

# get_state / fork need a checkpoint store. The in-memory one is
# the simplest option; SQLite / Postgres backends are NeoGraph-side
# (binding pending).
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())


# Run on thread 'alpha'.
engine.run(ng.RunConfig(thread_id="alpha", input={"count": 10}))

state = engine.get_state("alpha")
count = state["channels"]["count"]["value"]
print(f"alpha after run with count=10: count={count}")  # 11


# Fork alpha → beta. fork() copies the latest checkpoint of `alpha`
# under the new thread_id and returns the copied checkpoint id.
forked_cp_id = engine.fork(
    source_thread_id="alpha",
    new_thread_id="beta",
)
print(f"forked alpha → beta at checkpoint {forked_cp_id}")

beta_state = engine.get_state("beta")
print(f"beta inherits: count={beta_state['channels']['count']['value']}")


# get_state on an unknown thread is None.
print(f"\nunknown thread: {engine.get_state('nonexistent')}")
