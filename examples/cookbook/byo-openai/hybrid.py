"""NeoGraph + openai-sdk hybrid — bring your own OpenAI client.

Demonstrates that NeoGraph's `Provider` is Python-subclassable
(v0.2.3+): wrap the official `openai` SDK and plug it into NeoGraph
graph nodes. Keeps all your SDK-side configuration (retries, Azure,
custom transport, observability hooks) while gaining NeoGraph's
graph orchestration.

Run:
    pip install neograph-engine>=0.2.3 openai
    echo 'OPENAI_API_KEY=sk-...' > .env
    python hybrid.py
"""

import os
import sys
from pathlib import Path

import neograph_engine as ng
from openai import OpenAI


def _load_env_if_present():
    for p in (Path(".env"), Path(__file__).parent / ".env",
              Path(__file__).resolve().parents[3] / ".env"):
        if p.exists():
            for line in p.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))
            return


class OpenAISdkProvider(ng.Provider):
    """NeoGraph Provider backed by the official openai Python SDK.

    Translates between NeoGraph's CompletionParams / ChatCompletion
    shape and OpenAI's chat-completions API shape. Your SDK-level
    settings (retries, custom headers, Azure / proxy, observability
    callbacks) attach to the `client` argument and are honored on
    every call NeoGraph makes through this provider.
    """
    def __init__(self, client: OpenAI, model: str = "gpt-5.4-mini"):
        super().__init__()
        self.client = client
        self.model  = model
        self.calls  = 0

    def complete(self, params: ng.CompletionParams) -> ng.ChatCompletion:
        self.calls += 1
        messages = [{"role": m.role, "content": m.content}
                    for m in params.messages]
        model = params.model or self.model
        print(f"[provider] complete() call #{self.calls} "
              f"({len(messages)} msgs) — model={model}", file=sys.stderr)

        resp = self.client.chat.completions.create(
            model=model,
            messages=messages,
            temperature=params.temperature,
        )

        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = resp.choices[0].message.content or ""
        if resp.usage:
            out.usage.prompt_tokens     = resp.usage.prompt_tokens
            out.usage.completion_tokens = resp.usage.completion_tokens
            out.usage.total_tokens      = resp.usage.total_tokens
        return out

    def get_name(self) -> str:
        return "openai-sdk"


def main() -> int:
    _load_env_if_present()
    if not os.environ.get("OPENAI_API_KEY"):
        print("OPENAI_API_KEY not set", file=sys.stderr)
        return 2

    # User's existing openai SDK client — bring all your retry /
    # observability / Azure config here. Nothing is special about
    # this; it's whatever you already use elsewhere.
    sdk_client = OpenAI()
    print(f"[hybrid] using openai SDK inside NeoGraph {ng.__version__} graph")

    provider = OpenAISdkProvider(sdk_client, model="gpt-5.4-mini")
    ctx = ng.NodeContext(
        provider=provider,
        instructions=("You are a concise assistant. "
                      "Each turn must fit in 1-2 sentences."),
    )

    # 3-node graph: classify → respond → summarise. Each step is a
    # built-in `llm_call` node, so each step routes through our
    # OpenAISdkProvider.
    graph_def = {
        "name": "byo-openai-demo",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {
            "classify": {"type": "llm_call",
                "config": {"system": "Classify the user's intent in one word."}},
            "respond": {"type": "llm_call",
                "config": {"system": "Answer the user given the classification."}},
            "summarise": {"type": "llm_call",
                "config": {"system": "Summarise the conversation in one sentence."}},
        },
        "edges": [
            {"from": ng.START_NODE, "to": "classify"},
            {"from": "classify",     "to": "respond"},
            {"from": "respond",      "to": "summarise"},
            {"from": "summarise",    "to": ng.END_NODE},
        ],
    }
    print(f"[hybrid] running 3-node graph: classify → respond → summarise")

    engine = ng.GraphEngine.compile(graph_def, ctx)
    user_q = "How do I make my Python script run a graph through three LLM calls?"
    result = engine.run(ng.RunConfig(
        thread_id="hybrid-demo",
        input={"messages": [{"role": "user", "content": user_q}]},
    ))

    msgs = result.output["channels"]["messages"]["value"]
    print()
    for m in msgs:
        role = m.get("role", "?")
        content = m.get("content", "")
        if not content:
            continue
        print(f"  {role:>9}: {content}")
    print()
    print(f"[hybrid] provider.complete() called {provider.calls}× via openai SDK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
