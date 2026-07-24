<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/pybind/README.md locale=zh-CN source_sha256=097ce5a3394ec214d04a7245b7e4e7e6d937754bb1c18e589a850dc1bc41f54d -->
# Python 模式基准 — NeoGraph-from-Python vs LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

核心问题：**通过 pybind 从 Python 使用 NeoGraph（node body 也在 Python 中）是否会消除独立 C++ 的优势（启动 · RSS · 吞吐量）？**

答案：**不会。** 膨胀不是来自 Python 解释器，而是来自 LangChain import tree。NeoGraph-from-Python = 精简 Python（10MB/30ms）+ 单个编译后的 .so。

## 测量结果（2026-07-05，WSL2，python3.12）

```bash
cd <neograph>/build-pybind
LD=$(pwd)
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" python3 <bench>/pybind/startup_rss.py neograph
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" python3 <bench>/pybind/perturn.py   neograph 5000
python3 <bench>/pybind/startup_rss.py langgraph          # bare
python3 <bench>/pybind/startup_rss.py langgraph_openai   # actual chatbot stack
python3 <bench>/pybind/perturn.py   langgraph 5000
```

| 指标（全部为 Python 进程） | NeoGraph-from-Python | LangGraph | 优势 |
|---|---|---|---|
| 每 turn（5 个 Python-callable nodes，包含 GIL） | **0.38ms · ~2620 turns/s** | 0.93ms · ~1075 | 2.4× |
| 启动（import→compile） | **40ms** | 462ms（bare）/ 2977ms（+langchain_openai） | 11–73× |
| RSS | **36MB** | 61MB（bare）/ 561MB（+langchain_openai） | 1.7–15× |
| （参考）bare python3 RSS | — | 9.9MB | |

## 为什么 Python 模式也很快

- **每 turn**：BSP engine（super-step loop · scheduler · channel reduction · routing · checkpoint overhead）运行在 C++ 中，**只有 node body 是 Python**。LangGraph 的 engine 是纯 Python Pregel。两边在节点执行期间都会持有 GIL，但 NeoGraph 在节点 *之间* 的编排是 C++，所以更快。pybind/GIL 边界成本几乎为零，因此独立 C++ mock（9 nodes 0.38ms）和 Python 5 nodes 实际打平。
- **启动/RSS**：`import neograph_engine` 只加载单个 .so。LangGraph 的 462ms/61MB 来自 langgraph+langchain-core import tree，加上 langchain_openai 时可达 2977ms/561MB。NeoGraph 没有这样的树。

## 含义

从 Python 使用 NeoGraph 可以同时获得 **完整 Python 生态（HF·OpenAI SDK·pandas 等可直接放进 node body）+ 启动 · RSS · 吞吐量优势**。也就是说，“性能属于 C++ standalone，生态属于 Python”这个二分是错的 — Python 模式两者兼得。独立 C++ 更进一步（启动 8ms · RSS 7.5MB），但只在 node 是 C++ 或工具通过 HTTP 调用时成立。

注意：如果 node import torch/HF，RSS 会被该库主导（engine 成为噪声）。这是 workload 属性，不是框架属性 — 两边都一样。
