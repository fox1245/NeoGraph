# Python Mode Benchmark — NeoGraph-from-Python vs LangGraph

Core question: **Does using NeoGraph from Python via pybind (node body also Python) eliminate
the advantages of standalone C++ (startup · RSS · throughput)?**

Answer: **No.** The bloat is not from Python interpreter but from LangChain import tree.
NeoGraph-from-Python = lean Python (10MB/30ms) + single compiled .so.

## Measured Results (2026-07-05, WSL2, python3.12)

```bash
cd <neograph>/build-pybind
LD=$(pwd)
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" python3 <bench>/pybind/startup_rss.py neograph
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" python3 <bench>/pybind/perturn.py   neograph 5000
python3 <bench>/pybind/startup_rss.py langgraph          # bare
python3 <bench>/pybind/startup_rss.py langgraph_openai   # actual chatbot stack
python3 <bench>/pybind/perturn.py   langgraph 5000
```

| Metric (all Python processes) | NeoGraph-from-Python | LangGraph | Advantage |
|---|---|---|---|
| per-turn (5 Python-callable nodes, GIL included) | **0.38ms · ~2620 turns/s** | 0.93ms · ~1075 | 2.4× |
| startup (import→compile) | **40ms** | 462ms (bare) / 2977ms (+langchain_openai) | 11–73× |
| RSS | **36MB** | 61MB (bare) / 561MB (+langchain_openai) | 1.7–15× |
| (reference) bare python3 RSS | — | 9.9MB | |

## Why Also Fast in Python Mode

- **per-turn**: BSP engine (super-step loop · scheduler · channel reduction · routing · checkpoint
  overhead) runs in C++ and **only node body is Python**. LangGraph's engine is pure Python Pregel.
  GIL holds during node execution on both, but NeoGraph's orchestration *between* nodes is C++ so it's faster.
  pybind/GIL boundary cost is nearly zero so standalone C++ mock (9 nodes 0.38ms) and Python 5 nodes are
  effectively tied.
- **startup/RSS**: `import neograph_engine` loads single .so. LangGraph's 462ms/
  61MB is langgraph+langchain-core import tree, up to 2977ms/561MB with langchain_openai. NeoGraph has no such tree.

## Implications

Using NeoGraph from Python gives **Python ecosystem entire (HF·OpenAI SDK·pandas etc.
inline in node body) + startup · RSS · throughput advantage simultaneously**.
That is, the dichotomy "performance is C++ standalone, ecosystem is Python" is wrong —
Python mode gives both. standalone C++ goes one step further (startup 8ms · RSS
7.5MB) but only when node is C++ or tools called via HTTP.

Caution: If node imports torch/HF, RSS is dominated by that library
(engine is noise). This is workload property, not framework — same for both.
