"""22 - Self-evolving graph: the agent rewrites its own JSON, and only admitted rewrites run.

NeoGraph definitions are JSON, and a node's prompt can live in `node.config`. So an agent can
run its graph, score the output against a goal, and when the score falls short hand the
current definition and the failure to a rewriter model and ask for a revised one. The driver
admits a proposal only if it keeps the graph's name and schema version and uses nothing but the
registered `prompted_llm` node type; then it compiles a fresh engine and tries again. The
goal is a JSON object with exactly the keys name (string), age (integer) and city (string).

The scripted model plays both roles. The vague writer answers in prose and scores 0. The
rewriter first proposes a node of an unregistered type (rejected, with the reason fed back),
then a revised prompt that the writer follows. A model decides what to propose; this driver
decides what may run. Admission is not a sandbox: an admitted node can still spend provider
resources, so every graph run also has a bounded superstep count.

Run:
    python 22_self_evolving_graph.py            # scripted model, no key
    python 22_self_evolving_graph.py --live     # hosted model; OPENAI_API_KEY in the environment

smoke-expect: iteration 0: score 0.00
smoke-expect: rejected: node 'shell' has unregistered type 'shell_exec'
smoke-expect: iteration 1: score 1.00
smoke-expect: goal reached at iteration 1
smoke-expect: model calls: 4
smoke-forbid: [neograph] lint
"""

from __future__ import annotations

import json
from typing import Any

import neograph_engine as ng
from _common import ask_text, example_provider
from _scripted import say


# A generic LLM node whose prompts come from the graph JSON, so a rewrite needs no new code.
class PromptedLLMNode(ng.GraphNode):
    def __init__(self, name, config, ctx):
        super().__init__()
        self._name, self._ctx = name, ctx
        self._config = (config or {}).get("config", {})      # the factory receives the whole node spec

    def get_name(self):
        return self._name

    def run(self, input):
        values = {channel: input.state.get(channel) for channel in input.state.channel_names()}
        text = ask_text(self._ctx.provider, messages=[
            ng.ChatMessage(role="system", content=self._config["system_prompt"]),
            ng.ChatMessage(role="user", content=self._config["user_template"].format_map(values)),
        ], max_output_tokens=400)
        return [ng.ChannelWrite(self._config["output_channel"], text)]


ng.NodeFactory.register_type("prompted_llm", lambda name, config, ctx: PromptedLLMNode(name, config, ctx))


def graph_with(system_prompt: str, extra_nodes: dict | None = None) -> dict[str, Any]:
    return {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "person_profile",
        "channels": {"seed": {"reducer": "overwrite"}, "raw_reply": {"reducer": "overwrite"}},
        "nodes": {"writer": {"type": "prompted_llm", "config": {
            "system_prompt": system_prompt, "user_template": "Tell me about a person named {seed}.",
            "output_channel": "raw_reply"}}, **(extra_nodes or {})},
        "edges": [{"from": ng.START_NODE, "to": "writer"}, {"from": "writer", "to": ng.END_NODE}],
    }


def evaluate(reply: str) -> tuple[float, str]:
    """Score 0..1 against the goal, with feedback for the rewriter."""
    try:
        data = json.loads(reply)
    except json.JSONDecodeError:
        return 0.0, f"the reply is not valid JSON: {reply[:60]!r}"
    if not isinstance(data, dict) or set(data) != {"name", "age", "city"}:
        return 0.4, "the keys must be exactly name, age and city"
    ok = isinstance(data["name"], str) and type(data["age"]) is int and isinstance(data["city"], str)
    return (1.0, "perfect") if ok else (0.7, "name and city are strings, age is an integer")


def admit(proposal: Any, current: dict) -> dict:
    """The boundary between a model's proposal and the engine: raise unless it may run."""
    if not isinstance(proposal, dict) or proposal.get("name") != current["name"]:
        raise ValueError("the graph name must stay the same")
    if proposal.get("schema_version") != current["schema_version"]:
        raise ValueError("the schema_version must stay the same")
    for name, node in (proposal.get("nodes") or {}).items():
        if node.get("type") != "prompted_llm":
            raise ValueError(f"node {name!r} has unregistered type {node.get('type')!r}")
    return proposal


# What the scripted model answers. The rewriter's user message starts with "Rewrite".
REJECTED = graph_with("You answer.", {"shell": {"type": "shell_exec", "config": {}}})
REVISED = graph_with('Reply with one JSON object {"name": string, "age": integer, "city": string}, nothing else.')


def script(turn):
    if turn.last_user_text.startswith("Rewrite"):
        return say(json.dumps(REVISED if "rejected" in turn.last_user_text else REJECTED))
    if "JSON" in turn.system_text:
        return say('{"name": "Alice", "age": 31, "city": "Lyon"}')
    return say("Alice is a 31-year-old engineer who lives in Lyon.")


with example_provider(script) as chosen:
    ctx = ng.NodeContext(provider=chosen.provider)
    graph = graph_with("You answer the user's question.")
    feedback = ""
    for iteration in range(4):
        engine = ng.GraphEngine.compile(graph, ctx)
        run = engine.run(ng.RunConfig(thread_id=f"evolve-{iteration}", input={"seed": "Alice"}, max_steps=20))
        score, feedback = evaluate(run.output["channels"]["raw_reply"]["value"])
        print(f"iteration {iteration}: score {score:.2f}, {feedback}")
        if score == 1.0:
            print(f"goal reached at iteration {iteration}")
            break
        for _attempt in range(2):                      # ask again once if the proposal is rejected
            request = f"Rewrite this graph so the goal is met. Feedback: {feedback}\n{json.dumps(graph)}"
            raw = ask_text(chosen.provider, [{"role": "user", "content": request}], max_output_tokens=1500)
            try:
                graph = admit(json.loads(raw), graph)
                break
            except ValueError as error:
                print(f"rejected: {error.args[0]}")
                feedback += f" (previous proposal rejected: {error.args[0]})"
    if chosen.scripted:
        print("model calls:", chosen.scripted.call_count)
        assert score == 1.0 and chosen.scripted.call_count == 4
