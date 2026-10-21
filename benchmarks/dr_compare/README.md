# dr_compare: deep-research orchestration comparison

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

The runners implement router → plan → researcher Send branches → synthesis with matching prompts and model selection. Their engines, bindings, clients, and checkpoint implementations differ, so end-to-end timing does not isolate engine cost or transport alone. The April 2026 findings below are historical and were not rerun with the current SDK.

## Files and dependencies

`dr_neograph.py`, `dr_langgraph.py`, `bench.py`, `bench_mock.py`, `mem_probe.py`, `mem_prod_stack.py`, `sweep.sh`, `_run_single.py` cover real calls, plain-text mock workloads, memory probes, sweeps, and one-shot diagnosis. Install a wheel matching the current source via the [Python binding guide](../../docs/python-binding.md); an old wheel with `CompletionParams`/`OpenAIProvider` is not the current API. Source builds require the external SchemaProvider SDK even for Core.
Current builds require a wheel/native build matching SDK `0.4.0`, interface
revision/shared generation 7, at the SchemaProvider commit that
`cmake/NeoGraphSchemaProvider.cmake` pins.
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
| `LG_MAX_CONCURRENCY` | `NG_WORKER_COUNT`, else `4` | LangGraph pool width (`max_concurrency` of `invoke`). The default matches NeoGraph's, and `bench_mock.py` refuses unmatched widths unless `--allow-unmatched-widths` is given. |
| `DR_MODEL` | `gpt-5.4-mini` | Explicit model for real calls on both sides. |
| `DR_PLAN_TEMPERATURE` | unset | Planner temperature on both sides; unset sends none, because reasoning models such as `gpt-5.4-mini` reject it. |
| `NG_EXAMPLE_MAX_TOKENS` | `1600` | Per-call output cap on both NG and LG; never raised automatically. |
| `NEOGRAPH_PG_DSN` | `empty` | NG PostgreSQL DSN; without one, falls back to in-memory. |
| `LANGGRAPH_PG_DSN` | `NEOGRAPH_PG_DSN` | LG PostgreSQL DSN override. |
| `CRAWL4AI_URL` | `empty` | Search service; no service means search unavailable unless mocked. |

`OPENAI_API_BASE` selects an admitted origin/gateway prefix for real calls; `NG_PROVIDER_DESCRIPTOR` can supply a full descriptor on the NeoGraph side. Credentials and custom CA belong in runtime options (`OPENAI_API_KEY`, `NG_EXAMPLE_CA_FILE`), not descriptor data. The default NG route is HTTP Chat, matching the LG Chat wire API. HTTP Responses deliberately compares a different API. HTTP/2 availability depends on libcurl and the peer; these runners do not prove multiplexing or a fixed connection count.

## Historical findings: 2026-04-26

1. Mocked LLM, FANOUT=5: recorded medians were NeoGraph 1.0 ms and LangGraph 5.9 ms (5.9× ratio for that workload).
2. Initial hosted-model round: NG p50 23.90 s (sd 5.90 s), LG 21.95 s (sd 1.23 s).
3. The historical connection diagnosis recorded 21 `connect()` syscalls in a seven-model-call NG run. That count alone does not prove 21 TLS sessions, HTTP versions, or payload equivalence.
4. Historical pooling changes identified as commits `6da4810` / `bc2ab4f` were associated with NG p90 35.34 s → 25.28 s and sd 5.90 s → 1.28 s. Those changes concerned removed provider implementations; current typed SchemaProvider uses the external SDK/libcurl runtime.
5. A worker-count experiment reported NG 307 ms against 711 ms for LangGraph at FANOUT=50 and LLM_MOCK_MS=100 with NG_WORKER_COUNT=50. The LangGraph runner is the synchronous `invoke` on a thread pool that langchain-core creates for every call, not asyncio (the old label was wrong), and its pool kept the default width, `min(32, CPUs + 4)`, while NeoGraph's was widened to 50. The pools were not matched, so this is withdrawn as an engine comparison; the re-run below matches them.

The old claim that NeoGraph still needs HTTP/2 support is obsolete. These timings were recorded before the new SDK and the current Python wheel; they do not apply to other platforms or predict remote inference speedups. Mock mode measures orchestration and configured sleep only; it creates no provider evidence. Real mode extracts visible text from typed owned outcomes but does not benchmark native replay, portable history export, or provider-report/budget-charge settlement.

## Re-run with matched pool widths

Both runners fan out through a thread pool: NeoGraph's worker pool (`NG_WORKER_COUNT`) and, for LangGraph's synchronous `invoke`, the pool that langchain-core creates for every call (`LG_MAX_CONCURRENCY`, passed as `max_concurrency`). The re-run gives both sides the same width in every configuration, with the mock model (a sleep per call), canned search evidence and in-memory checkpoints: no key and no network. A failed run, or a run on unmatched widths, makes its cell an error and is left out of the table.

