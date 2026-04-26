"""Memory probe: NG worker scaling vs LG runtime baseline.

Spawns clean subprocesses so each measurement starts from a fresh
Python interpreter. Compares:

  * NG idle RSS at worker_count = 1, 8, 64, 256, 1024
  * LG idle RSS after importing langgraph + langchain_openai
  * NG / LG peak RSS handling FANOUT concurrent mock-LLM branches
"""

from __future__ import annotations

import json
import subprocess
import sys

PY = sys.executable

def _run(snippet: str) -> dict:
    out = subprocess.check_output([PY, "-c", snippet], stderr=subprocess.STDOUT)
    line = out.strip().decode().splitlines()[-1]
    return json.loads(line)


def ng_worker_scaling():
    snippet = r"""
import json, os, psutil
p = psutil.Process()
baseline = p.memory_info().rss / 1024**2

import neograph_engine as ng
imp = p.memory_info().rss / 1024**2

defn = {"name":"t","channels":{},"nodes":{},"edges":[]}
results = {"baseline_mb": round(baseline,1), "import_ng_mb": round(imp,1)}
last_rss = imp
for n in [1, 8, 64, 256, 1024]:
    e = ng.GraphEngine.compile(defn, ng.NodeContext())
    e.set_worker_count(n)
    rss = p.memory_info().rss / 1024**2
    results[f"workers_{n}"] = round(rss, 1)
    last_rss = rss
print(json.dumps(results))
"""
    return _run(snippet)


def lg_baseline():
    snippet = r"""
import json, psutil
p = psutil.Process()
baseline = p.memory_info().rss / 1024**2
import langgraph, langchain_openai
from langgraph.graph import StateGraph, START, END
from langchain_openai import ChatOpenAI
imp = p.memory_info().rss / 1024**2
print(json.dumps({"baseline_mb": round(baseline,1), "import_lg_mb": round(imp,1)}))
"""
    return _run(snippet)


def ng_concurrent_fanout(width: int):
    """NG: fan out `width` mock-LLM branches in parallel, measure peak RSS."""
    snippet = rf"""
import json, time, psutil, sys
sys.path.insert(0, '/root/Coding/NeoGraph/bindings/python/examples')
from _common import ng

WIDTH = {width}

class Mock(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute(self, state):
        time.sleep(0.5)  # simulate I/O
        return [ng.ChannelWrite("done", [1])]

class Fanout(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute_full(self, state):
        return [ng.Send("worker", {{}}) for _ in range(WIDTH)]

ng.NodeFactory.register_type("fan", lambda name, c, ctx, _f=Fanout: _f(name))
ng.NodeFactory.register_type("worker", lambda name, c, ctx, _f=Mock: _f(name))

defn = {{
    "name":"t",
    "channels":{{"done":{{"reducer":"append"}}}},
    "nodes":{{"fanout":{{"type":"fan"}}, "worker":{{"type":"worker"}}}},
    "edges":[
        {{"from":ng.START_NODE,"to":"fanout"}},
        {{"from":"worker","to":ng.END_NODE}},
    ],
}}
e = ng.GraphEngine.compile(defn, ng.NodeContext())
e.set_worker_count(WIDTH)
e.set_checkpoint_store(ng.InMemoryCheckpointStore())

p = psutil.Process()
pre = p.memory_info().rss / 1024**2
e.run(ng.RunConfig(thread_id="m", input={{}}, max_steps=5))
post = p.memory_info().rss / 1024**2
print(json.dumps({{"width": WIDTH, "pre_mb": round(pre,1), "post_mb": round(post,1)}}))
"""
    return _run(snippet)


def lg_concurrent_fanout(width: int):
    """LG: same workload via Send fan-out; asyncio handles concurrency."""
    snippet = rf"""
import json, time, psutil, asyncio, operator
from typing import Annotated, TypedDict
from langgraph.graph import StateGraph, START, END
from langgraph.types import Send

WIDTH = {width}

class S(TypedDict, total=False):
    done: Annotated[list, operator.add]

def fanout(state):
    return [Send("worker", {{}}) for _ in range(WIDTH)]

def worker(state):
    time.sleep(0.5)
    return {{"done": [1]}}

g = StateGraph(S)
g.add_node("worker", worker)
g.add_conditional_edges(START, fanout, ["worker"])
g.add_edge("worker", END)
app = g.compile()

p = psutil.Process()
pre = p.memory_info().rss / 1024**2
app.invoke({{}}, config={{"recursion_limit": 5}})
post = p.memory_info().rss / 1024**2
print(json.dumps({{"width": WIDTH, "pre_mb": round(pre,1), "post_mb": round(post,1)}}))
"""
    return _run(snippet)


if __name__ == "__main__":
    print("=" * 70)
    print("NeoGraph worker-pool scaling (idle RSS, fresh process)")
    print("=" * 70)
    r = ng_worker_scaling()
    for k, v in r.items():
        print(f"  {k:20s}  {v:6.1f} MB")

    print()
    print("=" * 70)
    print("LangGraph baseline (post-import RSS, fresh process)")
    print("=" * 70)
    r = lg_baseline()
    for k, v in r.items():
        print(f"  {k:20s}  {v:6.1f} MB")

    print()
    print("=" * 70)
    print("Concurrent fan-out (FANOUT branches, sleep(0.5) per worker)")
    print("=" * 70)
    print(f"  {'width':>6s}  {'NG pre':>8s}  {'NG post':>8s}  "
          f"{'LG pre':>8s}  {'LG post':>8s}")
    for w in [8, 64, 256]:
        ng_r = ng_concurrent_fanout(w)
        lg_r = lg_concurrent_fanout(w)
        print(f"  {w:>6d}  {ng_r['pre_mb']:>8.1f}  {ng_r['post_mb']:>8.1f}  "
              f"{lg_r['pre_mb']:>8.1f}  {lg_r['post_mb']:>8.1f}")
