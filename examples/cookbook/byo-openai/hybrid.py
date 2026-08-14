"""NeoGraph + openai-sdk hybrid — bring your own OpenRouter client.

Demonstrates that NeoGraph's `Provider` is Python-subclassable
(v0.2.3+): wrap the official `openai` SDK and plug it into NeoGraph
graph nodes. The SDK still owns retries, transport, and observability;
OpenRouter owns model routing and the ZDR provider preference.

Run:
    pip install neograph-engine>=0.2.3 openai
    echo 'OPENROUTER_API_KEY=sk-or-...' > .env
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
    def __init__(self, client: OpenAI,
                 model: str = "~deepseek/deepseek-v4-flash-latest"):
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
            extra_body={"provider": {"zdr": True}},
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
    if not os.environ.get("OPENROUTER_API_KEY"):
        print("OPENROUTER_API_KEY not set", file=sys.stderr)
        return 2

    # The official SDK remains the transport/client surface; the OpenRouter
    # endpoint is selected explicitly with the pinned DeepSeek model.
    sdk_client = OpenAI(
        api_key=os.environ["OPENROUTER_API_KEY"],
        base_url="https://openrouter.ai/api/v1",
    )
    print(f"[hybrid] using OpenRouter inside NeoGraph {ng.__version__} graph")

    provider = OpenAISdkProvider(
        sdk_client, model="~deepseek/deepseek-v4-flash-latest")
    ctx = ng.NodeContext(
        provider=provider,
        instructions=("You are a concise assistant. "
                      "Each turn must fit in 1-2 sentences."),
    )

    # A built-in `llm_call` gets its system prompt from
    # NodeContext.instructions and routes through our OpenAISdkProvider.
    graph_def = {
        "name": "byo-openai-demo",
        "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"answer": {"type": "llm_call"}},
        "edges": [
            {"from": ng.START_NODE, "to": "answer"},
            {"from": "answer",     "to": ng.END_NODE},
        ],
    }
    print("[hybrid] running one llm_call through the OpenAI SDK provider")

    engine = ng.GraphEngine.compile(graph_def, ctx)
    user_q = "How do I make my Python script run a graph through an LLM call?"
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
