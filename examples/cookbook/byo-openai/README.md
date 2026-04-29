# Bring Your Own OpenAI Client

Most production Python users already have an `openai.OpenAI()` client
instance with their own retries, custom transport, observability hooks,
or Azure / Bedrock / Groq routing. This cookbook shows how to plug
that existing client into NeoGraph as a custom `Provider` — instead of
NeoGraph's built-in `OpenAIProvider`.

The trick: NeoGraph's `Provider` is Python-subclassable in v0.2.3+.
A subclass's `complete(params)` runs inside graph nodes (LLMCallNode,
ReAct loops, etc.) just like the built-in provider.

## When to use which

| You want | Use |
|---|---|
| "Just give me the fastest path to OpenAI" | `neograph_engine.llm.OpenAIProvider` (native asio + connection pool, ~1.5× faster than the SDK at high RPS) |
| "I already have an `openai.OpenAI()` set up with retries / Azure / proxy / hooks" | This cookbook (subclass `Provider`, delegate to your client) |
| "I'm using LangChain / Anthropic / Bedrock / Groq" | This cookbook with the relevant SDK |
| "I want to mock the LLM in tests" | This cookbook with a deterministic stub |

The point: **NeoGraph's graph engine doesn't care how the LLM call
happens** — it just needs `params -> ChatCompletion`.

## The whole thing in 60 lines

See [`hybrid.py`](hybrid.py). The key shape:

```python
import neograph_engine as ng
from openai import OpenAI

class OpenAISdkProvider(ng.Provider):
    """NeoGraph Provider backed by the official `openai` SDK."""
    def __init__(self, client: OpenAI, model: str = "gpt-5.4-mini"):
        super().__init__()
        self.client = client
        self.model  = model

    def complete(self, params: ng.CompletionParams) -> ng.ChatCompletion:
        # Translate NeoGraph params into the SDK's chat-completions shape.
        messages = [{"role": m.role, "content": m.content}
                    for m in params.messages]
        resp = self.client.chat.completions.create(
            model=params.model or self.model,
            messages=messages,
            temperature=params.temperature,
        )
        # Translate back into NeoGraph's response shape.
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = resp.choices[0].message.content or ""
        return out

    def get_name(self) -> str:
        return "openai-sdk"
```

That's it. Pass `OpenAISdkProvider(OpenAI(api_key=...))` into a
`NodeContext` and any NeoGraph graph using `llm_call` nodes will route
through the SDK — keeping all your retry / Azure / observability / proxy
configuration that was attached to the SDK client.

## Run

```bash
pip install neograph-engine>=0.2.3 openai
echo 'OPENAI_API_KEY=sk-...' > .env
python hybrid.py
```

Output:
```
[hybrid] using openai SDK 2.33.0 inside NeoGraph 0.2.3 graph
[hybrid] running 3-node graph: classify → respond → summarise
[provider] complete() call #1 (system + 1 user) — model=gpt-5.4-mini
[provider] complete() call #2 (system + 2 user) — model=gpt-5.4-mini
[provider] complete() call #3 (system + 3 user) — model=gpt-5.4-mini
[hybrid] final summary: ...
```

## What you keep

- Your `openai.OpenAI()` client's `default_headers`, retry policy,
  custom `http_client=httpx.Client(...)`, Azure / proxy config.
- `OpenAIObservabilityCallbacks` / `langfuse` / `helicone` /
  `weights & biases` integrations attached at SDK level — they
  intercept every call.
- Your existing tracking of `usage` (token counts), errors, retries.

## What you give up vs `neograph_engine.llm.OpenAIProvider`

- The native HTTP path (asio + connection pool) — at ~1.5× faster than
  the SDK and zero GIL contention. If your bottleneck is OpenAI calls,
  the SDK is fine; if it's framework overhead, the native one wins.

## Tool calling — three working patterns

The Provider trampoline lets `complete()` return `tool_calls` cleanly.
What's currently **not working** is the C++ `tool_dispatch` graph node
calling back into a Python `Tool` subclass — that path segfaults
(pre-existing issue; tracked for v0.3). Three patterns work today:

### A. Agentic Provider (recommended for `byo-openai`)

Do the tool loop **inside** `complete()`. The user's `openai.OpenAI`
client already supports tool-calling; let it finish the agentic loop
(call → dispatch in Python → result → call → text) and return only
the final assistant message to NeoGraph. The graph sees exactly one
`complete()` per "turn", no `tool_dispatch` node needed.

```python
class AgenticOpenAIProvider(ng.Provider):
    def __init__(self, client, tools_by_name):
        super().__init__()
        self.client = client
        self.tools  = tools_by_name      # {"calc": calc_fn, ...}
    def complete(self, params):
        messages = [{"role": m.role, "content": m.content} for m in params.messages]
        sdk_tools = [{"type":"function",
                      "function":{"name":n,"description":fn.__doc__ or "",
                                  "parameters":fn.schema}}
                     for n, fn in self.tools.items()]
        for _ in range(10):  # cap loops
            r = self.client.chat.completions.create(
                model=params.model or "gpt-5.4-mini",
                messages=messages, tools=sdk_tools)
            choice = r.choices[0]
            if not choice.message.tool_calls:
                out = ng.ChatCompletion()
                out.message.role    = "assistant"
                out.message.content = choice.message.content or ""
                return out
            messages.append(choice.message.model_dump())
            for tc in choice.message.tool_calls:
                fn = self.tools[tc.function.name]
                result = fn(**stdjson.loads(tc.function.arguments))
                messages.append({"role":"tool","tool_call_id":tc.id,
                                 "content":str(result)})
```

Tradeoff: NeoGraph doesn't see intermediate steps (no checkpoint per
tool call), but you keep all SDK behavior and there's no dispatch
boundary friction.

### B. C++ tools + Python Provider

Use the built-in C++ tools (`MCPTool` from `neograph_engine.mcp`,
or any other C++-side `Tool`) for the dispatch path, and your Python
Provider for the LLM call. The graph's `tool_dispatch` node calls
the C++ tool fine; only the call back into a Python `Tool` subclass
crashes.

### C. Provider returns tool_calls; custom Python node dispatches

Skip the built-in `tool_dispatch` node. Write your own
`@ng.node("dispatch")` that reads `messages[-1].tool_calls`, calls
your Python tools directly, and writes the tool-result messages
back. Stays entirely in Python.

## A2A + custom Provider

This cookbook composes naturally with the
[ai-assembly cookbook](../ai-assembly/) — replace each member's
provider with `OpenAISdkProvider(...)` to get all your SDK-level
behavior on every persona while still using NeoGraph's A2A bridge.