<!-- bench-record:begin dr_compare -->
| Configuration | Fan-out | Model delay per call (ms) | Pool width, both sides | NeoGraph, ms per run | LangGraph, ms per run | LangGraph ÷ NeoGraph |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| A: no model delay (the engine floor) | 5 | 0 | 4 | 1.39 [1.37, 1.40] | 6.23 [6.11, 6.34] | 4.47× |
| B: five researchers | 5 | 100 | 4 | 402 [402, 402] | 407 [407, 407] | 1.01× |
| C: fifty researchers, pool as wide as the fan-out | 50 | 100 | 50 | 307 [307, 308] | 329 [328, 329] | 1.07× |
| D: fifty researchers, narrow pool | 50 | 100 | 4 | 1,508 [1,508, 1,508] | 1,516 [1,516, 1,516] | 1.01× |

Mock model (a sleep per call), canned search evidence, in-memory checkpoints, no key and no network; median of 5 interleaved rounds with [min, max] across rounds, each round one fresh process per configuration with 3 warmup and 50 (A); 20 (B, C, D) measured runs per side; neograph_engine 0.16.0, built with g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0. Both sides use the same pool width: `NG_WORKER_COUNT` and `LG_MAX_CONCURRENCY`.

Host: ARM Neoverse-N1, 4 cores, kernel 6.17.0-1018-oracle, Python 3.12.3, NeoGraph built with g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; CPU governor: not exposed by the host.
<!-- bench-record:end dr_compare -->

`session.py` runs the four configurations of the table in interleaved rounds, one fresh `bench_mock.py` process per configuration, and writes a record with the raw samples and its provenance; the table above is generated from that record by `scripts/render_bench_report.py`.

```sh
# Environment: the dr_compare section of benchmarks/requirements-compare.txt (benchmarks/compare/setup_venvs.sh creates it).
# The NeoGraph Python module is built from the source tree (-DNEOGRAPH_BUILD_PYBIND=ON, shared libraries) in $WORK/build-pybind.
python3 benchmarks/dr_compare/session.py --python "$WORK/venvs/dr_compare/bin/python" \
    --pythonpath "$WORK/build-pybind" --build-dir "$WORK/build-pybind" --out dr-record.json --rounds 5 \
    --harness-commit "$(git rev-parse HEAD)" --engine-commit "$(git rev-parse HEAD)"
python3 scripts/render_bench_report.py --dr-record dr-record.json --docs benchmarks/dr_compare/README
```

## Running a new cohort

The mock command below avoids hosted calls and persistence. Record source/SDK/wheel revisions, Python and dependency versions, host limits, worker count, warmup, iterations, checkpoint mode, and failure counts alongside new results. Keep historical files unchanged.
Both harnesses reject empty reports and exit nonzero if any warmup or measured
run fails; timings include only successful samples. `Failed runs` must be zero
before a cohort is reported as passing. Run the no-provider accounting
regressions with `python -m unittest discover -s benchmarks/dr_compare -p test_bench.py`
from the repository root.

`bench_mock.py` sets `LLM_MOCK_MS=0` when unset and forces mock search/in-memory
checkpoints before importing either runner. Negative delay or a cached non-mock
runner is rejected before dispatch; inherited credentials cannot select paid work.
Both harnesses require positive measured iterations and nonnegative warmups.
LG preserves genuine text blocks rather than stringifying empty/rich responses;
empty intermediate replies/plans are failures, not successful benchmark samples.
The real paths also reject truncated/refused/tool/unknown terminals rather than
timing them as completed reports. NG errors retain the original owned Outcome;
LG errors retain the original response. The no-provider regressions isolate
consumer projection policy; they are not transport/SDK or hosted-model evidence.

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
98 logical calls. The current NG SDK default disables transport retries; LG
explicitly sets `max_retries=0`. Set a limit on calls
and on the sum of configured output caps outside the benchmark. Both sides use
`NG_EXAMPLE_MAX_TOKENS=1600` by default; an explicit value is honored unchanged.
Use only a model, endpoint, credentials, and
search service that you are allowed to use. A reduced representative cohort is not the historical
multi-iteration measurement and must be labeled separately.

A finite paid-provider diagnostic (not a latency comparison) is:

```sh
# From benchmarks/dr_compare; this makes paid provider calls.
: "${OPENAI_API_KEY:?Requires an intentionally authorized hosted credential}"
env -u NG_WORKER_COUNT LLM_MOCK_MS=-1 MOCK_SEARCH=1 USE_INMEMORY_CP=1 \
  FANOUT=5 NG_TRANSPORT=http-chat NG_EXAMPLE_MAX_TOKENS=8192 \
  python bench.py --only neograph --warmup 0 --iters 1
```

This is at most seven model calls and 57,344 configured output tokens, plus input
tokens. It has no real web search and must be labelled paid-provider/mock-search,
not a real-search run. A pass needs exit 0, one successful sample, a
nonempty report and `Failed runs: 0`; any failed/empty plan, answer or report fails.
The 8,192-token allowance is an explicit diagnostic choice, not a raised default,
price estimate or spending authorization. With an approved running Crawl4AI
service, `MOCK_SEARCH=0` exercises real search with the same model-call bound.
