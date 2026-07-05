#!/usr/bin/env python3
"""기동 + RSS 측정 — import 부터 trivial 그래프 compile 까지. 프레시 프로세스.
argv[1] = neograph | langgraph
"""
import os, resource, sys, time

t0 = time.monotonic()
which = sys.argv[1]

if which == "neograph":
    import neograph_engine as ng
    # trivial 2-노드 그래프 compile (엔진 초기화 비용 포함)
    graph = {
        "name": "t",
        "channels": {"x": {"reducer": "overwrite"}},
        "nodes": {"a": {"type": "passthrough_noop"}},
        "edges": [{"from": "__start__", "to": "a"}, {"from": "a", "to": "__end__"}],
    }
    # passthrough 없는 최소: 그냥 엔진 클래스 로드까지가 핵심 비용.
    # compile 은 커스텀 노드타입 필요하므로 여기선 import+심볼 로드까지 측정.
    _ = ng.GraphEngine, ng.RunConfig, ng.NodeContext

elif which == "langgraph":
    from langgraph.graph import StateGraph, START, END
    from typing import TypedDict
    class S(TypedDict, total=False):
        x: int
    g = StateGraph(S)
    g.add_node("a", lambda s: {"x": 1})
    g.add_edge(START, "a")
    g.add_edge("a", END)
    app = g.compile()

elif which == "langgraph_openai":
    # 현실적 LangGraph 챗봇 스택 (벤치 쌍둥이가 실제 import 하는 것)
    from langgraph.graph import StateGraph, START, END
    from langchain_openai import ChatOpenAI
    from typing import TypedDict
    class S(TypedDict, total=False):
        x: int
    g = StateGraph(S)
    g.add_node("a", lambda s: {"x": 1})
    g.add_edge(START, "a")
    g.add_edge("a", END)
    app = g.compile()

elapsed = (time.monotonic() - t0) * 1000.0
rss_kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss  # Linux: KB
print(f"{which} startup_ms={elapsed:.1f} rss_mb={rss_kb/1024:.1f}")
