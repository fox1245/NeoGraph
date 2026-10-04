# NeoGraph Cookbooks

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## Typed C++ cutover status

The current C++ recipes use owned `ProviderRequest` payloads from the five typed
SDK families, ordered `sp::Event` observations and immutable
`std::shared_ptr<const sp::Outcome>` terminals (`Completion` or `Failure`).
`ChatMessage` / `ChatTool` are portable projections, not native replay authority.
Prepare exactly once; durable callers bind their claim/receipt to
`Provider::request_digest(prepared)` and dispatch that same prepared handle.
A stopped local wait does not prove the remote model stopped or will not bill.
Durable receipts prevent automatic redispatch; they do not promise exactly-once
external effects.
Do not reconstruct history from printed JSON or flatten failures to final text.

Canonical persistence uses `provider-message-v2` and `runtime-history-record-v2`;
portable summaries never replace native records. Optional controls are caller-chosen,
not silently clamped. Bounded calls require genuine model facts; missing facts are
`LimitUnknown`. Reservation/charged/held amounts are distinct from nullable provider
usage and do not renew a budget or represent a price, forecast or invoice.

The header-only `examples/provider_example_support.h` helper uses the real SDK
runtime. Every native build, including Core-only builds, requires
`SchemaProvider::runtime`. CMake uses an installed package, an
explicit `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>`, or the immutable
public SDK archive fallback. `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` disables
fetching when using an offline installed/source SDK.
Installed SDK include root is `include/SchemaProvider`. Interface/capability
checks are enforced; SDK `0.1.0` alpha uses interface/shared ABI 4.

The preserved interface-3 model-free execution covered the four local Assembly A2A member servers
and C++ speaker, JARVIS CLI synthetic turns with persisted memory, Beast strict
Core compilation/evolution/checkpoint rollback, dedicated mock topology load,
retrieval index reuse/admission, and ProgramChat browser tenant isolation/
generation replacement plus six SQLite and six PostgreSQL black-box scenarios.
The inventory below separates these scoped runs from unexercised surfaces.
They do not qualify vendor inference, voice, the migrated Python bindings, every
Beast live variant, or the dedicated live multitenant 1,000/32 workload.
Historical timings below are not new-cutover qualification. Live runs incur
provider charges and require explicit keys/network/model access. Keep keys,
prompts and artifacts private; envelope/native inspection demos print sensitive
payloads and must not feed public logs. A native archive provides authenticated,
owner-private custody, not encryption or vendor-issuer authentication.


These recipes compose multiple NeoGraph features. Build C++ targets from the
NeoGraph tree with the required SDK package; copying a recipe folder alone does
not provide a standalone build.

| Cookbook | What it shows |
|---|---|
| [`the-beast/`](the-beast/) | **A self-evolving agent: generate · evolve · roll back.** The Beast authors strict Core JSON, validates it before execution, evolves its bounded Core topology with `evolve()`, and rolls back through checkpoints. The live, apex, forge, script, and arithmetic-evolution variants retain the same compiler/validation boundary; JavaScript or trusted C++ owns source authoring, while strict Core JSON remains interchange data. |
| [`ai-assembly/`](ai-assembly/) | Multi-persona A2A: 4 members of the National Assembly (each its own A2A endpoint) + a Speaker that broadcasts a bill in parallel and tallies votes. Cross-language: C++ member servers + Python or C++ Speaker. |
| [`byo-openai/`](byo-openai/) | Python provider preparation through the authentic typed SDK path; see the recipe for compatibility and execution evidence. |
| [`jarvis/`](jarvis/) | Voice-driven meta-orchestrator. Optional local ASR/TTS surrounds a four-way router (chat/direct/delegate/parallel), MCP tools, A2A specialists and stored conversation memory. Local/mock operation is cloud-free; live inference uses OpenRouter. |
| [`minimal-mcp/`](minimal-mcp/) | MCP client round-trip with **no LLM, no API key, no fastmcp**: a ~60-line stdlib stdio server + a C++ harness that does `initialize` → `tools/list` → `tools/call`. Shows NeoGraph's MCP client only needs a process that speaks the wire protocol — the peer can be anything. |
| [`openrouter-provider/`](openrouter-provider/) | Typed Python OpenRouter provider requests and immutable outcomes; see the recipe for compatibility and execution evidence. |

