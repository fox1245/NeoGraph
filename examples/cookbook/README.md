# NeoGraph Cookbooks

End-to-end recipes that compose multiple NeoGraph features into a
real working scenario. Each one is self-contained: copy the folder,
follow its README, and run.

| Cookbook | What it shows |
|---|---|
| [`ai-assembly/`](ai-assembly/) | Multi-persona A2A: 4 국회의원 (each its own A2A endpoint) + a Speaker that broadcasts a bill in parallel and tallies votes. Cross-language: C++ member servers + Python or C++ Speaker. |
| [`byo-openai/`](byo-openai/) | Bring your own `openai.OpenAI()` client: subclass NeoGraph's `Provider` to delegate every LLM call into the SDK, keeping all your retries / Azure / observability config. Also: tool calling via the agentic-provider pattern. |
| [`ollama-provider/`](ollama-provider/) | Local LLM via Ollama. Two paths: built-in `OpenAIProvider` against Ollama's compat endpoint (zero new code), or a custom `Provider` against the native `/api/chat`. Full agent stack with no external API keys. |

Each cookbook also documents the friction it surfaced — useful for
finding the rough edges of the public API.
