"""08 — Intent classification + conditional routing.

A 'panel of experts' graph: an intent classifier node reads the
user's message, decides math / translate / general, and the
conditional edge dispatches to the matching expert. Each expert
adds its own answer to the messages channel.

Real LLM via OpenAIProvider. The classifier itself is a custom
Python node that calls the provider once with a classification
prompt — no special node type needed beyond what the binding
already exposes.

Run:
    pip install neograph-engine python-dotenv
    cp .env.example .env  # fill in OPENAI_API_KEY
    python 08_intent_routing.py
"""

from _common import ng, openai_provider


CLASSIFIER_PROMPT = (
    "Classify the user's message into exactly one label: "
    "'math' (numeric calculation), 'translate' (language translation), "
    "or 'general' (anything else). Respond with ONLY the label, "
    "no quotes, no explanation."
)


class ClassifierNode(ng.GraphNode):
    """Calls the LLM with a fixed classification prompt and writes
    the result label to the `__route__` channel."""

    def __init__(self, name, provider):
        super().__init__()
        self._name = name
        self._provider = provider

    def get_name(self):
        return self._name

    def run(self, input):
        msgs = input.state.get_messages()
        params = ng.CompletionParams(
            messages=[
                ng.ChatMessage(role="system", content=CLASSIFIER_PROMPT),
                *msgs,
            ],
            temperature=0.0,
        )
        completion = self._provider.complete(params)
        label = completion.message.content.strip().lower()
        if label not in {"math", "translate", "general"}:
            label = "general"
        return [ng.ChannelWrite("__route__", label)]


class ExpertNode(ng.GraphNode):
    """Calls the LLM with a domain-specific system prompt."""

    def __init__(self, name, provider, system_prompt):
        super().__init__()
        self._name = name
        self._provider = provider
        self._system = system_prompt

    def get_name(self):
        return self._name

    def run(self, input):
        msgs = input.state.get_messages()
        params = ng.CompletionParams(
            messages=[
                ng.ChatMessage(role="system", content=self._system),
                *msgs,
            ],
        )
        completion = self._provider.complete(params)
        # Append the expert's answer to messages.
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": completion.message.content,
        }])]


provider = openai_provider()

ng.NodeFactory.register_type(
    "classifier",
    lambda name, config, ctx: ClassifierNode(name, provider),
)
ng.NodeFactory.register_type(
    "math_expert",
    lambda name, config, ctx: ExpertNode(name, provider,
        "You are a math expert. Solve the user's problem step by step "
        "and end with the final numeric answer."),
)
ng.NodeFactory.register_type(
    "translate_expert",
    lambda name, config, ctx: ExpertNode(name, provider,
        "You are a professional translator. Translate the user's text "
        "into the requested target language. Output the translation only."),
)
ng.NodeFactory.register_type(
    "general_expert",
    lambda name, config, ctx: ExpertNode(name, provider,
        "You are a friendly, concise assistant. Answer the user's "
        "question in 1-2 sentences."),
)


definition = {
    "name": "intent_router",
    "channels": {
        "messages": {"reducer": "append"},
        "__route__": {"reducer": "overwrite"},
    },
    "nodes": {
        "classifier":  {"type": "classifier"},
        "math":        {"type": "math_expert"},
        "translate":   {"type": "translate_expert"},
        "general":     {"type": "general_expert"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "classifier"},
        {"from": "math",        "to": ng.END_NODE},
        {"from": "translate",   "to": ng.END_NODE},
        {"from": "general",     "to": ng.END_NODE},
    ],
    "conditional_edges": [
        {
            "from":      "classifier",
            "condition": "route_channel",
            "routes": {
                "math":      "math",
                "translate": "translate",
                "general":   "general",
            },
        },
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())

USER_INPUTS = [
    "What is 17 * 23?",
    "Translate 'good morning' into French.",
    "Who wrote the Iliad?",
]

for user_msg in USER_INPUTS:
    print(f"\n=== USER: {user_msg}")
    result = engine.run(ng.RunConfig(
        thread_id=f"q-{hash(user_msg)}",
        input={"messages": [{"role": "user", "content": user_msg}]},
    ))
    msgs = result.output["channels"]["messages"]["value"]
    answer = next((m["content"] for m in msgs
                   if m.get("role") == "assistant"), "(no answer)")
    route = result.output["channels"]["__route__"]["value"]
    print(f"    routed to: {route}")
    print(f"    answer:    {answer}")
