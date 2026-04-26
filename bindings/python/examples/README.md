# Python API examples

Five short scripts covering the binding surface. Run with:

```bash
pip install neograph-engine
python 01_minimal.py
```

Examples 01–04 are self-contained (no API key, no network). Example
05 makes a real OpenAI call and reads `OPENAI_API_KEY` from the
environment.

| # | File | Surface exercised |
|---|------|-------------------|
| 01 | [`01_minimal.py`](01_minimal.py) | `GraphNode` subclass + `NodeFactory.register_type` + `GraphEngine.compile` + `engine.run`. The smallest useful graph. |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | `Tool` subclass + `NodeContext(tools=[...])` + built-in `tool_dispatch` node. The LangChain-migration shape. |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | `execute_full` returning `Send` list + `engine.set_worker_count(...)` for real CPU parallelism. Map-reduce pattern. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | `engine.run_async` + `asyncio.gather` for concurrent runs. `engine.run_stream_async` with a Python callback. |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | `OpenAIProvider` wired into `NodeContext` + built-in `llm_call` node. Requires `OPENAI_API_KEY`. |

## Two mental models

NeoGraph from Python looks like LangGraph from Python: a graph of
nodes, channels with reducers, dynamic fan-out via `Send`, routing
overrides via `Command`. The same primitives, the same JSON-ish
graph definition. The difference is what's running it — a C++
engine that does the super-step loop, scheduling, and checkpointing
in a few microseconds per step instead of LangGraph's ~600 µs.

Two patterns to keep in mind when porting from LangGraph:

1. **Python custom nodes** (examples 01, 03, 04) subclass
   `neograph_engine.GraphNode` and override `execute(state)` or
   `execute_full(state)`. The engine dispatches into Python under
   GIL handling — fan-out workers each acquire the GIL re-entrantly,
   so concurrent custom nodes don't deadlock.

2. **Python tools** (example 02) subclass `neograph_engine.Tool`
   and pass instances into `NodeContext(tools=[...])`. The engine
   takes ownership at compile time; Python references can drop.

Async (example 04) is asyncio-compatible — every `*_async`
binding returns an `asyncio.Future` bound to the calling thread's
running loop. Stream callbacks are hopped onto the loop thread
via `loop.call_soon_threadsafe`, so your `cb(ev)` runs where
asyncio expects.

## Distribution name vs. import name

The PyPI package is **`neograph-engine`** (the bare `neograph`
name was already taken on PyPI by an unrelated project). The
Python import name is `neograph_engine`:

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider
```
