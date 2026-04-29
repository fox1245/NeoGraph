"""NeoGraph + Ollama via the OpenAI-compatible endpoint.

Ollama exposes /v1/chat/completions with the same shape as OpenAI's
API, so NeoGraph's built-in `OpenAIProvider` works against it
unchanged — just point `base_url` at the local Ollama server.

Run:
    ollama serve                              # in another terminal
    ollama pull qwen2.5:0.5b
    python via_openai_compat.py
"""

import sys
import urllib.request

import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider

OLLAMA_URL = "http://127.0.0.1:11434"
MODEL      = "qwen2.5:0.5b"   # small, fast, good for demos


def ensure_ollama() -> bool:
    try:
        with urllib.request.urlopen(OLLAMA_URL + "/api/version", timeout=2) as r:
            r.read()
        return True
    except Exception:
        print(f"Skipping: no Ollama at {OLLAMA_URL}. "
              f"Start with `ollama serve` and `ollama pull {MODEL}`.",
              file=sys.stderr)
        return False


def main() -> int:
    if not ensure_ollama():
        return 0  # env-skip

    # OpenAIProvider appends `/v1/chat/completions` to base_url itself,
    # so we pass the bare host — NOT host + "/v1". Otherwise the
    # request lands at /v1/v1/chat/completions and Ollama 404s.
    provider = OpenAIProvider(
        api_key="sk-anything",                # required field; Ollama ignores it
        base_url=OLLAMA_URL,                  # NOT OLLAMA_URL + "/v1"
        default_model=MODEL,
        timeout_seconds=120,                  # cold-start can be slow on first hit
    )
    print(f"[ollama] using {MODEL} at {OLLAMA_URL} (OpenAI-compat path)")

    ctx = ng.NodeContext(
        provider=provider,
        instructions="Be concise. One short sentence per answer.",
    )

    graph_def = {
        "name": "ollama-compat",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {
            "classify": {"type": "llm_call",
                "config": {"system": "Classify the user's question type in ONE word."}},
            "respond": {"type": "llm_call",
                "config": {"system": "Answer the user concisely (one sentence)."}},
        },
        "edges": [
            {"from": ng.START_NODE, "to": "classify"},
            {"from": "classify",     "to": "respond"},
            {"from": "respond",      "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(graph_def, ctx)

    user_q = "What's the capital of France?"
    print(f"[user] {user_q}")
    result = engine.run(ng.RunConfig(
        thread_id="ollama-compat",
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
