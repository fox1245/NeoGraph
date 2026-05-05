"""13 — Multi-agent debate (2 debaters + 1 judge, 1 round).

Two LLM-driven 'debater' agents argue opposing sides; a third agent
acts as judge and picks the stronger argument. Uses `Send` to fan
out the debaters in parallel within a single super-step.

Real LLM via OpenAIProvider. Three custom Python nodes — debater_a,
debater_b, judge — each with its own system prompt.

Run:
    pip install neograph-engine python-dotenv
    cp .env.example .env  # fill in OPENAI_API_KEY
    python 13_multi_agent_debate.py
"""

from _common import ng, openai_provider


PROVIDER = openai_provider()


def make_persona_node(name, system_prompt):
    """Factory for a debater node that calls the LLM with a persona-
    specific system prompt and writes its argument to the
    `arguments` channel (append reducer)."""

    class DebaterNode(ng.GraphNode):
        def __init__(self, _name):
            super().__init__()
            self._name = _name

        def get_name(self):
            return self._name

        def run(self, input):
            topic = input.state.get("topic")
            completion = PROVIDER.complete(ng.CompletionParams(
                messages=[
                    ng.ChatMessage(role="system", content=system_prompt),
                    ng.ChatMessage(role="user",
                                   content=f"Topic: {topic}\n\nState your case in 2-3 sentences."),
                ],
            ))
            return [ng.ChannelWrite("arguments", [{
                "agent": name,
                "case": completion.message.content.strip(),
            }])]

    return DebaterNode


class FanOutDebatersNode(ng.GraphNode):
    """Triggers both debater nodes in parallel via Send."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        # Same input goes to each branch; the debaters' system prompts
        # carry the persona difference.
        return [
            ng.Send("debater_a", {}),
            ng.Send("debater_b", {}),
        ]


class JudgeNode(ng.GraphNode):
    """Reads the collected arguments and writes a verdict."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        topic = input.state.get("topic")
        args = input.state.get("arguments") or []
        if not args:
            return [ng.ChannelWrite("verdict", "(no arguments to judge)")]

        cases = "\n".join(f"- {a['agent']}: {a['case']}" for a in args)
        completion = PROVIDER.complete(ng.CompletionParams(
            messages=[
                ng.ChatMessage(role="system", content=(
                    "You are an impartial judge. Review the arguments "
                    "below and pick which one is stronger. Respond with "
                    "the winner's name on the first line and a one-"
                    "sentence reason on the second line.")),
                ng.ChatMessage(role="user", content=(
                    f"Topic: {topic}\n\nArguments:\n{cases}\n\n"
                    "Who makes the stronger case?")),
            ],
            temperature=0.0,
        ))
        return [ng.ChannelWrite("verdict", completion.message.content.strip())]


ng.NodeFactory.register_type(
    "fanout_debaters",
    lambda name, config, ctx: FanOutDebatersNode(name),
)
ng.NodeFactory.register_type(
    "debater_a",
    lambda name, config, ctx: make_persona_node(
        "Optimist",
        "You are Optimist — argue in favour of the topic. Be concise.",
    )(name),
)
ng.NodeFactory.register_type(
    "debater_b",
    lambda name, config, ctx: make_persona_node(
        "Skeptic",
        "You are Skeptic — argue against the topic. Be concise.",
    )(name),
)
ng.NodeFactory.register_type(
    "judge",
    lambda name, config, ctx: JudgeNode(name),
)


definition = {
    "name": "multi_agent_debate",
    "channels": {
        "topic":     {"reducer": "overwrite"},
        "arguments": {"reducer": "append"},
        "verdict":   {"reducer": "overwrite"},
    },
    "nodes": {
        "fanout":    {"type": "fanout_debaters"},
        "debater_a": {"type": "debater_a"},
        "debater_b": {"type": "debater_b"},
        "judge":     {"type": "judge"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "fanout"},
        # Both Send-targets converge at `judge`; the engine waits for
        # all sibling branches before running judge (barrier semantics
        # implicit when fan-in goes to a shared next-step).
        {"from": "debater_a",   "to": "judge"},
        {"from": "debater_b",   "to": "judge"},
        {"from": "judge",       "to": ng.END_NODE},
    ],
}


engine = ng.GraphEngine.compile(definition, ng.NodeContext())
engine.set_worker_count(2)  # actual parallelism for the two debaters

result = engine.run(ng.RunConfig(
    thread_id="t1",
    input={
        "topic": "Static typing always pays off in agent-framework code.",
    },
    max_steps=8,
))

chans = result.output["channels"]
print("=== TOPIC ===")
print(chans["topic"]["value"])

print("\n=== ARGUMENTS ===")
for arg in chans["arguments"]["value"]:
    print(f"\n[{arg['agent']}]")
    print(arg["case"])

print("\n=== VERDICT ===")
print(chans["verdict"]["value"])
