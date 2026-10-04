"""07 — Checkpoint + human approval: interrupt before dispatch, then resume.

A scripted model proposes a payment. The engine saves a checkpoint before
tool_dispatch and returns an interrupted result. We inspect the proposal,
verify the simulated payment has not run, then resume the same engine/thread.
The demonstration approves automatically; a real UI supplies that decision.
No API key, payment processor, or real charge is involved.

Run:
    python 07_checkpoint_hitl.py
"""

import json

import neograph_engine as ng


class FakeLLMNode(ng.GraphNode):
    """Stand-in for the LLM. Emits an order-payment tool_call."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": "",
            "tool_calls": [{
                "id": "call_pay_1",
                "name": "pay_order",
                "arguments": json.dumps({
                    "item": "MacBook Pro",
                    "quantity": 1,
                    "amount_krw": 2_500_000,
                }),
            }],
        }])]


class PayOrderTool(ng.Tool):
    """Simulated sensitive action; never contacts a payment processor."""

    def __init__(self):
        super().__init__()
        self.invocations = []

    def get_name(self):
        return "pay_order"

    def get_definition(self):
        return ng.ChatTool(
            name="pay_order",
            description="Charge the user's stored payment method.",
            parameters={
                "type": "object",
                "properties": {
                    "item":       {"type": "string"},
                    "quantity":   {"type": "integer"},
                    "amount_krw": {"type": "integer"},
                },
            },
        )

    def execute(self, arguments):
        self.invocations.append(arguments)
        return f"Charged {arguments['amount_krw']} KRW for {arguments['item']}."


pay_tool = PayOrderTool()

ng.NodeFactory.register_type(
    "fake_llm",
    lambda name, config, ctx: FakeLLMNode(name),
)


# A single resumable graph: proposal -> interrupt before dispatch -> execution.

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "checkpoint_approval",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "fake_llm"}, "dispatch": {"type": "tool_dispatch"}},
    "edges": [
        {"from": ng.START_NODE, "to": "llm"},
        {"from": "llm", "to": "dispatch"},
        {"from": "dispatch", "to": ng.END_NODE},
    ],
    "interrupt_before": ["dispatch"],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext(tools=[pay_tool]),
                               ng.InMemoryCheckpointStore())
paused = engine.run(ng.RunConfig(thread_id="order-42", input={"messages": []}))
assert paused.interrupted, "expected a checkpoint before payment dispatch"
assert pay_tool.invocations == [], "payment ran before approval"
checkpoint = engine.get_state("order-42")
assert checkpoint is not None, "interrupted workflow must be resumable"
proposed_msg = paused.output["channels"]["messages"]["value"][-1]
proposed_call = proposed_msg["tool_calls"][0]
args = json.loads(proposed_call["arguments"])

print("=== HUMAN APPROVAL ===")
print(f"The agent proposes calling `{proposed_call['name']}` with:")
for k, v in args.items():
    print(f"  {k}: {v}")
print()

# In a real UI you'd prompt here; for the example we approve unconditionally.
APPROVED = True
print(f"approved = {APPROVED}")
print()

if not APPROVED:
    print("Skipping payment — order aborted.")
    raise SystemExit(0)

# Resume from the actual interrupted checkpoint, not a second graph.
state2 = engine.resume("order-42")
assert not state2.interrupted
assert len(pay_tool.invocations) == 1, "approval must execute payment once"
tool_msgs = [m for m in state2.output["channels"]["messages"]["value"]
             if m.get("role") == "tool"]
print("=== TOOL RESULT ===")
print(tool_msgs[-1]["content"])
print(f"\npay_tool invocations: {pay_tool.invocations}")
