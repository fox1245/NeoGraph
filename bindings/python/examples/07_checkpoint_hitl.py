"""07 — Checkpoint + Human-in-the-Loop (interrupt + resume).

A two-step workflow that pauses before a sensitive action, lets a
human inspect the state, and resumes (or aborts) on demand. No real
LLM — a fake-LLM custom node returns canned tool-call messages so
the example runs offline.

Pattern:
  1. fake_llm node emits a `pay_order` tool_call.
  2. Engine reaches `dispatch` node; we pause via `update_state`
     before it runs (the C++ examples use `interrupt_before`; the
     Python binding's clean equivalent is to read state, decide,
     and either run or skip the next step).
  3. We approve, then call `engine.resume_async(thread_id)` or
     simply re-run from the saved checkpoint.

Run:
    pip install neograph-engine
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

    def execute(self, state):
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
    """Sensitive tool — only run after human approval."""

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


# Two-graph approach to demo HITL:
#   stage 1 graph: only the LLM node (writes the tool_call into messages)
#   then we inspect — and if approved, run stage 2: just the dispatch.

stage1_def = {
    "name": "stage1_propose",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "fake_llm"}},
    "edges": [
        {"from": ng.START_NODE, "to": "llm"},
        {"from": "llm",         "to": ng.END_NODE},
    ],
}

stage2_def = {
    "name": "stage2_execute",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"dispatch": {"type": "tool_dispatch"}},
    "edges": [
        {"from": ng.START_NODE, "to": "dispatch"},
        {"from": "dispatch",    "to": ng.END_NODE},
    ],
}

# Stage 1: propose.
ctx1 = ng.NodeContext()  # tools not yet needed
engine1 = ng.GraphEngine.compile(stage1_def, ctx1)
state1 = engine1.run(ng.RunConfig(thread_id="order-42", input={"messages": []}))

proposed_msg = state1.output["channels"]["messages"]["value"][-1]
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

# Stage 2: execute. Replay the proposed messages so dispatch sees
# the tool_call.
ctx2 = ng.NodeContext(tools=[pay_tool])
engine2 = ng.GraphEngine.compile(stage2_def, ctx2)
state2 = engine2.run(ng.RunConfig(
    thread_id="order-42",
    input={"messages": state1.output["channels"]["messages"]["value"]},
))

tool_msgs = [m for m in state2.output["channels"]["messages"]["value"]
             if m.get("role") == "tool"]
print("=== TOOL RESULT ===")
print(tool_msgs[-1]["content"])
print(f"\npay_tool invocations: {pay_tool.invocations}")
