# Multi-tenant Chatbot Server

## Current source boundary and historical measurements

The mock is provider-free: its cache binds topology, model, instructions, extra
configuration and provider name, without the live cache's tenant/provider
capability key. Its reuse result is not a production cross-tenant isolation proof.

The C++ live sources now use typed SDK requests and full immutable Outcomes;
engine-cache identity binds the tenant/topology and captured provider/model/host
instructions. Trusted host tenant selection, quota/store boundaries and thread
isolation remain in force. Portable JSON summaries are not native authority.
The numerical results and transcripts retained below are historical, not a new
cutover execution or live pass. The live binary hardcodes **1,000 requests and
32 workers** and has no cheap small-smoke flag. Do not run it as a single-call
check: it needs live credentials/network and authorizes substantial provider
spending only when separately approved. No price or zero-error guarantee is
inferred. Keep credentials/prompts/artifacts private and never publicly export
raw native payloads. Provider retry is a single explicit layer, default off;
there is no retired throttling-provider wrapper in the current public API.

`server_live_llm.cpp` passes a path-specific 180-second timeout to the typed
provider factory. The global default is unchanged; a timeout is not permission
to resend an uncertain or observed call.

The preserved interface-3 model-free E2E executed the dedicated mock workload: 1,000 graph requests,
zero errors, three compiled topologies and 997 cache hits; the Alice topology
swap reused the existing fanout engine. These are topology-load/cache observations,
not production authentication, quota, semantic-response or memory-capacity guarantees.
The isolated-host CLI emitted two distinct policy tuples but did not execute a graph.


**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**One process serves N customers with N different agent topologies simultaneously.**
Measurements: 1000 concurrent real OpenAI calls / 6 customers / 3 topologies /
**peak 29 MB / 0 errors**.

The mock workload shares compiled engines for equivalent captured configuration.
Production tenant selection, authentication, stores and quotas remain host
responsibilities. The historical RSS below does not bound those resources.

This cookbook is a working minimal implementation of that structure.
## Isolation contract (production boundary)

The measurements above are **topology-sharing measurements**, not a security
or capacity claim for a production SaaS host. A production ingress creates an
immutable `TenantScope` after authentication and gives each tenant its own
provider/model policy, `GraphRegistry` snapshot, `ToolSet`, `ScopedStore`,
`ScopedCheckpointStore`, Harness record namespace, and `TenantQuota`:

```cpp
auto backend_store = std::make_shared<InMemoryStore>(); // or a tenant DB
auto backend_checkpoints = std::make_shared<InMemoryCheckpointStore>();
TenantScope scope("tenant-a", "authz-a"); // trusted ingress only
ScopedStore store(scope, backend_store);
ScopedCheckpointStore checkpoints(scope, backend_checkpoints);
TenantQuota quota({.max_concurrency = 32, .max_queue = 64,
                   .max_model_tokens = 2'000'000, .max_artifacts = 1000});
```

The same public `thread_id`, checkpoint ID, Harness run ID, artifact ID, or
result URI can safely occur in another tenant: scoped adapters map it to a
private backend namespace and wrong-tenant lookups return absence. Resume,
replay, fork, cancellation, and dereference must all use the same ingress
scope; do not route those operations from an unscoped identifier.

`CatalogConfig::materialization_context_identity` is a stable, non-secret host
identity for the provider/model policy, tool catalog, store binding, and
registry snapshot. It is part of the generation-cache identity. Provider
credentials, authorization tokens, and topology JSON never belong in that
identity, cache keys, journals, logs, or diagnostics. Omit the field only when
the exact capability receipts are intentionally equivalent.

The mock program below demonstrates measured topology reuse. It does **not**
prove tenant isolation, provider isolation, quota behavior, or production
memory extrapolations. Production hosts should keep one scoped engine/resource
binding per tenant and measure their own stores, provider routes, and quotas.
### Offline isolated-host reference

```bash
cmake --build build --target cookbook_multi_tenant_isolated_host
./build/cookbook_multi_tenant_isolated_host
```

This reference CLI prints two tenants' distinct topology,
provider/model policy, tool policy, stores, and quotas. It uses synthetic
identities and no network credentials; replace its ingress and control-plane
lookups with deployment-owned implementations before production use.

## Scenario

6 customers use 3 different topologies:

| Customer | Topology | Shape | LLM call/request |
|---|---|---|---|
| alice, bob | **simple** | `start → respond → end` | 1 |
| charlie, david | **reflexive** | `start → draft → critique → final → end` | 3 |
| eve, frank | **fanout** | `start → [perspective_a, _b, _c] → merge → end` | 3 (parallel) |

Each customer's graph_def is defined inline JSON, but real production would
store it directly as Postgres `customer_graphs.graph_def JSONB` row.

Core code flow ([server.cpp](server.cpp)):