Each cookbook also documents the friction it surfaced — useful for
finding the rough edges of the public API.

## Complete recipe inventory and evidence limits

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | Typed C++ migration; four actual local A2A member servers and C++ speaker exercised offline; synthetic abstentions are not model judgment |
| [`byo-openai/`](byo-openai/) | Python source migration uses authentic prepared handles; historical measurements do not qualify the migrated implementation |
| [`jarvis/`](jarvis/) | Typed C++ migration; CLI synthetic turn, persisted memory and graceful EOF exercised; voice/Python surfaces not qualified |
| [`minimal-mcp/`](minimal-mcp/) | Actual stdio handshake/discovery and arithmetic/UTC/demo-weather calls exercised; no LLM |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | Dedicated mock 1,000 requests: 0 errors, 3 compiled topologies, 997 cache hits; isolated host emits reference metadata only; live 1,000/32 unqualified |
| [`openrouter-provider/`](openrouter-provider/) | Typed Python source migration; historical measurements do not qualify the migrated implementation |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | Actual browser tenant isolation/generation replacement, six SQLite and six PostgreSQL black-box scenarios; explicit host model policy; no vendor inference |
| [`the-beast/`](the-beast/) | Typed C++ migration; actual strict Core compilation/evolution/checkpoint rollback exercised; not a pass claim for all live variants |
| [`topology-retrieval/`](topology-retrieval/) | Mock Python ranking/index reuse plus actual C++ registry admission/migration and unknown-key rejection; external pointer is not authority |

Python MCP servers, Jarvis CLI/REPL drivers and retrieval HTTP clients remain protocol clients; they do not implement provider bindings. The Assembly Python speaker uses the A2A binding; Jarvis pybind benchmarks use the migrated native bindings. Their execution evidence is separate from the preserved C++ runs above. Live multitenant 1,000/32 is not a smoke run. Current SDK runtime qualification is Linux/POSIX; these recipes do not establish macOS or Windows transport qualification.

## Interface-4 controls and retained call limits

`ProviderControls` preserves family-specific reasoning, sampling and tool selection:
Chat has the admitted OpenRouter reasoning object, usage/include controls and
alternative models; Responses has explicit provider-held continuation, verbosity,
truncation and include selections; Messages has manual/adaptive/disabled thinking,
effort, cache controls and tool choice; Gemini has explicit portable foreign history,
thinking level, safety settings, sampling and tool choice. Admission rejects
unsupported family/origin/model combinations before I/O. Native messages still
require authentic custody; printed JSON, cursors and portable projections do not
create it. Use the [provider reference](../../docs/reference-en.md) for exact types.

Forge requests low reasoning effort and pins one 300-second deadline per ask.
Only a completed empty `MaxTokens` reply without text or valid/invalid client calls
can cause one additional call with twice the current output cap. Both outcomes
and usage remain retained; failures and observer/settlement errors are not retried.
The older live chatbot paths `multi_tenant_chatbot/server_live_llm.cpp` and
`self_evolving_chatbot/server_multi.cpp` use 180-second provider timeouts.
This does not change ProgramChat's separate CLI limits or the shared factory default.

The [numbered-example contracts](../README.md#retained-example-contracts) detail
Deep Research's bounded cap ladder and deadline discovery, examples 16/28,
fork/new-turn behavior, computed replay savings, evolution file mode and A2A
0.x/1.0 snapshots. These source updates do not relabel the preserved runs above
as interface-4 or live-provider passes.
