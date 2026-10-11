# dr_compare: deep-research orchestration comparison

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

The runners implement router → plan → researcher Send branches → synthesis with matching prompts and model selection. Their engines, bindings, clients, and checkpoint implementations differ, so end-to-end timing does not isolate engine cost or transport alone. The April 2026 findings below are preserved historical evidence, not a rerun of the current cutover.

## Files and dependencies

`dr_neograph.py`, `dr_langgraph.py`, `bench.py`, `bench_mock.py`, `mem_probe.py`, `mem_prod_stack.py`, `sweep.sh`, `_run_single.py` cover real calls, plain-text mock workloads, memory probes, sweeps, and one-shot diagnosis. Install a wheel matching the current source via the [Python binding guide](../../docs/python-binding.md); an old wheel with `CompletionParams`/`OpenAIProvider` is not the current API. Source builds require the external SchemaProvider SDK even for Core.
Current builds require a wheel/native build matching SDK `0.3.0`, interface
revision/shared generation 6, at merged SchemaProvider PR #21
(`83112573ba59e3b561fc33c22394638be7aa5294`). Current integrated validation is pending.
This comparison runner is separate from the built-in Deep Research recovery path.

The workflow imports requests, LangGraph, and langchain-openai; memory probes use psutil. PostgreSQL mode also needs the appropriate checkpoint packages and a running database. `mem_prod_stack.py` imports additional web/database/observability packages for its named stacks; it is not a bare-engine-only RSS probe.

## Current environment controls

| Variable | Default | Purpose |
|---|---|---|
| `LLM_MOCK_MS` | `-1` | Real calls below zero; >=0 is plain-text node work with sleep, no Provider or outcomes. |
| `MOCK_SEARCH` | `0` | 1 skips Crawl4AI and returns canned evidence. |
| `FANOUT` | `5` | Researcher branch count/limit. |
| `USE_INMEMORY_CP` | `0` | 1 selects in-memory checkpoints; mock mode also selects them. |
| `NG_TRANSPORT` | `http-chat` | NG: http-chat or http-responses. No WebSocket; Responses changes wire API. |
| `NG_WORKER_COUNT` | `4` | NG worker count for fan-out. |
| `DR_MODEL` | `gpt-5.4-mini` | Explicit model for real calls on both sides. |
| `NEOGRAPH_PG_DSN` | `empty` | NG PostgreSQL DSN; without one, falls back to in-memory. |
| `LANGGRAPH_PG_DSN` | `NEOGRAPH_PG_DSN` | LG PostgreSQL DSN override. |
| `CRAWL4AI_URL` | `empty` | Search service; no service means search unavailable unless mocked. |

`OPENAI_API_BASE` selects an admitted origin/gateway prefix for real calls; `NG_PROVIDER_DESCRIPTOR` can supply a full descriptor on the NeoGraph side. Credentials and custom CA belong in runtime options (`OPENAI_API_KEY`, `NG_EXAMPLE_CA_FILE`), not descriptor data. The default NG route is HTTP Chat, matching the LG Chat wire API. HTTP Responses deliberately compares a different API. HTTP/2 availability depends on libcurl and the peer; these runners do not prove multiplexing or a fixed connection count.

## Historical findings: 2026-04-26

1. Mocked LLM, FANOUT=5: recorded medians were NeoGraph 1.0 ms and LangGraph 5.9 ms (5.9× ratio for that workload).
2. Initial hosted-model round: NG p50 23.90 s (sd 5.90 s), LG 21.95 s (sd 1.23 s).
3. The historical connection diagnosis recorded 21 `connect()` syscalls in a seven-model-call NG run. That count alone does not prove 21 TLS sessions, HTTP versions, or payload equivalence.
4. Historical pooling changes identified as commits `6da4810` / `bc2ab4f` were associated with NG p90 35.34 s → 25.28 s and sd 5.90 s → 1.28 s. Those changes concerned removed provider implementations; current typed SchemaProvider uses the external SDK/libcurl runtime.
5. A worker-count experiment reported NG 307 ms vs LG asyncio 711 ms at FANOUT=50 and LLM_MOCK_MS=100 with NG_WORKER_COUNT=50. This is a separate workload, not a general server-capacity result.

The old claim that NeoGraph still needs HTTP/2 support is obsolete. These recorded timings do not qualify the new SDK, current Python wheel, other platforms, or remote inference speedups. Mock mode measures orchestration and configured sleep only; it creates no provider evidence. Real mode extracts visible text from typed owned outcomes but does not benchmark native replay, portable history export, or provider-report/budget-charge settlement.

## Running a new cohort

The mock command below avoids hosted calls and persistence. Record source/SDK/wheel revisions, Python and dependency versions, host limits, worker count, warmup, iterations, checkpoint mode, and failure counts alongside new results. Keep historical files unchanged.
Both harnesses reject empty reports and exit nonzero if any warmup or measured
run fails; timings include only successful samples. `Failed runs` must be zero
before a cohort is reported as passing. Run the no-provider accounting
regressions with `python -m unittest discover -s benchmarks/dr_compare -p test_bench.py`
from the repository root.

```sh
# Install a current-cutover wheel using the Python binding build guide first.
python -m pip install requests langgraph langchain-openai psutil
cd benchmarks/dr_compare
env -u NG_WORKER_COUNT LLM_MOCK_MS=0 MOCK_SEARCH=1 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench_mock.py --warmup 5 --iters 50
```

To observe fan-out overlap, rerun with `LLM_MOCK_MS=100` and compare unset
`NG_WORKER_COUNT` (default 4) with an explicit `NG_WORKER_COUNT=1`, keeping
`FANOUT=5`, warmup, iterations, and checkpoint mode identical. Capture stderr
and failures; absence of a serial-fan-out warning is not proof of overlap by
itself. These are model-free workloads, not paid-provider evidence.

The following hosted-model command may incur charges. Set credentials intentionally; it uses in-memory checkpoints. A PostgreSQL comparison needs matching DSNs, package setup, and a separately recorded durability scope. Neither a syscall trace nor a packet capture alone establishes semantic equivalence or provider billing.

```sh
# From benchmarks/dr_compare; hosted calls require explicit credentials.
: "${OPENAI_API_KEY:?Set a hosted key only if you intend paid calls}"
: "${CRAWL4AI_URL:?Set a running Crawl4AI service}"
LLM_MOCK_MS=-1 MOCK_SEARCH=0 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench.py --warmup 2 --iters 5
```

This command is not a spending limit: for `FANOUT=5`, a research query performs
at most seven logical model calls per side (plan, five researchers, synthesis).
Two warmups plus five measured runs on both sides can therefore dispatch
98 logical calls before any client retry. Authorized bounded validation must
reserve calls and the sum of configured maximum output allowances outside
the benchmark, including all SDK/LangChain retries. NeoGraph's helper defaults
to `NG_EXAMPLE_MAX_TOKENS=1600`; this does not configure LangChain's output cap.
Use only the host's explicitly authorized model, endpoint, credentials, and
search service. A reduced representative cohort is not the historical
multi-iteration measurement and must be labeled separately.