```cpp
class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        // This provider-free style demo still binds all captured configuration.
        const std::string key = json::array({def, ctx.model, ctx.instructions,
            ctx.extra_config, ctx.provider_name}).dump();
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        // Miss — compile (lock 밖에서, 다른 customer 차단 안 함).
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        {
            std::unique_lock lk(mu_);
            auto [it, inserted] = cache_.emplace(key, engine);
            if (!inserted) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;  // race — 다른 thread 가 먼저 넣음
            }
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
    std::size_t hits()   const { return hits_.load(); }
    std::size_t misses() const { return misses_.load(); }
    std::size_t size()   { std::shared_lock lk(mu_); return cache_.size(); }
};

// On request arrival
auto def    = db.fetch_graph(customer_id);   // One JSONB row
auto engine = cache.get_or_compile(def, ctx);
RunConfig cfg;
cfg.thread_id = customer_id + "__" + session_id;   // Session isolation key
cfg.input     = {{"messages", json::array({{{"role", "user"}, {"content", user_message}}})}};
auto result   = engine->run(cfg);
```

The provider-free demo shares engines only for equivalent topology and captured
model, instructions, extra configuration and provider name. Customer graph
modification changes hash, triggering new engine compile + cache.

## Build / Run

### Mock version (no provider calls)

```bash
cmake --build build --target cookbook_multi_tenant_mock
./build/cookbook_multi_tenant_mock
```

Requires SchemaProvider at native configure/link time, even without a provider
key. Set `CMAKE_PREFIX_PATH` or `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` first.
Measures this workload (1000 concurrent requests /
compile cache hit rate / memory).

### Live LLM version (OpenRouter DeepSeek)

```bash
# .env must contain OPENROUTER_API_KEY at repo root
cmake --build build --target cookbook_multi_tenant_live
./build/cookbook_multi_tenant_live
```

**Cost ≈ provider-dependent** (2330 calls through the pinned DeepSeek route).

## Measurements

| Aspect | Mock 1000 req | Live 100 req | **Live 1000 req** |
|---|---|---|---|
| OK / Errors | 1000 / 0 | 100 / 0 | **1000 / 0** ⭐ |
| Wall time | 5 ms | 11.5 s | 50.2 s |
| Mean latency | 39 µs | 1.58 s | 1.4 s |
| Max latency | 2.99 ms | 9.33 s | 14.4 s |
| Throughput | 200K RPS | 8.67 RPS | **19.9 RPS** |
| **Peak RSS** | **5.25 MB** | **21.9 MB** | **29.25 MB** |
| Compile cache hit rate | 99.7% | 94% | **99.4%** |
| Distinct engines | 3 | 6 | 6 |

**Measurement environment**: WSL2 / 32-thread asio thread pool / single host /
real OpenRouter DeepSeek API call.

Key numbers:

- **1000 concurrent in-flight LLM coroutine + connection memory cost ≈
  29 MB**. 100 req → 1000 req increase +7 MB ⇒ ~8 KB per additional connection.
  Combination of asio coroutine + httplib SSL connection pool.
- **0 errors at 1000 concurrent** — NG gracefully absorbs rate-limit / network jitter / TLS
  handshake jitter without retry in that historical run; this is not a current reliability guarantee.
- **Cache hit rate 99.4%** — hit rate maintained even with more customers if
  topology count stays the same in this historical workload. This does not
  establish memory capacity for 1,000 production tenants.

## LangGraph Comparison — Real Meaning

These runs did not benchmark LangGraph. LangGraph does not require one process
per customer. A comparison needs equivalent graph, store, provider and isolation
policies; the earlier process-per-customer estimates are not measured results.

## Practical Scenario — How Far Can It Go

The historical six-customer load cannot establish capacity on a cloud instance
or predict 10,000/100,000 connections. Measure the production host with its real
authentication, tenant-specific resource bindings, stores, provider routes and
limits. Current SDK transport qualification is Linux/POSIX.

## Hot-swap Demonstration

`server.cpp` end shows in-place change of alice's topology from `simple` → `fanout`
and immediately processes next request. 0 deploy cycle, 0 restart. Real production
would be customer edits graph JSON in web UI → DB save → next request uses new topology.

## Future Enhancements

- **CheckpointStore integration** — Currently passes history as input per request.
  With Postgres CheckpointStore, automatic persistence per thread_id.
- **Pinned Provider** — every customer uses the same OpenRouter DeepSeek model;
  `NodeContext::provider` can still carry customer-specific context.
- **Streaming response** — `run(input)` with `input.stream_cb` + SSE for token-level
  streaming. Use NG's `run(NodeInput)` path with the stream callback directly.
- **A/B experiment framework** — Traffic split via graph_def hash + customer_id sticky split.
  Extend code pattern directly.
- **Streaming + cancel integration** — Abort outbound LLM socket on client disconnect.
  Wire NG's `RunConfig::cancel_token` directly.

## Core Message

The preserved mock evidence is 1,000 requests, three compiled topologies and
997 cache hits. The isolated-host CLI is a policy reference, not a serving
workload. The live timings above remain historical, not a new-cutover pass.
