"""12 — Self-Ask with intermediate questions.

Self-Ask (Press et al., arXiv:2210.03350): when the model needs to
answer a multi-hop question, it generates *intermediate questions*,
the engine answers each via a sub-LLM call, then the model produces
the final answer using the intermediate answers as context.

Real LLM via OpenAIProvider. The decomposer node calls the LLM with
a prompt that asks it to either (a) emit a follow-up question or
(b) emit the final answer. The graph loops on `state["next_step"]`.

Run:
    pip install neograph-engine python-dotenv
    cp .env.example .env  # fill in OPENAI_API_KEY
    python 12_self_ask.py
"""

from _common import ng, openai_provider


SELF_ASK_SYSTEM = (
    "You are a careful researcher who breaks complex questions into "
    "simple intermediate questions before answering.\n\n"
    "Output one of these two formats EXACTLY:\n"
    "  Follow up: <a single intermediate question>\n"
    "  -- or --\n"
    "  So the final answer is: <the answer>\n\n"
    "Choose 'Follow up' until you have enough information to answer "
    "the original question, then choose 'So the final answer is'."
)

INTERMEDIATE_SYSTEM = (
    "Answer the following question concisely in one sentence."
)


class DecomposeNode(ng.GraphNode):
    """Calls the LLM with the running scratchpad. Either emits a
    follow-up question or the final answer.

    Writes either:
      next_step = 'follow_up'  + follow_up = '...'
      next_step = 'done'       + final_answer = '...'
    """

    def __init__(self, name, provider):
        super().__init__()
        self._name = name
        self._provider = provider

    def get_name(self):
        return self._name

    def run(self, input):
        question = input.state.get("question")
        scratchpad = input.state.get("scratchpad") or ""

        prompt = f"Question: {question}\n{scratchpad}"
        completion = self._provider.complete(ng.CompletionParams(
            messages=[
                ng.ChatMessage(role="system", content=SELF_ASK_SYSTEM),
                ng.ChatMessage(role="user", content=prompt),
            ],
            temperature=0.0,
        ))
        text = completion.message.content.strip()

        if "Follow up:" in text:
            follow_up = text.split("Follow up:", 1)[1].strip().splitlines()[0]
            return [
                ng.ChannelWrite("next_step", "follow_up"),
                ng.ChannelWrite("follow_up", follow_up),
            ]
        if "So the final answer is:" in text:
            final = text.split("So the final answer is:", 1)[1].strip()
            return [
                ng.ChannelWrite("next_step", "done"),
                ng.ChannelWrite("final_answer", final),
            ]
        # The model didn't follow the format — treat as done with raw text.
        return [
            ng.ChannelWrite("next_step", "done"),
            ng.ChannelWrite("final_answer", text),
        ]


class AnswerIntermediateNode(ng.GraphNode):
    """Answers the latest follow_up question, appends the Q+A to the
    scratchpad."""

    def __init__(self, name, provider):
        super().__init__()
        self._name = name
        self._provider = provider

    def get_name(self):
        return self._name

    def run(self, input):
        follow_up = input.state.get("follow_up")
        completion = self._provider.complete(ng.CompletionParams(
            messages=[
                ng.ChatMessage(role="system", content=INTERMEDIATE_SYSTEM),
                ng.ChatMessage(role="user", content=follow_up),
            ],
            temperature=0.0,
        ))
        answer = completion.message.content.strip()

        scratchpad = input.state.get("scratchpad") or ""
        scratchpad += f"\nFollow up: {follow_up}\nIntermediate answer: {answer}"
        return [
            # `scratchpad` carries the conversation; overwrite with the
            # extended version each iteration.
            ng.ChannelWrite("scratchpad", scratchpad),
        ]


provider = openai_provider()

ng.NodeFactory.register_type(
    "decompose",
    lambda name, config, ctx: DecomposeNode(name, provider),
)
ng.NodeFactory.register_type(
    "answer_intermediate",
    lambda name, config, ctx: AnswerIntermediateNode(name, provider),
)


definition = {
    "name": "self_ask",
    "channels": {
        "question":     {"reducer": "overwrite"},
        "scratchpad":   {"reducer": "overwrite"},
        "next_step":    {"reducer": "overwrite"},
        "follow_up":    {"reducer": "overwrite"},
        "final_answer": {"reducer": "overwrite"},
    },
    "nodes": {
        "decompose":           {"type": "decompose"},
        "answer_intermediate": {"type": "answer_intermediate"},
    },
    "edges": [
        {"from": ng.START_NODE,           "to": "decompose"},
        {"from": "answer_intermediate",   "to": "decompose"},
    ],
    "conditional_edges": [
        {
            "from":      "decompose",
            "condition": "route_channel",
            # Looks up channel matching the condition name; here we use
            # `next_step` as the channel-keyed router.
            "channel":   "next_step",
            "routes": {
                "follow_up": "answer_intermediate",
                "done":      ng.END_NODE,
            },
        },
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())

result = engine.run(ng.RunConfig(
    thread_id="t1",
    input={
        "question": (
            "Who was the president of the United States when the year was "
            "the same as the year Apollo 11 landed on the Moon?"
        ),
    },
    max_steps=10,
))

chans = result.output["channels"]
print("=== final answer ===")
print(chans["final_answer"]["value"])
print("\n=== scratchpad ===")
print(chans["scratchpad"]["value"])
