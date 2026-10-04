# Python graph benchmarks: NeoGraph and LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

These scripts measure Python graph execution and process startup. Neither script calls a model provider or requires an API key. They do not qualify the typed provider request, event, or outcome bindings. The commands below are intended for an installed current package; they were reviewed against source, not executed during this update.

## Reproduction

Use a Python environment with the current `neograph-engine` wheel installed. Install `langgraph` for the comparison and `langchain-openai` for the optional larger import stack. Run from the repository root:

```bash
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py neograph
python3 examples/cookbook/jarvis/bench/pybind/perturn.py neograph 5000
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph_openai
python3 examples/cookbook/jarvis/bench/pybind/perturn.py langgraph 5000
```

`perturn.py` warms up and runs a five-node chain. Each Python node increments the `v` channel; each measured run starts at zero. It prints mean, p50, p90, and runs per second. Use a positive run count.

`startup_rss.py` runs in a fresh process and prints startup milliseconds and peak RSS. The NeoGraph branch imports the package and resolves three graph symbols; it does not compile a graph. The LangGraph branches import their packages and compile a one-node graph. These startup measurements cover different work, so their ratio is not a graph-compilation speedup. RSS conversion assumes Linux's `ru_maxrss` unit of KiB; do not use that conversion unchanged on macOS. Python's `resource` module is unavailable on Windows.

## Historical measurements

The repository previously reported these values. They are not measurements of the current cutover build and were not rerun for this update.

| Metric | NeoGraph from Python | LangGraph |
|---|---|---|
| Five Python nodes per run | 0.38 ms; about 2620 runs/s | 0.93 ms; about 1075 runs/s |
| Startup, with the different scopes above | 40 ms | 462 ms; 2977 ms with `langchain_openai` |
| Peak RSS | 36 MB | 61 MB; 561 MB with `langchain_openai` |

NeoGraph executes the graph scheduler and channel reductions in C++; Python node bodies acquire the GIL. These measurements do not isolate GIL boundary cost or establish performance for other workloads. Imports inside a node, such as PyTorch, also contribute to process memory.
