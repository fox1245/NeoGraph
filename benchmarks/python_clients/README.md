# Python client overhead: current runners and historical results

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

This directory compares NeoGraph Python bindings and Python SDK clients against local in-process protocol peers. Measured wall time includes client work, server scheduling, and the HTTP exchange; the server’s cost is not a proven constant. No real model runs here. The tables preserve measurements from 2026-04-29 on x86_64 Ubuntu 24.04 (WSL2), Python 3.12.3, not measurements of the updated SDK cutover.

## Historical sequential overhead: K=1

`bench_a2a_clients.py`, local canned A2A responses; the recorded median ratio was 1.93×.

| Client (2026-04-29) | Median | P95 | Throughput |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | 1,137 µs | 1,381 µs | 860 req/s |
| `a2a-sdk` 1.0.2 | 2,196 µs | 2,746 µs | 444 req/s |

`bench_openai_clients.py`, local canned Chat responses; the recorded median ratio was 1.54×. The legacy NeoGraph provider label is retained to identify the old implementation, not as a current import.

| Client (2026-04-29, legacy provider) | Median | P95 | Throughput |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider` (removed) | 1,252 µs | 1,423 µs | 789 req/s |
| `openai` 2.33 | 1,927 µs | 2,393 µs | 509 req/s |

## Historical concurrent throughput

`bench_concurrent.py`, local A2A peer, K ∈ {1, 4, 16, 64}, 500 requests per row.

| K (2026-04-29, A2A) | NeoGraph req/s | a2a-sdk req/s | Ratio |
|----:|---------------:|--------------:|--------:|
| 1 | 881 | 448 | 1.97× |
| 4 | 1,461 | 446 | 3.28× |
| 16 | 403 | 390 | 1.03× |
| 64 | 343 | 275 | 1.25× |

The throughput drop at K=16/64 includes interaction with `ThreadingHTTPServer` and the clients. It does not isolate a stdlib ceiling or prove a general asyncio limitation. Nor does the K=4 result establish near-linear scaling: it reports one sampled configuration.

## Current API and dependencies

The current OpenAI comparison constructs `SchemaProvider` from an admitted HTTP Chat descriptor and runtime options, then uses `make_provider_request` and `invoke`. It consumes authentic typed owned outcomes and checks failure/completion and visible text. `OpenAIProvider`, `CompletionParams`, and `complete()` are removed APIs. The request model is explicit; controls use typed factory defaults unless supplied. They are not legacy provider constructor defaults.

Both clients use the same private HTTP loopback peer and Chat route, with a canned response whose text is `ok`. The peer checks route, model, and prompt. It requires no hosted credentials, custom CA, or paid calls. This is an HTTP/1.0 local protocol workload, not a TLS/HTTP2 transport qualification or native replay benchmark. Canned token usage is provider-reported fixture data, not measured tokens or a budget charge.

Install a wheel matching the current source using the [Python binding guide](../../docs/python-binding.md), then install the comparison SDKs below. Source builds require external `SchemaProvider::runtime` even for Core; follow the [build guide](../../README.md). Python wrappers alone cannot add the typed native API to an old wheel. These pages do not claim a newly verified wheel or new benchmark pass.
For NeoGraph `0.13.0`, the wheel and native consumers must match alpha SDK
`0.1.0`, interface revision/shared generation 4. Current integrated validation is pending.

## Running a new cohort

Run from the repository root. Record the wheel/source/SDK revisions, Python build (including GIL mode), comparison package versions, platform, peer protocol, warmup, iteration count, and failures. The runners default to 500 requests and warm up before measurement; the concurrent runner sweeps K=1/4/16/64.

```bash
# First install a wheel matching this source via docs/python-binding.md.
python -m pip install a2a-sdk openai httpx
python benchmarks/python_clients/bench_a2a_clients.py 500
python benchmarks/python_clients/bench_openai_clients.py 500
python benchmarks/python_clients/bench_concurrent.py 500
```

Retain the old tables separately from new outputs. Individual layer ratios do not multiply into an end-to-end OpenAI-inside-A2A speedup: that combined workload was not measured here. There is no universal 2–3× client guarantee or ±5% reproducibility claim for the current implementation.
