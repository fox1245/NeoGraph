"""22 — Self-evolving graph: the agent rewrites its own JSON.

Premise: NeoGraph definitions are JSON. A node's prompt can live in
``node.config`` and be read at execute() time. So an agent can:

  1. Run its current graph on a task.
  2. Score the output against a goal.
  3. If the score is below threshold, hand the *current graph JSON* +
     the failure to a "self-modifier" LLM call and ask for a revised
     JSON.
  4. Apply the proposed JSON, recompile a fresh engine, retry.

This is the smallest demo that proves the loop closes:

  Goal:     produce a JSON object with exactly the keys
            {"name", "age", "city"} (typed string/int/string).
  Initial:  one LLM node with a vague prompt — almost always fails.
  Evolve:   LLM proposes a stricter system_prompt; if still failing,
            it adds a critic node, then a retry edge.

The 'self-modifier' is a separate LLM call whose ONLY job is to emit
a new graph JSON. We do not let it execute arbitrary Python — the
attack surface is just the JSON validator (`GraphEngine.compile`
rejects invalid graphs).

Run:
    OPENAI_API_KEY=sk-... python 22_self_evolving_graph.py

Caveats / honest limits of this PoC:

  - No persistence. Each iteration's JSON is in-memory; production
    would version it (JSON in a checkpoint store + diff log).
  - Single-shot self-modifier. A real evolver would also be a graph
    (planner → critic → mutator), and the meta-graph could itself be
    self-modified.
  - No safety budget: stops after `max_iters`. A production loop also
    needs cost/latency budgets and rejection of out-of-distribution
    graphs (e.g., reject definitions that introduce un-registered
    node types or unbounded loops).
  - Observed in practice: the LLM is good at adding nodes but bad at
    reasoning about channel data flow. A 3-stage chain that routes
    "raw_reply" through writer→critic→validator without distinct
    channels per stage degrades — each node's "user_template" only
    sees {seed}, not the previous stage's output. The fix that the
    self-modifier needs to learn (and our prompt doesn't currently
    teach it well): add per-stage channels and reference them via
    {channel_name} placeholders in user_template.

    What this tells us: graph topology mutation is the easy part;
    data-flow rewiring is the hard part. A second-iteration version
    of this PoC should expose the channel graph more explicitly to
    the modifier — e.g., let it see "node X currently reads from
    channels {a,b} and writes to {c}" rather than expecting it to
    trace edges through raw JSON.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any

import neograph_engine as ng

# Reuse the example helper for env loading + Provider construction.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import openai_provider  # noqa: E402


# ── A generic LLM node whose prompts come from the graph JSON ───────
#
# The whole point: the prompts live in ``node.config``, so the
# self-modifier can edit them via plain JSON without touching Python.

class PromptedLLMNode(ng.GraphNode):
    """LLM node that reads ``system_prompt`` and ``user_template`` from
    its JSON ``config`` block and writes the reply to ``output_channel``.

    Example node JSON::

        "writer": {
            "type": "prompted_llm",
            "config": {
                "system_prompt": "You output strict JSON only.",
                "user_template": "Write a profile for: {seed}",
                "output_channel": "raw_reply"
            }
        }
    """

    def __init__(self, name: str, node_def: dict, ctx):
        super().__init__()
        self._name = name
        # The engine passes the whole node spec (including "type" and
        # "config" keys) to the factory, so config lives at node_def["config"].
        cfg = node_def.get("config", {}) or {}
        self._system_prompt = cfg.get("system_prompt", "")
        self._user_template = cfg.get("user_template", "{seed}")
        self._output_channel = cfg.get("output_channel", "reply")
        self._ctx = ctx

    def get_name(self):
        return self._name

    def execute(self, state):
        seed = state.get("seed") or ""
        params = ng.CompletionParams()
        params.model = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
        params.temperature = 0.4
        params.max_tokens = 250

        params.messages = [
            ng.ChatMessage(role="system", content=self._system_prompt),
            ng.ChatMessage(
                role="user",
                content=self._user_template.format(seed=seed)),
        ]
        result = self._ctx.provider.complete(params)
        text = result.message.content if result.message else ""
        return [ng.ChannelWrite(self._output_channel, text)]


def _node_factory(name, config, ctx):
    # Pybind passes config as a Python dict already (json_to_py).
    return PromptedLLMNode(name, config, ctx)


ng.NodeFactory.register_type("prompted_llm", _node_factory)


# ── Goal: parse-able JSON with exactly the right shape ──────────────

def evaluate(reply_text: str) -> tuple[float, str]:
    """Score the LLM reply against the goal.

    Returns (score 0..1, feedback string for the self-modifier).
    """
    try:
        data = json.loads(reply_text)
    except json.JSONDecodeError as e:
        return 0.0, f"reply is not valid JSON ({e}). raw: {reply_text[:200]!r}"

    expected = {"name", "age", "city"}
    have = set(data.keys()) if isinstance(data, dict) else set()
    missing = expected - have
    extra = have - expected

    if missing or extra:
        return 0.4, (
            f"keys mismatch — missing={sorted(missing)} extra={sorted(extra)} "
            f"got={sorted(have)}")

    if not isinstance(data.get("name"), str):
        return 0.6, f"'name' must be a string, got {type(data['name']).__name__}"
    if not isinstance(data.get("age"), int):
        return 0.7, f"'age' must be an int, got {type(data['age']).__name__}"
    if not isinstance(data.get("city"), str):
        return 0.8, f"'city' must be a string, got {type(data['city']).__name__}"

    return 1.0, "perfect"


# ── The self-modifier: prompt the LLM with current graph + failure ──

def propose_new_graph(current_graph: dict, last_reply: str,
                      score: float, feedback: str, provider) -> dict:
    """Ask the LLM to emit a revised graph JSON.

    Constraints baked into the prompt:
      - must keep the same ``name`` + at least the START → end path
      - may modify any ``config`` block (esp. ``system_prompt`` /
        ``user_template``)
      - may NOT introduce node types other than ``prompted_llm``
      - returns strict JSON only
    """
    instructions = f"""You are a graph-rewriter. Given a NeoGraph
