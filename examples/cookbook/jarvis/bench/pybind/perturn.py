#!/usr/bin/env python3
"""per-turn 오케스트레이션 오버헤드 — 동일 no-op 그래프를 N회 run.
노드는 파이썬 콜러블(양쪽 동일). C++ BSP 루프 + pybind 경계 + GIL vs 순수 파이썬 Pregel.
argv[1] = neograph | langgraph ; argv[2] = N
"""
import sys, time, statistics as st

which = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

if which == "neograph":
    import neograph_engine as ng

    # 5개 파이썬 노드 체인 (각 노드가 파이썬 콜러블 = GIL 경계 왕복)
    class PyNode(ng.GraphNode):
        def __init__(self, name, ch):
            super().__init__(); self._n = name; self._ch = ch
        def get_name(self): return self._n
        def execute(self, state):
            v = state.get("v") or 0
            return [ng.ChannelWrite(self._ch, v + 1)]

    for i in range(5):
        ng.NodeFactory.register_type(
            f"n{i}", (lambda ch: (lambda name, cfg, ctx: PyNode(name, ch)))("v"))

    graph = {
        "name": "chain", "channels": {"v": {"reducer": "overwrite"}},
        "nodes": {f"s{i}": {"type": f"n{i}"} for i in range(5)},
        "edges": ([{"from": "__start__", "to": "s0"}]
                  + [{"from": f"s{i}", "to": f"s{i+1}"} for i in range(4)]
                  + [{"from": "s4", "to": "__end__"}]),
    }
    engine = ng.GraphEngine.compile(graph, ng.NodeContext())
    # 웜업
    for _ in range(50):
        engine.run(ng.RunConfig(thread_id="w", input={"v": 0}))
    samples = []
    for _ in range(N):
        t = time.perf_counter()
        engine.run(ng.RunConfig(thread_id="b", input={"v": 0}))
        samples.append((time.perf_counter() - t) * 1000.0)

elif which == "langgraph":
    from langgraph.graph import StateGraph, START, END
    from typing import TypedDict
    class S(TypedDict, total=False):
        v: int
    g = StateGraph(S)
    for i in range(5):
        g.add_node(f"s{i}", lambda s: {"v": (s.get("v") or 0) + 1})
    g.add_edge(START, "s0")
    for i in range(4):
        g.add_edge(f"s{i}", f"s{i+1}")
    g.add_edge("s4", END)
    app = g.compile()
    for _ in range(50):
        app.invoke({"v": 0})
    samples = []
    for _ in range(N):
        t = time.perf_counter()
        app.invoke({"v": 0})
        samples.append((time.perf_counter() - t) * 1000.0)

samples.sort()
p = lambda q: samples[min(len(samples) - 1, int(q / 100 * (len(samples) - 1)))]
print(f"{which:<10} N={N} per-run: mean={st.mean(samples):.4f}ms "
      f"p50={p(50):.4f}ms p90={p(90):.4f}ms  → {1000/st.mean(samples):.0f} turns/s")
