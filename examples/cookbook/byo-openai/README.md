# Bring Your Own OpenAI Client

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

Use your existing `openai.OpenAI()` client inside a custom NeoGraph `GraphNode`.
The official SDK keeps its HTTP client, retry policy, headers, and SDK-level
instrumentation. NeoGraph schedules the node and applies its `ChannelWrite`
results to graph state.

[`hybrid.py`](hybrid.py) calls the SDK once and appends an assistant reply.
[`hybrid_with_tools.py`](hybrid_with_tools.py) runs the SDK's tool-calling loop
inside one node, executes three Python functions, and appends the final reply.
Both examples use OpenRouter and default to `~deepseek/deepseek-v4-flash-latest`.
They send `provider={"zdr": true}` to request OpenRouter's zero-data-retention
routing policy. This preference does not establish a geographic residency
policy.

## Prerequisites and local run

Install the wheel built from the current typed-provider checkout and the
`openai` package. Older releases that expose the removed completion API cannot
run these examples. Start a local Chat Completions protocol peer before running
the commands below from this directory. These are verification instructions,
not a recorded successful run.

```bash
python -m pip install openai
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python hybrid.py
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python hybrid_with_tools.py
```

`OPENROUTER_BASE_URL` includes the API prefix: `/v1` for this local peer and
`/api/v1` for OpenRouter. The SDK adds `/chat/completions`. Canonical loopback
hosts `127.0.0.1` and `::1` use the fixed dummy credential `local-smoke`, even if
a hosted key exists in the environment. Other hosts require HTTPS,
`NG_ALLOW_HOSTED_CALLS=1`, and `OPENROUTER_API_KEY`; without that opt-in the
program exits with status 2 before a request. Hosted calls may cost money.
The examples optionally read an existing `.env` without replacing exported
variables. Do not commit keys or log request authorization headers.

## Graph state and SDK requests

Each graph has one custom node between `START_NODE` and `END_NODE`. A scoped
`GraphRegistry` registers the node type; no global provider subclass or
completion trampoline is involved. The node reads the `messages` channel,
prepends its system instruction to the SDK request, and returns channel writes.
The append reducer keeps the input user message and then the assistant message.
The system message remains request-local.

For `hybrid.py`, the peer accepts one buffered `POST /v1/chat/completions` with
`model="fixture-model"`, a system message, a user message, `temperature=0.7`,
and `provider={"zdr": true}`. Return a standard Chat Completion JSON object with
`id`, `object="chat.completion"`, `created`, `model`, and one `choices` entry
containing `index=0`, an assistant message, and `finish_reason="stop"`.
`usage` is optional. Expected state has two messages; `sdk_usage` contains the
SDK's usage dictionary or `None`, never invented zero counters. Tool calls
fail this text-only node rather than being discarded.

## Tool loop

For `hybrid_with_tools.py`, the first request also declares `reverse_string`,
`word_count`, and `calc` as function tools. A deterministic peer can return
three assistant tool calls with distinct ids and JSON argument strings:
`{"s":"NeoGraph"}`, `{"text":"the quick brown fox"}`, and
`{"expr":"17*23+5"}`. Set `finish_reason="tool_calls"`.

The node appends the assistant tool-call message to its SDK-local history,
executes each function, and appends tool results with the matching
`tool_call_id`. The second request must contain results `hparGoeN`, `4`, and
`396`. Return an assistant text choice with `finish_reason="stop"` and no tool
calls. Expected graph state contains only the original user and final assistant
messages, `tool_calls=3`, and a two-entry `sdk_usage` list. Each entry is the
corresponding response's usage dictionary or `None`; the final call's usage
cannot stand for the whole loop. The program prints three dispatched tools and
two SDK calls for this exchange.

Tool exceptions become tool-result error text so the model can respond. Eight
consecutive tool-call responses exhaust the cap and raise an error instead of
writing a fabricated final answer. `calc` uses Python expression evaluation for
this arithmetic demonstration; it is not a sandbox for untrusted expressions.

## Provider evidence boundary

These nodes write application-owned JSON state. They do not produce
`ProviderOutcome`, native replay authority, provider receipts, or per-tool
checkpoints. SDK retries and intermediate tool calls remain inside the node;
a graph checkpoint is not a durable receipt for those requests. The client is
closed after the run.

When you need NeoGraph's typed provider outcomes and native SDK transport, use
the [OpenRouter SchemaProvider example](../openrouter-provider/README.md).
Passing an existing SDK client to these custom nodes preserves that client's
configuration; it does not make the client a `SchemaProvider`.
