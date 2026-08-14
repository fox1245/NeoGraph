# NeoGraph + OpenRouter (pinned DeepSeek)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

Run NeoGraph graphs against OpenRouter's pinned
`~deepseek/deepseek-v4-flash-latest` model. This cookbook shows two equivalent
provider surfaces, both using the same API key, model, and explicit
zero-data-retention (ZDR) provider preference:

1. the built-in `OpenAIProvider` against OpenRouter's OpenAI-compatible chat
   endpoint (`via_openai_compat.py`);
2. a custom Python `Provider` that posts the normalized chat request directly
   (`via_http.py`).


The second path demonstrates that NeoGraph's `Provider` trampoline is
transport-agnostic while keeping the wire contract explicit.

## Path A — built-in provider

`OpenAIProvider` appends `/v1/chat/completions` itself, so pass the bare
OpenRouter API base URL (`https://openrouter.ai/api`), not the `/v1` suffix:

```python
from neograph_engine.llm import OpenAIProvider

provider = OpenAIProvider(
    api_key=os.environ["OPENROUTER_API_KEY"],
    base_url="https://openrouter.ai/api",
    default_model="~deepseek/deepseek-v4-flash-latest",
    provider_routing={"zdr": True},
```

See [`via_openai_compat.py`](via_openai_compat.py).

## Path B — custom direct HTTP provider

[`via_http.py`](via_http.py) subclasses `Provider`, posts to
`https://openrouter.ai/api/v1/chat/completions`, maps `choices[0].message`
back to `ChatCompletion`, and copies token usage into NeoGraph's response.
The request also asks OpenRouter for its ZDR provider preference.

## Run

```bash
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
pip install neograph-engine>=0.2.3 httpx
python via_openai_compat.py
python via_http.py
```

Both demos build a strict one-node graph and route its `llm_call` through the
same pinned DeepSeek model. The shared `NodeContext.instructions` supplies
the system prompt. A missing key exits with a clear message; no mock provider
silently changes the live-provider path.

## Output shape

```text
[openrouter] using ~deepseek/deepseek-v4-flash-latest via OpenAI-compatible path
[user] What's the capital of France?
  assistant: Paris is the capital of France.
```

Actual completions vary. The direct HTTP path prints `via direct HTTP` and
uses its own arithmetic question.

## Why keep both paths?

- Path A uses NeoGraph's native HTTP implementation and connection pooling.
- Path B is the smallest example for custom headers, transport policy, or
  response translation owned by application code.
- Both paths use the exact same OpenRouter endpoint family, API key, and
  pinned DeepSeek model, so provider-surface comparisons are meaningful.

OpenRouter API request and response shape:
<https://openrouter.ai/docs/api-reference/overview>
