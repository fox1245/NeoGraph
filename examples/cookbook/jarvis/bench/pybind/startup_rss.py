#!/usr/bin/env python3
"""기동 + RSS 측정. 각 측정은 새 프로세스에서 실행한다.
neograph는 import + 심볼 로드, langgraph는 import + 최소 그래프 compile을 측정한다.
측정 범위가 다르므로 두 결과를 동일 작업의 compile 비용으로 비교하지 않는다.
argv[1] = neograph | langgraph | langgraph_openai
"""
import os, resource, sys, time

t0 = time.monotonic()
which = sys.argv[1]

if which == "neograph":
    import neograph_engine as ng
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
