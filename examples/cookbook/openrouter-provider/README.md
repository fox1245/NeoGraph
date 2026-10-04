# NeoGraph + OpenRouter

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

This cookbook keeps two request paths to OpenRouter's Chat Completions API.
Both default to `~deepseek/deepseek-v4-flash-latest` and send
`provider={"zdr": true}` to request zero-data-retention routing. This setting
is not a geographic residency guarantee.

[`via_openai_compat.py`](via_openai_compat.py) uses the native `SchemaProvider`
and built-in `llm_call` node. [`via_http.py`](via_http.py) uses a custom Python
`GraphNode` that owns the HTTP request and response mapping through `httpx`.
They share the endpoint/credential helper, but not their transport or outcome
representation. The compatibility example does not require `httpx`.

## Path A: typed SchemaProvider

The compatibility path calls `load_provider_descriptor` with a closed,
versioned `openai.chat` descriptor. `connection.base_url` contains the origin,
while `connection.paths` contains the complete API path. For OpenRouter these
are `https://openrouter.ai` and `/api/v1/chat/completions`. Unknown descriptor
fields are rejected; arbitrary JSON cannot inject codec behavior.

A typed `OpenRouterRouting` value sets `zdr=True` in `SchemaProviderDefaults`.
`ProviderRuntimeOptions` supplies the API key, a 120-second timeout, and an
optional `OPENROUTER_CA_FILE`. The runtime uses libcurl; there is no HTTP2 or
WebSocket transport selector. `NodeContext` supplies the explicit model and
system instruction. `RunConfig.provider_messages` supplies a typed
`ProviderMessage` with a `Text` part. The built-in node creates a typed request,
prepares it, and dispatches it through the provider.

For loopback smoke, the example reads `provider_policy_json()`, adds the exact
loopback origin to the `openai.chat` family's `openrouter_origins`, and passes
the full family and codec-resource data through `load_provider_policy` before
loading the descriptor. The fixture origin is declared policy data; no runtime
endpoint override or skipped ZDR validation is involved.

Successful results retain immutable provider outcomes and typed message
history. `outcome.completion` holds the completion; `outcome.failure` holds a
failure instead. A missing usage counter is `None`; an observed zero is a
`UsageCount` with `value=0`. The example prints assistant text and available
input/output counts without dumping raw payloads or keys. Provider failure is
not converted into a successful answer.

## Path B: custom HTTP node

`OpenRouterHttpNode` reads graph messages, prepends its system instruction,
posts JSON to the selected endpoint, and maps `choices[0].message` to an
ordinary `ChannelWrite`. It keeps custom HTTP headers, timeout policy, and
response translation in application code. The append reducer retains the user
then assistant messages. `http_usage` stores the response's usage dictionary,
or `None` when absent. The `httpx` client closes after the run.

This path does not manufacture `ProviderOutcome`, prepared-request authority,
native replay history, or typed provider usage. HTTP errors fail the node; tool
calls fail this text-only example rather than being dropped. Use the
[BYO OpenAI SDK cookbook](../byo-openai/README.md) when an existing official SDK
client should own the call and tool loop.

## Prerequisites and local run

Install the wheel built from the current typed-provider checkout. Older
completion-API releases cannot run these examples. Install `httpx` for Path B,
then start a local Chat Completions peer before running these commands from this
directory. These are verification instructions, not a recorded successful run.

```bash
python -m pip install httpx
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python via_openai_compat.py
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python via_http.py
```

`OPENROUTER_BASE_URL` includes the API prefix; both paths append
`/chat/completions`. Canonical loopback hosts `127.0.0.1` and `::1` use the fixed
dummy credential `local-smoke`, even when a hosted key exists in the environment.
Other hosts require HTTPS, `NG_ALLOW_HOSTED_CALLS=1`, and `OPENROUTER_API_KEY`.
Without opt-in, each program exits with status 2 before any request. Hosted
calls may cost money. The optional existing `.env` does not overwrite exported
variables. Never commit keys or log Authorization headers. A non-OpenRouter
host also needs explicit routing-policy admission in Path A; merely opting in
does not grant it OpenRouter semantics.

## Local protocol and expected state

The peer accepts buffered `POST /v1/chat/completions` with a system message,
a user message, `model="fixture-model"`, and `provider={"zdr": true}`. Path B
also sends `temperature=0.7`. Path A asks for the capital of France; Path B
asks for `17 * 23`. Example peer response for Path A:

```json
{
  "id": "fixture-chat-1",
  "object": "chat.completion",
  "created": 0,
  "model": "fixture-model",
  "choices": [{
    "index": 0,
    "message": {"role": "assistant", "content": "Paris."},
    "finish_reason": "stop"
  }],
  "usage": {"prompt_tokens": 8, "completion_tokens": 2, "total_tokens": 10}
}
```

For Path A, expect one successful real provider outcome, a user and assistant
in `result.provider_messages`, and the assistant reply in graph state. Usage
counts for this fixture are input 8 and output 2. Omit `usage` to exercise
unknown counts; do not expect fabricated zeroes. For Path B, return `"391"`
as assistant content and expect two graph messages plus the returned
`http_usage` dictionary. Neither example silently switches to a mock provider.

OpenRouter's request/response reference:
<https://openrouter.ai/docs/api-reference/overview>
