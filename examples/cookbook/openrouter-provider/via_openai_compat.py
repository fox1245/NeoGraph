"""NeoGraph + OpenRouter via the OpenAI-compatible chat endpoint.

OpenRouter exposes `/api/v1/chat/completions` with the normalized Chat API
shape, so NeoGraph's built-in `OpenAIProvider` can own the request path and
connection pooling. The endpoint, API key, pinned model, and explicit
zero-data-retention (ZDR) provider preference are set here.

Run:
    echo 'OPENROUTER_API_KEY=sk-or-...' > .env
    python via_openai_compat.py
"""

import os
import sys
from pathlib import Path

import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider


OPENROUTER_BASE_URL = "https://openrouter.ai/api"
MODEL = "~deepseek/deepseek-v4-flash-latest"


def _load_env_if_present() -> None:
    for p in (Path(".env"), Path(__file__).parent / ".env",
              Path(__file__).resolve().parents[3] / ".env"):
        if p.exists():
            for line in p.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))
            return


def main() -> int:
    _load_env_if_present()
    api_key = os.environ.get("OPENROUTER_API_KEY")
    if not api_key:
        print("OPENROUTER_API_KEY not set", file=sys.stderr)
        return 2

    provider = OpenAIProvider(
        api_key=api_key,
        base_url=OPENROUTER_BASE_URL,
        default_model=MODEL,
        timeout_seconds=120,
        provider_routing={"zdr": True},
    )
    print(f"[openrouter] using {MODEL} via OpenAI-compatible path")

    ctx = ng.NodeContext(
        provider=provider,
        instructions="Be concise. One short sentence per answer.",
    )
    graph_def = {
        "name": "openrouter-compat",
        "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"answer": {"type": "llm_call"}},
        "edges": [
            {"from": ng.START_NODE, "to": "answer"},
            {"from": "answer", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(graph_def, ctx)
    user_q = "What's the capital of France?"
    print(f"[user] {user_q}")
    result = engine.run(ng.RunConfig(
        thread_id="openrouter-compat",
        input={"messages": [{"role": "user", "content": user_q}]},
    ))
    for message in result.output["channels"]["messages"]["value"]:
        role = message.get("role", "?")
        content = message.get("content", "")
        if content:
            print(f"  {role:>9}: {content}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
