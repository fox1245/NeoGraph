"""76 — RetryPolicy: a flaky node is retried with exponential backoff.

A node fails twice and works on the third attempt. A `RetryPolicy` tells the engine to run it
again after a growing pause, so the caller only sees the final result:

    pause before retry n = initial_delay_ms * backoff_multiplier^(n-1), capped at max_delay_ms

The policy here applies to one node (`set_node_retry_policy`); `set_retry_policy` sets a default
for every node. A second run with a smaller budget shows what the caller gets when the retries
run out: the node's own exception, wrapped with the number of attempts.

Run:
    pip install neograph-engine
    python 76_retry_policy.py

smoke-expect: max_retries=3: ok, attempts: 3, answer: done, backoff: 20 ms, 40 ms
smoke-expect: max_retries=1: gave up after 2 attempts, backoff: 20 ms
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng

calls = 0


@ng.node("flaky")
def flaky(state):
    """Fails until it has been called three times."""
    global calls
    calls += 1
    if calls < 3:
        raise RuntimeError("upstream timeout")
    return [ng.ChannelWrite("answer", "done")]


definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "retry_policy",
    "channels": {"answer": {"reducer": "overwrite"}},
    "nodes": {"flaky": {"type": "flaky"}},
    "edges": [
        {"from": ng.START_NODE, "to": "flaky"},
        {"from": "flaky", "to": ng.END_NODE},
    ],
}

for max_retries in (3, 1):
    calls = 0
    policy = ng.RetryPolicy()
    policy.max_retries = max_retries      # retries after the first attempt
    policy.initial_delay_ms = 20          # pause before the first retry
    policy.backoff_multiplier = 2.0       # each pause is twice the last one
    policy.max_delay_ms = 200             # and never longer than this

    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    engine.set_node_retry_policy("flaky", policy)

    # In DEBUG mode every failed attempt that will be retried raises an ERROR event that
    # carries the pause the engine is about to take.
    pauses = []

    def on_event(event):
        if event.type == ng.GraphEvent.Type.ERROR:
            pauses.append(f"{event.data['delay_ms']} ms")

    print(f"max_retries={max_retries}: ", end="")
    try:
        result = engine.run_stream(ng.RunConfig(stream_mode=ng.StreamMode.DEBUG), on_event)
        answer = result.output["channels"]["answer"]["value"]
        print(f"ok, attempts: {calls}, answer: {answer}", end="")
    except RuntimeError:
        print(f"gave up after {calls} attempts", end="")
    print(f", backoff: {', '.join(pauses)}")
