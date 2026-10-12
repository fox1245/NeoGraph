# Program API end-to-end scenarios

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

These tests drive the sealed-v1 JavaScript Program API through the Python binding the way a user does after reading the [Python guide](../../../../docs/python-binding.md): Python `GraphNode` classes, registered conditions, a `ProgramRegistryBuilder` snapshot, `ProgramCompiler` and `LocalProgramHost`. They need a `neograph_engine` built with Program and QuickJS control; without them the package is skipped.

## Coverage

| File | Scenarios |
|---|---|
| `test_01_quickstart.py` | the documented example, a `define()` graph with Python nodes and a condition, identities across processes, activation and rollback |
| `test_02_hitl.py` | `interruptBefore`, `NodeInterrupt`, default and explicit pending ids, interrupts inside `main()`, resume after a process restart |
| `test_03_failures.py` | a node raises, a fan-out branch fails, `main()` throws, a failed `callCore`, `llm_call` without a provider |
| `test_04_budgets.py` | compile-time and run-time JavaScript timeouts, step and operation budgets, wall-time cancellation, admitted budgets |
| `test_05_diagnostics.py` | source locations for syntax and builder errors, registry misses, lint spans, control characters, admission errors |
| `test_06_graph_shapes.py` | barrier joins, conditional routing, `Send` map-reduce, subgraphs, invalid graph shapes |
| `test_07_main_programs.py` | `callCore`, `all`, `race`, `quorum`, `await` timeouts, `cancelScope`, external cancel, `spawn` |
| `test_08_live_llm.py` | a Program whose node calls a model: a local peer by default, OpenRouter when enabled; a Python node and a bound `llm_call` |
| `test_09_bound_llm.py` | `llm_call` bound through `node_context` (#372): reported and estimated costs, refusals before sending (budget, unknown model, router alias), zero budget, ids without a provider, `all` and `quorum` against one budget, a crash after the call is journaled and recovery that charges it once |
| `test_10_failure_routing.py` | failure routing (#364): a failing TEST routed to an `interruptBefore` handler with its record, resume along the retry and abort routes, author codes, redacted error text, `max_visits`, the `max_node_retries` ceiling, a Program diagnostic inside a node, and a resume in a new process with SQLite |
| `test_11_attempt_timeout.py` | attempt timeouts (#364): a slow Python node that times out and recovers on retry, a node that always times out routed to its handler as `NG.Timeout` (and failing the run without a handler), a late completion that never reaches the result or the trace, a node that ignores cancellation past its stop deadline (`NG.EffectUncertain`), and a backoff that would cross `wall_time_ms` stopping with kind `deadline` |
| `test_11_send_interrupts.py` | a `Send` target that raises `NodeInterrupt` (#385): the Program pauses on it, resume replays the map node and the finished Sends, a resume after a restart with SQLite, and a finished Send that called a bound `llm_call` is neither resent nor charged again |

`support.py` holds the shared Python nodes, conditions, registry, budgets and JavaScript builders.

## Run

From the repository root, with a build that has the Python binding, Program and QuickJS control enabled:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_PYBIND=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build
PYTHONPATH=build:bindings/python python -m pytest -q bindings/python/tests/program_e2e
```

The offline scenarios take about 20 seconds; `-m "not slow"` skips the ones that wait a second or more on purpose. CI runs them on every push and pull request as part of `bindings/python/tests/` in the native Linux lane (`scripts/verify_ci.py native-linux`).

## Live model test

`test_live_openrouter_model_call_inside_a_program` and `test_live_openrouter_bound_llm_call_is_priced` each make one paid OpenRouter request (`max_output_tokens=64`, zero data retention routing). The second runs the built-in `llm_call` bound through `LocalProgramHost(node_context=...)` under a 1,000,000-microunit (1 USD) monetary budget and checks the settled record. They are skipped unless both variables are set, and CI never sets them:

```bash
NEOGRAPH_E2E_LIVE=1 OPENROUTER_API_KEY=... PYTHONPATH=build:bindings/python \
  python -m pytest -q -rs bindings/python/tests/program_e2e/test_08_live_llm.py
```

The key is read only from the environment. `OPENROUTER_MODEL`, `OPENROUTER_PRICED_MODEL` (a model whose catalog entry has a route ceiling, `deepseek/deepseek-v4-flash-0731` by default) and `OPENROUTER_BASE_URL` override the models and the endpoint.

## Known issues

A scenario whose intended behavior is decided but not implemented yet is marked with `support.known_issue("#NNN", reason)`, a strict `xfail`. When the fix lands the test passes, strict mode reports that as a failure, and the marker must be removed in the same change. To see the current failures:

```bash
PYTHONPATH=build:bindings/python python -m pytest -q --runxfail -m known_issue bindings/python/tests/program_e2e
```
