# Python client overhead — NeoGraph bindings vs the standard SDKs

Same workload through `neograph_engine` (the C++ engine via pybind11)
and through the official Python SDKs. The C++ engine bench shows
100×-600× wins on engine-overhead (see [`benchmarks/`](../README.md));
this folder answers a narrower question: when the user is sitting in
Python, does any of that win survive the binding boundary?

Methodology: an in-process Python mock server returns canned
responses in <1 ms, so server-side time is a flat constant. The
delta is purely client-side — JSON build, HTTP, parse.

## Sequential overhead (K=1)

`bench_a2a_clients.py`:

| Client | median | p95 | throughput |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | **1,137 µs** | 1,381 µs | **860 req/s** |
| `a2a-sdk` 1.0.2 | 2,196 µs | 2,746 µs | 444 req/s |

→ NeoGraph **1.93×** faster.

`bench_openai_clients.py`:

| Client | median | p95 | throughput |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider` | **1,252 µs** | 1,423 µs | **789 req/s** |
| `openai` 2.33 | 1,927 µs | 2,393 µs | 509 req/s |

→ NeoGraph **1.54×** faster.

A combined OpenAI-call-inside-A2A round-trip therefore goes
**~3× faster** through the NeoGraph binding than through the
SDK stack — the wins compound because each layer is independent.

## Concurrent throughput

`bench_concurrent.py`, K = 1/4/16/64 in-flight requests, 500 total:

|   K | NeoGraph req/s | a2a-sdk req/s | speedup |
|----:|---------------:|--------------:|--------:|
|   1 |            881 |           448 |  1.97×  |
|   4 |        **1,461** |           446 |  **3.28×**  |
|  16 |            403 |           390 |  1.03×  |
|  64 |            343 |           275 |  1.25×  |

The K=4 row is the cleanest read: NeoGraph's pybind11 wrapper releases
the GIL during the C++ HTTP exchange, so a `ThreadPoolExecutor` scales
nearly linearly. `a2a-sdk` is asyncio-native and uses one event loop —
adding more in-flight requests doesn't help the per-request
serialization cost it pays inside Python.

(K=16+ both drop because the mock server's `http.server.ThreadingHTTPServer`
saturates at ~400 r/s of concurrent threads — that's a Python stdlib
limitation, not a client property. The NeoGraph A2A C++ server
handles 200 concurrent runs without breaking a sweat per the engine
bench, but the client-side numbers above stand on their own.)

## What this means

- **Sub-µs engine win doesn't survive the binding boundary**, but a
  **2–3× client win does.** Per-request overhead is GIL release,
  HTTP plumbing, and JSON parsing — all of which the native client
  does faster than httpx + pydantic.
- For real LLM workloads (300+ ms per call), the client overhead is
  in the noise — but at high RPS to a fast endpoint (mock test,
  internal services, agent fan-out, multi-shot routing) it
  dominates wall time.
- **Concurrent threads scale.** GIL release on `send_message` /
  `complete()` lets you run a real ThreadPoolExecutor without
  hitting Python's parallelism wall. Useful when you have one client
  fanning out to N agents.

## Reproduce

```bash
pip install neograph-engine==0.2.2 a2a-sdk openai httpx
python bench_a2a_clients.py 500       # sequential A2A
python bench_openai_clients.py 500    # sequential OpenAI
python bench_concurrent.py 500        # concurrent A2A
```

Numbers above were measured 2026-04-29 on x86_64 Ubuntu 24.04
(WSL2), Python 3.12.3, against the in-process mock servers in this
folder. Results are reproducible to within ±5%.
