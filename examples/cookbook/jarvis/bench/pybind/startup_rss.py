#!/usr/bin/env python3
"""Startup latency + RSS measurement. Run each target in a fresh process.
neograph measures import + symbol loading; langgraph measures import + minimal graph compile.
Because the scopes differ, do not compare the two as identical graph-compilation costs.
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
    # Realistic LangGraph chatbot stack (what the benchmark twin actually imports)
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
