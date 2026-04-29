"""NeoGraph + Ollama via the native /api/chat endpoint.

Demonstrates the v0.2.3 Provider trampoline: subclass
`neograph_engine.Provider`, call any HTTP API in `complete()`,
return a NeoGraph `ChatCompletion`. The graph engine doesn't know
or care that the LLM lives on localhost — it just sees a Provider.

Why pick this over the OpenAI-compat path: access to Ollama-native
fields like `options` (per-request temperature / top_p / repeat_penalty),
`keep_alive` (how long the model stays loaded after idle), and
`format=json` (force JSON output without prompting tricks).

Run:
    ollama serve                              # in another terminal
    ollama pull qwen2.5:0.5b
    pip install httpx
    python via_native_api.py
"""

import sys

try:
    import httpx
except ImportError:
    print("Skipping: install httpx (`pip install httpx`).", file=sys.stderr)
    sys.exit(0)

import neograph_engine as ng


OLLAMA_URL = "http://127.0.0.1:11434"
MODEL      = "qwen2.5:0.5b"


class OllamaProvider(ng.Provider):
    """NeoGraph Provider talking to Ollama's native /api/chat.

    Maps NeoGraph CompletionParams → Ollama request shape, and the
    response back into NeoGraph ChatCompletion. Tools and streaming
    are intentionally omitted in this minimal example — see the
    byo-openai cookbook for a tool-capable Provider.
    """
    def __init__(self, host: str = OLLAMA_URL, model: str = MODEL,
                 keep_alive: str = "5m"):
        super().__init__()
        self.host       = host
        self.model      = model
        self.keep_alive = keep_alive
        self._client    = httpx.Client(timeout=120)

    def complete(self, params):
        body = {
            "model": params.model or self.model,
            "messages": [{"role": m.role, "content": m.content}
                         for m in params.messages],
            "stream": False,
            "options": {"temperature": float(params.temperature)},
            "keep_alive": self.keep_alive,
        }
        r = self._client.post(f"{self.host}/api/chat", json=body)
        r.raise_for_status()
        data = r.json()

        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = data.get("message", {}).get("content", "")

        # Ollama reports prompt_eval_count + eval_count; map them as
        # NeoGraph's prompt_tokens + completion_tokens for parity.
        if "prompt_eval_count" in data:
            out.usage.prompt_tokens = data["prompt_eval_count"]
        if "eval_count" in data:
            out.usage.completion_tokens = data["eval_count"]
        out.usage.total_tokens = (out.usage.prompt_tokens
                                  + out.usage.completion_tokens)
        return out

    def get_name(self) -> str:
        return "ollama-native"


def ensure_ollama() -> bool:
    try:
        httpx.get(f"{OLLAMA_URL}/api/version", timeout=2).raise_for_status()
        return True
    except Exception:
        print(f"Skipping: no Ollama at {OLLAMA_URL}. "
              f"Start with `ollama serve` and `ollama pull {MODEL}`.",
              file=sys.stderr)
        return False


def main() -> int:
    if not ensure_ollama():
        return 0  # env-skip

    provider = OllamaProvider()
    print(f"[ollama] using {MODEL} at {OLLAMA_URL} (native /api/chat path)")

    ctx = ng.NodeContext(
        provider=provider,
        instructions="Be concise. One short sentence per answer.",
    )

    graph_def = {
        "name": "ollama-native",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {
            "classify": {"type": "llm_call",
                "config": {"system": "Classify the user's question type in ONE word."}},
            "respond":  {"type": "llm_call",
                "config": {"system": "Answer concisely (one sentence)."}},
        },
        "edges": [
            {"from": ng.START_NODE, "to": "classify"},
            {"from": "classify",     "to": "respond"},
            {"from": "respond",      "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(graph_def, ctx)

    user_q = "What's 17 * 23?"
    print(f"[user] {user_q}")
    result = engine.run(ng.RunConfig(
        thread_id="ollama-native",
        input={"messages": [{"role": "user", "content": user_q}]},
    ))
    msgs = result.output["channels"]["messages"]["value"]
    for m in msgs:
        role = m.get("role", "?")
        content = m.get("content", "")
        if not content:
            continue
        print(f"  {role:>9}: {content}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
