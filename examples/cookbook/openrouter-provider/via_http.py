"""NeoGraph + OpenRouter through a custom HTTP Provider.

Demonstrates the v0.2.3 Provider trampoline: subclass
`neograph_engine.Provider`, call the OpenRouter Chat Completions API in
`complete()`, and return a NeoGraph `ChatCompletion`. The graph engine does
not depend on the transport implementation; this path owns the HTTP request
and response mapping directly.

Run:
    echo 'OPENROUTER_API_KEY=sk-or-...' > .env
    pip install neograph-engine>=0.2.3 httpx
    python via_http.py
"""

import os
import sys
from pathlib import Path

try:
    import httpx
except ImportError:
    print("Skipping: install httpx (`pip install httpx`).", file=sys.stderr)
    sys.exit(0)

import neograph_engine as ng


OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions"
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


class OpenRouterHttpProvider(ng.Provider):
    """Map NeoGraph completion parameters to OpenRouter's chat API."""

    def __init__(self, api_key: str, model: str = MODEL):
        super().__init__()
        self.api_key = api_key
        self.model = model
        self._client = httpx.Client(timeout=120)

    def complete(self, params):
        response = self._client.post(
            OPENROUTER_URL,
            headers={
                "Authorization": f"Bearer {self.api_key}",
                "Content-Type": "application/json",
            },
            json={
                "model": params.model or self.model,
                "messages": [
                    {"role": message.role, "content": message.content}
                    for message in params.messages
                ],
                "temperature": float(params.temperature),
                "provider": {"zdr": True},
            },
        )
        response.raise_for_status()
        body = response.json()
        choice = body["choices"][0]
        message = choice["message"]

        out = ng.ChatCompletion()
        out.message.role = message.get("role", "assistant")
        out.message.content = message.get("content") or ""
        usage = body.get("usage") or {}
        out.usage.prompt_tokens = usage.get("prompt_tokens", 0)
        out.usage.completion_tokens = usage.get("completion_tokens", 0)
        out.usage.total_tokens = usage.get("total_tokens", 0)
        return out

    def get_name(self) -> str:
        return "openrouter-http"


def main() -> int:
    _load_env_if_present()
    api_key = os.environ.get("OPENROUTER_API_KEY")
    if not api_key:
        print("OPENROUTER_API_KEY not set", file=sys.stderr)
        return 2

    provider = OpenRouterHttpProvider(api_key)
    print(f"[openrouter] using {MODEL} via direct HTTP")

    ctx = ng.NodeContext(
        provider=provider,
        instructions="Be concise. One short sentence per answer.",
    )
    graph_def = {
        "name": "openrouter-http",
        "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"answer": {"type": "llm_call"}},
        "edges": [
            {"from": ng.START_NODE, "to": "answer"},
            {"from": "answer", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(graph_def, ctx)
    user_q = "What's 17 * 23?"
    print(f"[user] {user_q}")
    result = engine.run(ng.RunConfig(
        thread_id="openrouter-http",
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
