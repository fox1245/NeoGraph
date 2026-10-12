"""77 — NodeInterrupt: a breakpoint the node itself raises.

`interrupt_before` stops a graph in front of a fixed node. `NodeInterrupt` is for the case
where the node decides: a small refund goes through, a large one needs a person. The node
raises, the engine saves a checkpoint and `run()` returns with `interrupted == True`; the
payload tells the caller what to ask. `engine.resume(thread_id, answer)` runs the node again,
and this time `input.ctx.resume_value` holds the answer. An answer is not always "yes": the
node reads it and may refuse.

The node runs again from the top, so keep everything before the raise free of side effects.
A checkpoint store is required; the thread_id names the paused run.

Run:
    pip install neograph-engine
    python 77_dynamic_interrupt.py

smoke-expect: small refund: Refund of 40 USD sent
smoke-expect: paused: node=refund reason="Refund of 250 USD needs approval" amount=250
smoke-expect: resumed: Refund of 250 USD sent (approved by alice)
smoke-expect: refused: Refund of 900 USD declined (bob)
smoke-expect: node ran 5 times for 3 refunds
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng

LIMIT = 100   # refunds above this need approval
visits = 0


class RefundNode(ng.GraphNode):
    def get_name(self):
        return "refund"

    def run(self, input):
        global visits
        visits += 1
        amount = input.state.get("amount")
        money = f"Refund of {amount} USD"
        result = f"{money} sent"
        if amount > LIMIT:
            answer = input.ctx.resume_value     # None on the first visit
            if answer is None:
                raise ng.NodeInterrupt({"amount": amount}, reason=f"{money} needs approval")
            who = answer.get("by", "?")
            result = (f"{money} sent (approved by {who})" if answer.get("approved")
                      else f"{money} declined ({who})")
        return [ng.ChannelWrite("result", result)]


ng.NodeFactory.register_type("refund", lambda name, config, ctx: RefundNode())

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "node_interrupt",
    "channels": {"amount": {"reducer": "overwrite"}, "result": {"reducer": "overwrite"}},
    "nodes": {"refund": {"type": "refund"}},
    "edges": [
        {"from": ng.START_NODE, "to": "refund"},
        {"from": "refund", "to": ng.END_NODE},
    ],
}
# The store is what makes a pause possible: it holds the run until someone answers.
engine = ng.GraphEngine.compile(definition, ng.NodeContext(), ng.InMemoryCheckpointStore())


def start(thread_id, amount):
    return engine.run(ng.RunConfig(thread_id=thread_id, input={"amount": amount}))


def result_of(run):
    return run.output["channels"]["result"]["value"]


# 1. Under the limit: the node never raises.
print("small refund:", result_of(start("t-small", 40)))

# 2. Over the limit: the run pauses, the payload says what to ask.
paused = start("t-large", 250)
print(f'paused: node={paused.interrupt_node} reason="{paused.interrupt_value["reason"]}" '
      f'amount={paused.interrupt_value["value"]["amount"]}')

# 3. An operator answers; the node runs again and acts on the answer.
print("resumed:", result_of(engine.resume("t-large", {"approved": True, "by": "alice"})))

# 4. A refusal is an answer too.
start("t-refused", 900)
print("refused:", result_of(engine.resume("t-refused", {"approved": False, "by": "bob"})))

print(f"node ran {visits} times for 3 refunds")
