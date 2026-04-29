# NeoGraph + Ollama (local LLM)

Run NeoGraph graphs against a locally-served model on
[Ollama](https://ollama.com) — no API keys, no network egress, full
privacy. Two paths, both working with NeoGraph 0.2.3+:

## Path A — `OpenAIProvider` against Ollama's compat endpoint *(zero new code)*

Ollama exposes `/v1/chat/completions` with the OpenAI request/response
shape. Point NeoGraph's built-in `OpenAIProvider` at it and you're
done — same native asio HTTP path, same connection pool, same speed.

```python
from neograph_engine.llm import OpenAIProvider
# OpenAIProvider appends `/v1/chat/completions` itself, so base_url
# is the bare host — NOT host + "/v1". Ollama returns 404 otherwise.
provider = OpenAIProvider(
    api_key="sk-anything",                       # required field, value ignored
    base_url="http://127.0.0.1:11434",            # NOT ".../v1"
    default_model="qwen2.5:0.5b",
)
```

That's the entire integration. See [`via_openai_compat.py`](via_openai_compat.py).

**When to pick Path A**: you want speed, you don't care about Ollama-
specific features (model pull/load, streaming with native fields,
`keep_alive` and `options` blocks). 95% of users.

## Path B — Native Ollama API via custom Python `Provider`

Demonstrates that the v0.2.3 `Provider` trampoline is genuinely
vendor-agnostic. Wraps Ollama's native `/api/chat` (richer parameters,
no compat-layer translation), shipping as
[`via_native_api.py`](via_native_api.py).

```python
class OllamaProvider(ng.Provider):
    def __init__(self, model="qwen2.5:0.5b", host="http://127.0.0.1:11434"):
        super().__init__()
        self.model = model
        self.host = host
    def complete(self, params):
        r = httpx.post(f"{self.host}/api/chat", json={
            "model": params.model or self.model,
            "messages": [{"role": m.role, "content": m.content}
                         for m in params.messages],
            "stream": False,
            "options": {"temperature": params.temperature},
        }, timeout=120)
        r.raise_for_status()
        body = r.json()
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = body["message"]["content"]
        return out
    def get_name(self): return "ollama"
```

**When to pick Path B**: you need Ollama-native fields (`options`,
`keep_alive`, `format=json` constraint), or your team already uses the
`ollama` Python SDK and you want to keep that as your client surface.

## Run

```bash
# 1. start Ollama (separate terminal)
ollama serve

# 2. pull a small model so it fits in CI / laptop RAM
ollama pull qwen2.5:0.5b      # ~400 MB

# 3. install NeoGraph + httpx (Path B uses httpx)
pip install neograph-engine>=0.2.3 httpx

# 4. run either path
python via_openai_compat.py
python via_native_api.py
```

Both demos build a 2-node graph (`classify → respond`) and route both
LLM calls through the local model. No external API key needed.

## Output

```
[ollama] using qwen2.5:0.5b at http://127.0.0.1:11434
[graph] step 1 — classify: question type
[graph] step 2 — respond:  one-sentence answer
```

(Actual completions vary with the model.)

## Notes

- **Why `qwen2.5:0.5b`?** Smallest mainstream model that handles
  English + simple reasoning. Loads in 2-3 s on cold start, then
  ~100 ms / completion on CPU. Good fit for a cookbook demo. Swap to
  `llama3.2:3b` / `qwen2.5:7b` / `phi4:14b` once you've confirmed the
  loop works.
- **First call is slow** — Ollama lazily loads the model into memory
  on first request (~2-5 s for sub-1B). Subsequent calls are warm.
  The `keep_alive` parameter (Path B) controls how long the model
  stays loaded after idle.
- **Tool calling**: Ollama's tool-calling support is model-dependent
  and uses the OpenAI-compat shape via the `/v1/chat/completions`
  endpoint. Use Path A + the agentic-provider pattern from
  [`../byo-openai/hybrid_with_tools.py`](../byo-openai/hybrid_with_tools.py)
  with a tool-capable Ollama model (e.g. `qwen2.5:7b`).
- **Streaming**: Path A inherits NeoGraph's native streaming via
  `OpenAIProvider.complete_stream`. Path B's example doesn't stream;
  add `"stream": True` + a chunk loop if you need it.

## Why this matters

Combining NeoGraph (5 µs engine overhead) with a local Ollama model
gives you the entire agent stack with **zero external dependencies**:

- 13 MB native binary + ~400 MB model = **fits on a Raspberry Pi 5**
- No API key, no rate limits, no network egress, full data privacy
- LangGraph parity in graph semantics; Ollama-quality models for the
  LLM call
- Per-iteration overhead at the engine layer is in the noise (the
  500-2000 ms model inference dominates), but framework footprint is
  meaningfully smaller than LangGraph + Python runtime

Suitable for: edge AI, on-device agents, privacy-required deployments,
self-hosted assistants, and anyone tired of OpenAI bills for early
prototyping.
