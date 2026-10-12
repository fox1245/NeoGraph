"""23 - Evolving chat agent: the conversation survives an admitted rewrite of the agent.

Example 22 evolves a graph toward one goal. This one evolves between the turns of a chat.
Each thread has an agent definition (JSON). After a few turns an evolver model reads the
conversation and proposes a revised definition. `validate_agent` is the boundary: it admits
a proposal only if it keeps the schema version, the channels and a path from start to end,
and uses only registered node types. An admitted definition compiles into a fresh engine
that shares the checkpoint store with the old one, so with `resume_if_exists=True` the earlier
messages are still there. An `__graph_meta__` channel records each accepted version and hash;
that is application metadata, not replay evidence.

Run:
    python 23_evolving_chat_agent.py            # scripted model, no key
    python 23_evolving_chat_agent.py --live     # hosted model; OPENAI_API_KEY in the environment

smoke-expect: evolver proposed v2, admitted: ok
smoke-expect: changes: system_prompt, max_output_tokens 400 -> 60
smoke-expect: messages kept across the rewrite: 4 of 4
smoke-expect: __graph_meta__: v2
smoke-expect: model calls: 4
smoke-forbid: [neograph] lint
"""

import hashlib
import json
import time

import neograph_engine as ng
from _common import ask_text, example_provider
from _scripted import say


class PromptedChatNode(ng.GraphNode):
    """A chat node whose persona and length cap live in its JSON config, so the evolver can tune them."""

    def __init__(self, name, spec, ctx):
        super().__init__()
        self._name, self._ctx = name, ctx
        self._config = (spec or {}).get("config", {})

    def get_name(self):
        return self._name

    def run(self, input):
        history = [ng.ChatMessage(role=m["role"], content=m["content"])
                   for m in input.state.get("messages") or []]
        text = ask_text(self._ctx.provider,
                        [ng.ChatMessage(role="system", content=self._config["system_prompt"]), *history],
                        max_output_tokens=int(self._config["max_output_tokens"]))
        return [ng.ChannelWrite("messages", [{"role": "assistant", "content": text, "node": self._name}])]


ng.NodeFactory.register_type("prompted_chat", lambda name, spec, ctx: PromptedChatNode(name, spec, ctx))


def agent(version: int, system_prompt: str, max_tokens: int) -> dict:
    return {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "evolving_chat", "_version": version,
        "channels": {"messages": {"reducer": "append"}, "__graph_meta__": {"reducer": "append"}},
        "nodes": {"chat": {"type": "prompted_chat", "config": {
            "system_prompt": system_prompt, "max_output_tokens": max_tokens}}},
        "edges": [{"from": ng.START_NODE, "to": "chat"}, {"from": "chat", "to": ng.END_NODE}],
    }


def validate_agent(current: dict, proposed) -> tuple[bool, str]:
    """The safety boundary between a model's JSON and the running engine."""
    if not isinstance(proposed, dict) or proposed.get("schema_version") != current["schema_version"]:
        return False, "must be an object that keeps the schema_version"
    if proposed.get("_version") != current["_version"] + 1:
        return False, "must bump _version by one"
    nodes = proposed.get("nodes") or {}
    if not nodes or len(nodes) > 6 or any(n.get("type") != "prompted_chat" for n in nodes.values()):
        return False, "one to six prompted_chat nodes only"
    channels = proposed.get("channels") or {}
    if any((channels.get(c) or {}).get("reducer") != "append" for c in ("messages", "__graph_meta__")):
        return False, "must keep the append channels messages and __graph_meta__"
    if not any(e.get("from") == ng.START_NODE for e in proposed.get("edges", [])) or \
            not any(e.get("to") == ng.END_NODE for e in proposed.get("edges", [])):
        return False, "must connect __start__ to __end__"
    return True, "ok"


V1 = agent(1, "You are a friendly assistant. Give helpful context and one practical suggestion.", 400)
V2 = agent(2, "You are a friendly assistant. Answer in one short sentence.", 60)


def script(turn):
    if "evolver" in turn.last_user_text:                       # the evolver call
        return say(json.dumps(V2))
    if "one short sentence" in turn.system_text:
        return say("Wind is strongest on exposed ridges, so turn back early.")
    return say("Winter trips need layers, a charged phone, a paper map and a plan for early dark. "
               "Check the avalanche report the night before and tell someone your route.")


def messages_of(engine, thread):
    return (engine.get_state(thread)["channels"]["messages"] or {}).get("value") or []


with example_provider(script) as chosen:
    ctx, store, thread = ng.NodeContext(provider=chosen.provider), ng.InMemoryCheckpointStore(), "alice"

    def chat(engine, text):
        engine.run(ng.RunConfig(thread_id=thread, resume_if_exists=True,
                                input={"messages": [{"role": "user", "content": text}]}))
        return messages_of(engine, thread)[-1]["content"]

    engine = ng.GraphEngine.compile(V1, ctx, store)
    print("v1 reply:", chat(engine, "I'm planning a winter weekend in the mountains.")[:50] + "...")
    print("v1 reply:", chat(engine, "Too long, keep it shorter please.")[:50] + "...")

    # The evolver reads the conversation and proposes a revised definition.
    conversation = "\n".join(f"[{m['role']}] {m['content'][:100]}" for m in messages_of(engine, thread))
    request = f"You are an agent evolver. Propose a revised graph.\n{conversation}\n{json.dumps(V1)}"
    proposed = json.loads(ask_text(chosen.provider, [{"role": "user", "content": request}],
                                   max_output_tokens=1500))
    admitted, why = validate_agent(V1, proposed)
    print(f"evolver proposed v{proposed['_version']}, admitted: {why}")
    assert admitted
    old, new = V1["nodes"]["chat"]["config"], proposed["nodes"]["chat"]["config"]
    print(f"changes: system_prompt, max_output_tokens {old['max_output_tokens']} -> {new['max_output_tokens']}")

    # Compile the admitted definition on the same store and record the rewrite.
    kept = len(messages_of(engine, thread))
    meta = {"version": proposed["_version"], "applied_at": time.time(), "reason": "shorter answers",
            "hash": hashlib.sha256(json.dumps(proposed, sort_keys=True).encode()).hexdigest()[:12]}
    engine.update_state(thread, {"__graph_meta__": [meta]}, as_node="__evolve__")
    engine = ng.GraphEngine.compile(proposed, ctx, store)
    survived = len(messages_of(engine, thread))
    print(f"messages kept across the rewrite: {survived} of {kept}")
    print("v2 reply:", chat(engine, "What about wind on exposed ridges?"))
    recorded = engine.get_state(thread)["channels"]["__graph_meta__"]["value"]
    print(f"__graph_meta__: v{recorded[0]['version']} hash {recorded[0]['hash']}")
    if chosen.scripted:
        print("model calls:", chosen.scripted.call_count)
        assert chosen.scripted.call_count == 4 and survived == kept == 4
