# NeoGraph Cookbooks

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## Typed C++ cutover status

The current C++ recipes use owned `ProviderRequest` payloads from the five typed
SDK families, ordered `sp::Event` observations and immutable
`std::shared_ptr<const sp::Outcome>` terminals (`Completion` or `Failure`).
`ChatMessage` / `ChatTool` are portable projections, not native replay authority.
Prepare exactly once; durable callers bind their claim/receipt to
`Provider::request_digest(prepared)` and dispatch that same prepared handle.
Do not reconstruct history from printed JSON or flatten failures to final text.

Canonical persistence uses `provider-message-v2` and `runtime-history-record-v2`;
portable summaries never replace native records. Optional controls are caller-chosen,
not silently clamped. Bounded calls require genuine model facts; missing facts are
`LimitUnknown`. Reservation/charged/held amounts are distinct from nullable provider
usage and do not renew a budget or represent a price, forecast or invoice.

The header-only `examples/provider_example_support.h` helper uses the real SDK
runtime; it is not a replacement transport. LLM builds require
`SchemaProvider::runtime` through `find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)` or an
explicit `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>` configuration.
Installed SDK include root is `include/SchemaProvider`. Interface/capability
checks are enforced; the SDK package remains unstable `0.0.0` (interface 3).

Current model-free execution covered the four local Assembly A2A member servers
and C++ speaker, JARVIS CLI synthetic turns with persisted memory, Beast strict
Core compilation/evolution/checkpoint rollback, and ProgramChat browser tenant
isolation/generation replacement plus six PostgreSQL black-box scenarios.
The inventory below separates these scoped runs from unexercised surfaces.
They do not qualify vendor inference, voice, deferred Python bindings, every
Beast live variant, or the dedicated multitenant server/load scenario.
Historical timings below are not new-cutover qualification. Live runs incur
provider charges and require explicit keys/network/model access. Keep keys,
prompts and artifacts private; envelope/native inspection demos print sensitive
payloads and must not feed public logs. A native archive provides authenticated,
owner-private custody, not encryption or vendor-issuer authentication.


End-to-end recipes that compose multiple NeoGraph features into a
real working scenario. Each one is self-contained: copy the folder,
follow its README, and run.

| Cookbook | What it shows |
|---|---|
| [`the-beast/`](the-beast/) | **A self-evolving agent: generate · evolve · roll back.** The Beast authors strict Core JSON, validates it before execution, evolves its bounded Core topology with `evolve()`, and rolls back through checkpoints. The live, apex, forge, script, and arithmetic-evolution variants retain the same compiler/validation boundary; JavaScript or trusted C++ owns source authoring, while strict Core JSON remains interchange data. |
| [`ai-assembly/`](ai-assembly/) | Multi-persona A2A: 4 members of the National Assembly (each its own A2A endpoint) + a Speaker that broadcasts a bill in parallel and tallies votes. Cross-language: C++ member servers + Python or C++ Speaker. |
| [`byo-openai/`](byo-openai/) | Historical provider recipes; Python binding migration deferred. |
| [`jarvis/`](jarvis/) | **Voice-driven meta-orchestrator (skeleton).** Mic → whisper.cpp (auto-detect language) → router (direct / delegate / parallel 3-way) → MCP tools or A2A specialists → supertonic on-device TTS, in the user's detected language. JSON-driven tool + agent catalogs, A2A bidirectional (JARVIS is itself reachable). On-device, zero cloud required. |
| [`minimal-mcp/`](minimal-mcp/) | MCP client round-trip with **no LLM, no API key, no fastmcp**: a ~60-line stdlib stdio server + a C++ harness that does `initialize` → `tools/list` → `tools/call`. Shows NeoGraph's MCP client only needs a process that speaks the wire protocol — the peer can be anything. |
| [`openrouter-provider/`](openrouter-provider/) | Historical provider recipes; Python binding migration deferred. |

Each cookbook also documents the friction it surfaced — useful for
finding the rough edges of the public API.

## Complete recipe inventory and deferred surfaces

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | Typed C++ migration; four actual local A2A member servers and C++ speaker exercised offline; synthetic abstentions are not model judgment |
| [`byo-openai/`](byo-openai/) | Historical provider recipes; Python binding migration deferred |
| [`jarvis/`](jarvis/) | Typed C++ migration; CLI synthetic turn, persisted memory and graceful EOF exercised; voice/Python surfaces not qualified |
| [`minimal-mcp/`](minimal-mcp/) | Protocol-only client/server; intentionally unchanged |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | Typed C++ migration; dedicated server execution and live 1,000/32 load not qualified |
| [`openrouter-provider/`](openrouter-provider/) | Historical provider recipes; Python binding migration deferred |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | Typed C++ migration; ProgramChat browser tenant isolation/generation replacement and six PostgreSQL black-box scenarios exercised without vendor inference |
| [`the-beast/`](the-beast/) | Typed C++ migration; actual strict Core compilation/evolution/checkpoint rollback exercised; not a pass claim for all live variants |
| [`topology-retrieval/`](topology-retrieval/) | Protocol-only client/server; intentionally unchanged |

Python MCP servers, Jarvis CLI/REPL drivers and retrieval HTTP clients remain protocol clients; they do not implement provider bindings. Assembly Python speaker and Jarvis pybind benchmark depend on deferred bindings. Live multitenant 1,000/32 is not a smoke run.
