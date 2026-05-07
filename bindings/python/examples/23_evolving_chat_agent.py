"""23 — Evolving chat agent: per-thread agent that rewrites its own JSON.

Difference from example 22:
  - 22 is goal-driven: one task, evolve until output passes a JSON
    schema check.
  - 23 is conversation-driven: multi-turn chat with persistent thread
    state, evolve between turns based on accumulated history. Prior
    messages survive the evolution because the new engine shares the
    checkpoint store with the old one.

Pattern shape (from ProjectDatePop's per-customer evolving agent):

  1. Each thread has its own agent definition (JSON).
  2. Conversation runs through the current definition.
  3. ``evolve_agent()`` inspects the thread's conversation and proposes
     a revised JSON.
  4. ``validate_agent()`` rejects unsafe proposals (whitelist node
     types, required channels, edge connectivity, node count cap).
  5. New definition compiles into a fresh engine that *shares* the
     checkpoint store with the old one. With ``resume_if_exists=True``
     prior messages survive because their channel + reducer is
     compatible across the two graph versions.
  6. An ``__graph_meta__`` channel records evolution events alongside
     the messages timeline. Replay/audit can reconstruct which graph
     version produced each turn.

The ``__graph_meta__`` convention is purely application-level — the
engine has no special handling. It is a regular append-reduced channel
that the application updates via ``engine.update_state(thread_id,
{"__graph_meta__": [meta]})`` on each evolution.

Runs end-to-end with no API key — a deterministic mock chat node + a
heuristic mock evolver demonstrate the loop. Set ``OPENAI_API_KEY`` to
swap in a live OpenAI provider for the chat node (the evolver stays
mock for reproducibility; the live evolver shape is shown in
ProjectDatePop's main_neograph.py).
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import neograph_engine as ng

sys.path.insert(0, str(Path(__file__).resolve().parent))


# ── A deterministic in-process Provider (no network needed) ─────────
#
# Lets the example demonstrate the evolution loop without an API key.
# Reads system_prompt + last user message and returns a reply whose
# *length* and *style* tracks the system_prompt — so when the evolver
# shrinks max_tokens or appends a "be technical" instruction, the
# user can see the change in the next turn's reply.

class MockProvider(ng.Provider):
    def get_name(self):
        return "mock"

    def complete(self, params):
        sys_prompt = ""
        last_user = ""
        for m in params.messages:
            if m.role == "system":
                sys_prompt = m.content or ""
            elif m.role == "user":
                last_user = m.content or ""

        brevity = "2 sentences" in sys_prompt or "shorter" in sys_prompt.lower()
        technical = "technical" in sys_prompt.lower()

        body = f"Re: {last_user[:60]}"
        if technical:
            body += " (analytical breakdown follows: factor A correlates with metric M at coefficient 0.42; factor B contributes residually)"
        else:
            body += " — here's a friendly take with some context and a suggestion you might enjoy thinking about"
        if brevity:
            body = body.split(" — ")[0].split(" (")[0]
            body += "."

        cap = max(20, params.max_tokens or 200)
        if len(body) > cap:
            body = body[: cap - 3].rstrip() + "..."

        msg = ng.ChatMessage(role="assistant", content=body)
        completion = ng.ChatCompletion()
        completion.message = msg
        return completion


# ── Generic config-driven chat node ─────────────────────────────────
#
# All persona / token-cap knobs live in node.config so the evolver
# can tweak them via plain JSON. This is the same shape as
# ProjectDatePop's PromptedChatNode.

class PromptedChatNode(ng.GraphNode):
    def __init__(self, name: str, node_def: dict, ctx):
        super().__init__()
        self._name = name
        cfg = (node_def or {}).get("config", {}) or {}
        self._system_prompt = cfg.get("system_prompt", "You are a helpful assistant.")
        self._max_tokens = int(cfg.get("max_output_tokens", 200))
        self._ctx = ctx

    def get_name(self):
        return self._name

    def run(self, input):
        msgs = input.state.get("messages") or []
        params = ng.CompletionParams()
        params.model = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
        params.temperature = 0.4
        params.max_tokens = self._max_tokens
        # Build the list locally then assign — params.messages is bound
        # to a C++ std::vector, so .append() on the property's view is a
        # silent no-op. Build then assign is the safe pattern.
        all_messages = [ng.ChatMessage(role="system", content=self._system_prompt)]
        for m in msgs:
            role = m.get("role", "")
            content = m.get("content", "")
            if role in ("user", "assistant", "system") and content:
                all_messages.append(ng.ChatMessage(role=role, content=content))
        params.messages = all_messages

        result = self._ctx.provider.complete(params)
        text = result.message.content if result.message else ""
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": text,
            "node": self._name,
            "created_at": time.time(),
        }])]


ng.NodeFactory.register_type(
    "prompted_chat",
    lambda name, cfg, ctx: PromptedChatNode(name, cfg, ctx),
)


# ── Initial agent definition ────────────────────────────────────────

INITIAL_AGENT: dict[str, Any] = {
    "name": "evolving_chat_v1",
    "_version": 1,
    "channels": {
        "messages":       {"reducer": "append"},
        "__graph_meta__": {"reducer": "append"},
    },
    "nodes": {
        "chat": {
            "type": "prompted_chat",
            "config": {
                "system_prompt": "You are a friendly assistant. Reply with helpful context.",
                "max_output_tokens": 400,
            },
        },
    },
    "edges": [
        {"from": ng.START_NODE, "to": "chat"},
        {"from": "chat",        "to": ng.END_NODE},
    ],
}


# ── Validator: reject unsafe proposals before compile ───────────────
#
# This is the safety boundary between an LLM-proposed JSON and the
# running engine. Anything the validator passes must compile and run;
# anything that doesn't compile is the validator's bug, not the
# evolver's privilege.

ALLOWED_NODE_TYPES = {"prompted_chat"}


def validate_agent(defn: dict) -> tuple[bool, str]:
    if not isinstance(defn, dict):
        return False, "not an object"
    if "name" not in defn:
        return False, "missing 'name'"

    nodes = defn.get("nodes") or {}
    if not isinstance(nodes, dict) or not nodes:
        return False, "missing or empty 'nodes'"
    if len(nodes) > 6:
        return False, f"node count cap exceeded ({len(nodes)} > 6)"
    for k, v in nodes.items():
        ntype = (v or {}).get("type")
        if ntype not in ALLOWED_NODE_TYPES:
            return False, f"node '{k}' has unsupported type {ntype!r}"

    chans = defn.get("channels") or {}
    if "messages" not in chans:
        return False, "must keep 'messages' channel"
    if (chans["messages"] or {}).get("reducer") != "append":
        return False, "'messages' reducer must remain 'append'"
    if "__graph_meta__" not in chans:
        return False, "must keep '__graph_meta__' audit channel"
    if (chans["__graph_meta__"] or {}).get("reducer") != "append":
        return False, "'__graph_meta__' reducer must remain 'append'"

    edges = defn.get("edges") or []
    has_start = any(e.get("from") == ng.START_NODE for e in edges)
    has_end = any(e.get("to") == ng.END_NODE for e in edges)
    if not (has_start and has_end):
        return False, "edges must include both START and END"
    valid_targets = set(nodes.keys()) | {ng.START_NODE, ng.END_NODE}
    for e in edges:
        if e.get("from") not in valid_targets or e.get("to") not in valid_targets:
            return False, f"edge {e} references unknown node"

    return True, "ok"


# ── Mock evolver: heuristic mutation from conversation signals ──────
#
# A live evolver would replace this with an LLM call (see
# ProjectDatePop main_neograph.py::evolve_customer_agent for the
# prompt shape). Heuristics here let the example run reproducibly
# without a key.

def propose_new_agent(current: dict, conversation: list[dict]) -> tuple[dict, str]:
    new = json.loads(json.dumps(current))
    new["_version"] = current.get("_version", 1) + 1
    cfg = new["nodes"]["chat"]["config"]

    user_text = " ".join(
        m.get("content", "") for m in conversation if m.get("role") == "user"
    ).lower()

    if "shorter" in user_text or "brief" in user_text or "concise" in user_text:
        cfg["max_output_tokens"] = max(60, cfg.get("max_output_tokens", 400) // 3)
        cfg["system_prompt"] = cfg.get("system_prompt", "") + " Reply in 2 sentences max."
        return new, "user requested brevity — shrink max_tokens + add brevity instruction"

    if "technical" in user_text or "details" in user_text or "depth" in user_text:
        cfg["system_prompt"] = cfg.get("system_prompt", "") + " Use technical terminology and quantitative detail."
        return new, "user prefers technical depth — relax persona toward analytical tone"

    return new, "no clear signal — keep current persona"


# ── Audit logger: record evolution event in __graph_meta__ channel ──
#
# Application-level convention (not an engine feature). Every
# evolution writes one entry; on replay you can scan __graph_meta__
# in global_version order to reconstruct which graph version
# produced each message turn.

def log_evolution(engine, thread_id: str, new_def: dict, reason: str) -> dict:
    state = engine.get_state(thread_id) or {}
    gv_before = state.get("global_version", 0)

    canonical = json.dumps(new_def, sort_keys=True, ensure_ascii=False).encode()
    meta = {
        "version": new_def.get("_version"),
        "hash": hashlib.sha256(canonical).hexdigest()[:12],
        "applied_after_global_version": gv_before,
        "reason": reason,
        "applied_at": time.time(),
    }
    engine.update_state(
        thread_id,
        {"__graph_meta__": [meta]},
        as_node="__evolve__",
    )
    return meta


# ── Driver ──────────────────────────────────────────────────────────

def _last_assistant(state) -> str:
    msgs = (state.get("channels", {}).get("messages") or {}).get("value") or []
    for m in reversed(msgs):
        if m.get("role") == "assistant":
            return m.get("content", "")
    return ""


def _make_provider():
    if os.getenv("OPENAI_API_KEY"):
        from _common import openai_provider
        print("(using live OpenAIProvider — set by OPENAI_API_KEY)")
        return openai_provider()
    print("(using deterministic MockProvider — set OPENAI_API_KEY to use live OpenAI)")
    return MockProvider()


def main():
    provider = _make_provider()
    ctx = ng.NodeContext(provider=provider)
    store = ng.InMemoryCheckpointStore()
    TID = "customer_alice"

    agent = INITIAL_AGENT
    engine = ng.GraphEngine.compile(agent, ctx, store)

    pre_evolve_turns = [
        "Hi! What should I think about when planning a winter trip?",
        "That's a lot — can you make it shorter please?",
        "Also more technical — give me concrete temperature and gear specs.",
    ]
    for i, user_msg in enumerate(pre_evolve_turns, 1):
        print(f"\n=== turn {i} (graph v{agent['_version']}) ===")
        print(f"  user:      {user_msg}")
        engine.run(ng.RunConfig(
            thread_id=TID,
            input={"messages": [{"role": "user", "content": user_msg}]},
            resume_if_exists=True,
        ))
        state = engine.get_state(TID)
        print(f"  assistant: {_last_assistant(state)[:140]}")

    print("\n=== evolving agent ===")
    state = engine.get_state(TID)
    conv = (state.get("channels", {}).get("messages") or {}).get("value") or []
    proposed, reason = propose_new_agent(agent, conv)

    ok, why = validate_agent(proposed)
    if not ok:
        print(f"  REJECTED: {why}")
        return
    try:
        ng.GraphEngine.compile(proposed, ctx, store)
    except Exception as e:
        print(f"  COMPILE FAILED: {e}")
        return
    print(f"  accepted: {reason}")
    print(f"  v{agent['_version']} → v{proposed['_version']}")

    meta = log_evolution(engine, TID, proposed, reason)
    print(f"  __graph_meta__ entry: hash={meta['hash']} applied_after_gv={meta['applied_after_global_version']}")

    agent = proposed
    engine = ng.GraphEngine.compile(agent, ctx, store)

    post_evolve_turns = [
        "OK now what about wind exposure on ridges?",
    ]
    for user_msg in post_evolve_turns:
        print(f"\n=== post-evolve turn (graph v{agent['_version']}) ===")
        print(f"  user:      {user_msg}")
        engine.run(ng.RunConfig(
            thread_id=TID,
            input={"messages": [{"role": "user", "content": user_msg}]},
            resume_if_exists=True,
        ))
        state = engine.get_state(TID)
        print(f"  assistant: {_last_assistant(state)[:140]}")

    print("\n=== audit chain ===")
    state = engine.get_state(TID)
    msgs = (state.get("channels", {}).get("messages") or {}).get("value") or []
    metas = (state.get("channels", {}).get("__graph_meta__") or {}).get("value") or []
    print(f"  total messages:    {len(msgs)}")
    print(f"  total evolutions:  {len(metas)}")
    for m in metas:
        print(f"    v{m['version']} hash={m['hash']} applied_after_gv={m['applied_after_global_version']} — {m['reason']}")
    print(f"  final global_version: {state.get('global_version')}")

    print("\n=== full message timeline ===")
    for i, m in enumerate(msgs):
        prefix = f"  [{i}] {m.get('role','?'):9s}"
        node = m.get("node", "")
        node_str = f" ({node})" if node else ""
        content = m.get("content", "")[:80]
        print(f"{prefix}{node_str} {content}")


if __name__ == "__main__":
    main()
