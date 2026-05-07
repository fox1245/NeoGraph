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
  3. ``evolve_agent()`` asks an LLM to inspect the conversation and
     propose a revised JSON.
  4. ``validate_agent()`` rejects unsafe proposals (whitelist node
     types, required channels, edge connectivity, node count cap).
     This is the safety boundary between LLM-proposed JSON and the
     running engine.
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

Run::

    OPENAI_API_KEY=sk-... python 23_evolving_chat_agent.py

Both the chat node and the evolver call OpenAI, so this is a real
end-to-end demo. Cost is small (~5 short completions per run).
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
from _common import openai_provider  # noqa: E402


# ── Generic config-driven chat node ─────────────────────────────────
#
# All persona / token-cap knobs live in node.config so the evolver
# can tweak them via plain JSON.

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
        params.messages = [ng.ChatMessage(role="system", content=self._system_prompt)]
        for m in msgs:
            role = m.get("role", "")
            content = m.get("content", "")
            if role in ("user", "assistant", "system") and content:
                params.messages.append(ng.ChatMessage(role=role, content=content))

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
                "system_prompt": "You are a friendly assistant. Reply with helpful context and one practical suggestion.",
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


# ── LLM-driven evolver ──────────────────────────────────────────────
#
# A separate LLM call whose only job is to emit a revised graph JSON
# based on the current agent + the conversation. The validator above
# is the safety boundary — anything LLM proposes that fails validation
# is rejected before compile.

def propose_new_agent(current: dict, conversation: list[dict], provider) -> tuple[dict, str]:
    convo = "\n".join(
        f"[{m.get('role','?')}] {m.get('content','')[:200]}"
        for m in conversation[-20:]
    ) or "(no conversation yet)"

    instructions = f"""You are an agent-evolver. Given a NeoGraph agent
definition (JSON) and the recent conversation that ran through it,
propose a REVISED graph JSON that better fits the user's communication
style.

Hard rules — violating any rejects your output:
  - Output ONLY a JSON object — no prose, no markdown fences.
  - Only "prompted_chat" node type is registered.
  - Keep the "messages" channel with reducer "append".
  - Keep the "__graph_meta__" channel with reducer "append".
  - Edges must connect "__start__" → ... → "__end__".
  - Max 6 nodes.
  - Make ONE specific, minimal change — focused improvement, not rewrite.
  - Bump "_version" by 1.

Soft guidance:
  - User asks for shorter answers → lower max_output_tokens + add brevity instruction in system_prompt.
  - User wants more technical depth → adjust system_prompt to allow technical detail.
  - User's tone is casual → relax persona; formal → match formal register.

Recent conversation:
{convo}

Current graph:
{json.dumps(current, indent=2, ensure_ascii=False)}

Return the revised graph JSON."""

    params = ng.CompletionParams()
    params.model = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
    params.temperature = 0.2
    params.max_tokens = 1500
    params.messages = [
        ng.ChatMessage(role="system",
                       content="You output strict JSON only — no prose, no fences."),
        ng.ChatMessage(role="user", content=instructions),
    ]
    result = provider.complete(params)
    raw = (result.message.content if result.message else "").strip()

    if raw.startswith("```"):
        raw = raw.split("\n", 1)[1] if "\n" in raw else raw[3:]
        if raw.endswith("```"):
            raw = raw[:-3]
        raw = raw.strip()

    try:
        proposed = json.loads(raw)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"evolver returned non-JSON: {e}; raw[:300]: {raw[:300]!r}")

    reason = proposed.pop("_reason", None) or "evolved from conversation"
    return proposed, reason


# ── Audit logger: record evolution event in __graph_meta__ channel ──

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


def main():
    provider = openai_provider()
    ctx = ng.NodeContext(provider=provider)
    store = ng.InMemoryCheckpointStore()
    TID = "customer_alice"

    agent = INITIAL_AGENT
    engine = ng.GraphEngine.compile(agent, ctx, store)

    pre_evolve_turns = [
        "Hi! I'm planning a winter weekend trip to the mountains. What should I think about?",
        "Way too much, can you keep it shorter? Just the essentials.",
        "Also I'd prefer concrete numbers — temperatures, gear weights, that kind of detail.",
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
        reply = _last_assistant(state)
        print(f"  assistant: {reply[:200]}{'...' if len(reply) > 200 else ''}")

    print("\n=== evolving agent ===")
    state = engine.get_state(TID)
    conv = (state.get("channels", {}).get("messages") or {}).get("value") or []
    proposed, reason = propose_new_agent(agent, conv, provider)

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
    cfg_diff = _summarise_change(agent, proposed)
    if cfg_diff:
        print(f"  changes: {cfg_diff}")

    meta = log_evolution(engine, TID, proposed, reason)
    print(f"  __graph_meta__: hash={meta['hash']} applied_after_gv={meta['applied_after_global_version']}")

    agent = proposed
    engine = ng.GraphEngine.compile(agent, ctx, store)

    post_evolve_turns = [
        "OK now what about wind exposure on exposed ridges?",
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
        reply = _last_assistant(state)
        print(f"  assistant: {reply[:200]}{'...' if len(reply) > 200 else ''}")

    print("\n=== audit chain ===")
    state = engine.get_state(TID)
    msgs = (state.get("channels", {}).get("messages") or {}).get("value") or []
    metas = (state.get("channels", {}).get("__graph_meta__") or {}).get("value") or []
    print(f"  total messages:    {len(msgs)}")
    print(f"  total evolutions:  {len(metas)}")
    for m in metas:
        print(f"    v{m['version']} hash={m['hash']} applied_after_gv={m['applied_after_global_version']} — {m['reason']}")
    print(f"  final global_version: {state.get('global_version')}")


def _summarise_change(old: dict, new: dict) -> str:
    """Best-effort diff summary for the console output."""
    parts = []
    old_nodes = set((old.get("nodes") or {}).keys())
    new_nodes = set((new.get("nodes") or {}).keys())
    if old_nodes != new_nodes:
        added = new_nodes - old_nodes
        removed = old_nodes - new_nodes
        if added: parts.append(f"+nodes={sorted(added)}")
        if removed: parts.append(f"-nodes={sorted(removed)}")
    for nname in old_nodes & new_nodes:
        old_cfg = (old["nodes"][nname] or {}).get("config") or {}
        new_cfg = (new["nodes"][nname] or {}).get("config") or {}
        old_max = old_cfg.get("max_output_tokens")
        new_max = new_cfg.get("max_output_tokens")
        if old_max != new_max:
            parts.append(f"{nname}.max_tokens {old_max}→{new_max}")
        old_sp = old_cfg.get("system_prompt", "")
        new_sp = new_cfg.get("system_prompt", "")
        if old_sp != new_sp:
            parts.append(f"{nname}.system_prompt changed ({len(old_sp)}→{len(new_sp)} chars)")
    return ", ".join(parts)


if __name__ == "__main__":
    main()