definition (JSON) and the failure mode of its last run, output a
REVISED graph that fixes the failure.

Hard rules:
  - Output ONLY a JSON object — no prose, no markdown fences.
  - Keep the same top-level "name" field.
  - Only "prompted_llm" node type is registered. Do not invent others.
  - The graph must reach END_NODE from START_NODE.
  - You may modify any node's "config.system_prompt" or
    "config.user_template", change channel reducers, add/remove
    nodes/edges, or add a critic node — anything within the rules above.

The goal is for the final reply (read from the "raw_reply" channel)
to be a JSON object with EXACTLY these keys:
  - "name" (string)
  - "age"  (integer)
  - "city" (string)

Last run scored {score:.2f}/1.0.
Last reply was:
{last_reply!r}

Feedback on what went wrong:
{feedback}

Current graph:
{json.dumps(current_graph, indent=2)}

Return the revised graph JSON now."""

    params = ng.CompletionParams()
    params.model = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
    params.temperature = 0.2
    params.max_tokens = 1200
    params.messages = [
        ng.ChatMessage(role="system",
                       content="You output strict JSON only — no prose."),
        ng.ChatMessage(role="user", content=instructions),
    ]
    result = provider.complete(params)
    raw = result.message.content if result.message else ""

    # Strip code fences if the LLM ignored the instruction.
    raw = raw.strip()
    if raw.startswith("```"):
        raw = raw.split("\n", 1)[1] if "\n" in raw else raw[3:]
        if raw.endswith("```"):
            raw = raw[:-3]
        raw = raw.strip()

    try:
        return json.loads(raw)
    except json.JSONDecodeError as e:
        raise RuntimeError(
            f"self-modifier returned non-JSON: {e}. raw: {raw[:400]!r}")


# ── Driver ──────────────────────────────────────────────────────────

INITIAL_GRAPH: dict[str, Any] = {
    "name": "person_profile",
    "channels": {
        "seed":      {"reducer": "overwrite"},
        "raw_reply": {"reducer": "overwrite"},
    },
    "nodes": {
        "writer": {
            "type": "prompted_llm",
            "config": {
                # Deliberately vague — the agent should learn it needs
                # to enforce JSON-only output, then field types.
                "system_prompt": "You answer the user's question.",
                "user_template": "Tell me about a person named {seed}.",
                "output_channel": "raw_reply",
            },
        },
    },
    "edges": [
        {"from": ng.START_NODE, "to": "writer"},
        {"from": "writer",      "to": ng.END_NODE},
    ],
}


def run_one(graph: dict, seed: str, ctx) -> str:
    engine = ng.GraphEngine.compile(graph, ctx)
    result = engine.run(ng.RunConfig(thread_id="evolve", input={"seed": seed}))
    return result.output["channels"]["raw_reply"]["value"]


def main(max_iters: int = 5):
    provider = openai_provider()
    ctx = ng.NodeContext(provider=provider)

    graph = INITIAL_GRAPH
    history: list[dict] = []

    for i in range(max_iters):
        print(f"\n──────── iteration {i} ────────")
        print(f"graph nodes: {list(graph.get('nodes', {}).keys())}")

        try:
            reply = run_one(graph, seed="Alice", ctx=ctx)
        except Exception as e:
            print(f"compile/run failed: {e}")
            score, feedback = 0.0, f"graph failed to run: {e}"
            reply = ""
        else:
            score, feedback = evaluate(reply)
            print(f"score:    {score:.2f}")
            print(f"feedback: {feedback}")
            print(f"reply:    {reply[:200]}")

        history.append({
            "iter": i,
            "score": score,
            "feedback": feedback,
            "reply_preview": reply[:200],
            "graph_node_count": len(graph.get("nodes", {})),
        })

        if score >= 1.0:
            print(f"\n✅ goal reached at iteration {i}")
            break

        try:
            graph = propose_new_graph(graph, reply, score, feedback, provider)
        except Exception as e:
            print(f"self-modifier failed: {e}")
            break

    print("\n──────── evolution history ────────")
    for h in history:
        print(f"  iter={h['iter']} score={h['score']:.2f} "
              f"nodes={h['graph_node_count']} — {h['feedback'][:80]}")

    print("\n──────── final graph ────────")
    print(json.dumps(graph, indent=2))


if __name__ == "__main__":
    main(max_iters=int(os.getenv("MAX_ITERS", "5")))
