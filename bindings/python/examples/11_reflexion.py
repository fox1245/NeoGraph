"""11 — Reflexion loop (Shinn et al., arXiv:2303.11366).

A two-LLM-pass agent: first the actor produces an attempt, then the
critic scores it and writes a textual reflection. The next iteration
prepends the reflection to the actor's prompt. Loops until the critic
says 'good enough' OR `max_iterations` is reached.

Real LLM via OpenAIProvider. The actor and critic are two custom
Python nodes, each calling the provider with its own system prompt.

Run:
    pip install neograph-engine python-dotenv
    cp .env.example .env  # fill in OPENAI_API_KEY
    python 11_reflexion.py
"""

from _common import ng, openai_provider


ACTOR_SYSTEM = (
    "You are a careful problem-solver. The user will give you a task. "
    "If a previous attempt and a critic's reflection are present, "
    "incorporate the reflection's suggestions in your new attempt. "
    "Output ONLY your final answer, nothing else."
)

CRITIC_SYSTEM = (
    "You are a strict but constructive critic. Evaluate the actor's "
    "latest attempt against the user's task. Reply in this exact format:\n"
    "VERDICT: <ok|retry>\n"
    "REFLECTION: <one or two sentences explaining what to change>"
)


class ActorNode(ng.GraphNode):
    def __init__(self, name, provider):
        super().__init__()
        self._name = name
        self._provider = provider

    def get_name(self):
        return self._name

    def execute(self, state):
        task = state.get("task")
        prior_attempt = state.get("attempt") or ""
        prior_reflection = state.get("reflection") or ""

        prompt = f"Task:\n{task}\n"
        if prior_attempt:
            prompt += f"\nYour previous attempt:\n{prior_attempt}\n"
        if prior_reflection:
            prompt += f"\nCritic's reflection:\n{prior_reflection}\n"
        prompt += "\nProduce your improved answer:"

        completion = self._provider.complete(ng.CompletionParams(
            messages=[
                ng.ChatMessage(role="system", content=ACTOR_SYSTEM),
                ng.ChatMessage(role="user", content=prompt),
            ],
        ))
        return [ng.ChannelWrite("attempt", completion.message.content.strip())]


class CriticNode(ng.GraphNode):
    def __init__(self, name, provider):
        super().__init__()
        self._name = name
        self._provider = provider

    def get_name(self):
        return self._name

    def execute(self, state):
        task = state.get("task")
        attempt = state.get("attempt") or ""
        n = (state.get("iteration") or 0) + 1

        completion = self._provider.complete(ng.CompletionParams(
            messages=[
                ng.ChatMessage(role="system", content=CRITIC_SYSTEM),
                ng.ChatMessage(role="user", content=(
                    f"Task:\n{task}\n\nActor's latest attempt:\n{attempt}\n"
                )),
            ],
        ))
        verdict_line, _, reflection = completion.message.content.partition(
            "REFLECTION:")
        verdict = "ok" if "ok" in verdict_line.lower() else "retry"
        reflection = reflection.strip() or "(no reflection given)"

        return [
            ng.ChannelWrite("verdict",    verdict),
            ng.ChannelWrite("reflection", reflection),
            ng.ChannelWrite("iteration",  n),
        ]


provider = openai_provider()

ng.NodeFactory.register_type(
    "actor",
    lambda name, config, ctx: ActorNode(name, provider),
)
ng.NodeFactory.register_type(
    "critic",
    lambda name, config, ctx: CriticNode(name, provider),
)


# Conditional edge: from critic, route on `verdict` channel — `ok`
# ends, `retry` loops back to actor. `route_channel` is a built-in
# condition that just looks up the named channel value.
definition = {
    "name": "reflexion",
    "channels": {
        "task":       {"reducer": "overwrite"},
        "attempt":    {"reducer": "overwrite"},
        "verdict":    {"reducer": "overwrite"},
        "reflection": {"reducer": "overwrite"},
        "iteration":  {"reducer": "overwrite"},
    },
    "nodes": {
        "actor":  {"type": "actor"},
        "critic": {"type": "critic"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "actor"},
        {"from": "actor",       "to": "critic"},
    ],
    "conditional_edges": [
        {
            "from":      "critic",
            "condition": "route_channel",
            "routes": {
                "retry": "actor",
                "ok":    ng.END_NODE,
            },
        },
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())

result = engine.run(ng.RunConfig(
    thread_id="t1",
    input={
        "task": (
            "Write a haiku about a debugger that gets stuck in an "
            "infinite loop and finally breaks free."
        ),
    },
    max_steps=8,  # caps iterations even if critic never says ok
))

chans = result.output["channels"]
print("=== iterations:", chans["iteration"]["value"])
print("=== final attempt ===")
print(chans["attempt"]["value"])
print("\n=== last verdict:", chans["verdict"]["value"])
print("=== last reflection:", chans["reflection"]["value"])
print("\ntrace:", result.execution_trace)
