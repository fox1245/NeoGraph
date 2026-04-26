"""LangGraph equivalent of dr_neograph.py.

Same topology, same prompts, same model, same Crawl4AI search,
same Postgres checkpoint backend (langgraph_checkpoint_postgres).
Only the engine + transport differ:
  * NeoGraph: C++ engine, asio thread-pool fan-out, WebSocket Responses.
  * LangGraph: Python engine, asyncio fan-out, HTTP via httpx.

Returns the final assistant markdown report. Used by bench.py."""

from __future__ import annotations

import operator
import os
import re
import urllib.parse
from typing import Annotated, TypedDict

import requests
from langchain_core.messages import HumanMessage
from langchain_openai import ChatOpenAI
from langgraph.graph import StateGraph, START, END
from langgraph.types import Command, Send

CRAWL4AI_URL = os.environ.get("CRAWL4AI_URL", "").rstrip("/")
PG_DSN       = os.environ.get("LANGGRAPH_PG_DSN", os.environ.get("NEOGRAPH_PG_DSN", ""))
DR_MODEL     = os.environ.get("DR_MODEL", "gpt-5.4-mini")

# Bench-mode env knobs (mirror dr_neograph.py).
LLM_MOCK_MS  = int(os.environ.get("LLM_MOCK_MS", "-1"))  # -1 = real LLM
MOCK_SEARCH  = os.environ.get("MOCK_SEARCH", "0") == "1"
FANOUT       = int(os.environ.get("FANOUT", "5"))
USE_INMEMORY = os.environ.get("USE_INMEMORY_CP", "0") == "1"

RESEARCH_TRIGGER_PATTERN = re.compile(
    r"(조사|리서치|연구|research|investigate|deep[- ]?dive)", re.IGNORECASE)


class _MockLGResp:
    __slots__ = ("content",)
    def __init__(self, content): self.content = content


class _MockLGModel:
    """Drop-in for ChatOpenAI — same .invoke() / .bind() shape, no network."""
    def __init__(self, delay_ms: int): self._delay = delay_ms / 1000.0
    def bind(self, **_): return self
    def invoke(self, messages):
        if self._delay > 0:
            import time as _t; _t.sleep(self._delay)
        prompt = messages[-1].content if messages else ""
        if "sub-question" in prompt:
            text = "\n".join(f"sub-question {i+1}" for i in range(FANOUT))
        elif "마크다운 종합 보고서" in prompt:
            text = "# Mock Report\n\n## 개요\nmock\n\n## 결론\nmock"
        else:
            text = "Mock answer for: " + prompt[:80]
        return _MockLGResp(text)


if LLM_MOCK_MS >= 0:
    _LLM = _MockLGModel(LLM_MOCK_MS)
else:
    # Single ChatOpenAI client; httpx reuses an HTTP/2 connection pool
    # under the hood so successive calls amortise TCP/TLS setup.
    # OPENAI_API_BASE routes to Groq / vLLM / etc. when set — same
    # env var the NeoGraph side reads in _common.schema_provider().
    _api_base = os.environ.get("OPENAI_API_BASE", "").rstrip("/")
    _kwargs = {"model": DR_MODEL, "api_key": os.environ.get("OPENAI_API_KEY")}
    if _api_base:
        _kwargs["base_url"] = _api_base + ("/v1" if not _api_base.endswith("/v1") else "")
    _LLM = ChatOpenAI(**_kwargs)


class Crawl4AIClient:
    def __init__(self, base_url):
        self.base_url = base_url.rstrip("/")
    def search_markdown(self, query, *, max_chars=8000):
        ddg = f"https://duckduckgo.com/html/?q={urllib.parse.quote_plus(query)}"
        resp = requests.post(
            f"{self.base_url}/md",
            json={"url": ddg, "f": "bm25", "q": query},
            timeout=60,
        )
        resp.raise_for_status()
        data = resp.json()
        if not data.get("success"):
            raise RuntimeError(f"Crawl4AI failed: {data}")
        return (data.get("markdown") or "")[:max_chars]


class _MockSearch:
    def search_markdown(self, query, *, max_chars=8000):
        return f"(mock evidence for: {query[:60]})"


if MOCK_SEARCH:
    SEARCH_CLIENT = _MockSearch()
elif CRAWL4AI_URL:
    SEARCH_CLIENT = Crawl4AIClient(CRAWL4AI_URL)
else:
    SEARCH_CLIENT = None


class State(TypedDict, total=False):
    messages:          Annotated[list[dict], operator.add]
    research_topic:    str
    sub_questions:     list[str]
    research_findings: Annotated[list[dict], operator.add]
    current_question:  str


def _llm_text(prompt: str, *, temperature: float | None = None) -> str:
    """One-shot LLM call returning bare text."""
    llm = _LLM if temperature is None else _LLM.bind(temperature=temperature)
    resp = llm.invoke([HumanMessage(content=prompt)])
    return resp.content if isinstance(resp.content, str) else str(resp.content)


def router(state: State) -> Command:
    msgs = state.get("messages") or []
    last = next((m for m in reversed(msgs) if m.get("role") == "user"), None)
    if last and RESEARCH_TRIGGER_PATTERN.search(last.get("content", "")):
        return Command(
            goto="research_plan",
            update={"research_topic": last["content"]})
    return Command(goto="general_chat")


def general_chat(state: State) -> dict:
    msgs = state.get("messages") or []
    text = _llm_text("\n".join(
        f"{m['role']}: {m['content']}" for m in msgs))
    return {"messages": [{"role": "assistant", "content": text}]}


def research_plan(state: State) -> dict:
    topic = state.get("research_topic") or ""
    text = _llm_text(
        "다음 주제를 심층 조사하기 위한 sub-question 3-5개로 분해. "
        "각각 독립적으로 답변 가능한 형태. 한 줄에 하나, 번호/글머리표 없이.\n\n"
        f"주제: {topic}",
        temperature=0.0)
    qs = [
        line.strip().lstrip("-•0123456789. ")
        for line in text.strip().splitlines()
        if line.strip()
    ][:FANOUT]
    return {"sub_questions": qs}


def research_fanout(state: State) -> list[Send]:
    """Conditional edge function — LangGraph 1.x semantic for Send fan-out.
    Equivalent to NeoGraph's FanOutNode.execute_full returning Sends, but
    LangGraph requires this to be wired as a conditional edge, not a node."""
    return [Send("researcher", {"current_question": q})
            for q in (state.get("sub_questions") or [])]


def researcher(state: State) -> Command:
    q = state.get("current_question") or ""
    evidence = ""
    if SEARCH_CLIENT:
        try:
            evidence = SEARCH_CLIENT.search_markdown(q)
        except Exception as exc:
            evidence = f"(web search failed: {exc})"
        prompt = (
            f"Question: {q}\n\nWeb search results (markdown):\n{evidence}\n\n"
            "Using the search results above as primary evidence, write a "
            "detailed factual answer. Cite specific snippets when relevant. "
            "If the search results don't cover the question, say so and "
            "answer from general knowledge with that caveat.")
    else:
        prompt = (f"Question: {q}\n\nAnswer from your general knowledge. "
                  "Be detailed and factual. Note any uncertainty.")
    text = _llm_text(prompt)
    return Command(
        goto="synthesize",
        update={"research_findings": [{
            "question": q,
            "answer":   text.strip(),
            "had_web_evidence": bool(SEARCH_CLIENT),
        }]})


def synthesize(state: State) -> dict:
    topic    = state.get("research_topic") or ""
    findings = state.get("research_findings") or []
    sections = "\n\n".join(
        f"### {f['question']}\n\n{f['answer']}" for f in findings)
    text = _llm_text(
        f"아래는 '{topic}'에 대한 sub-question별 조사 결과입니다. 이를 통합해서 "
        "마크다운 종합 보고서를 작성하세요. 구조: 개요(2-3 문장) → 주요 발견 → 결론.\n\n"
        f"--- 조사 결과 ---\n\n{sections}")
    return {"messages": [{"role": "assistant", "content": text}]}


_g = StateGraph(State)
_g.add_node("router",        router)
_g.add_node("general_chat",  general_chat)
_g.add_node("research_plan", research_plan)
_g.add_node("researcher",    researcher)
_g.add_node("synthesize",    synthesize)
_g.add_edge(START,           "router")
_g.add_edge("general_chat",  END)
_g.add_conditional_edges("research_plan", research_fanout, ["researcher"])
_g.add_edge("synthesize",    END)


_checkpointer = None
_compiled = None


_pool = None


def _get_compiled():
    global _checkpointer, _compiled, _pool
    if _compiled is not None:
        return _compiled
    if (USE_INMEMORY or LLM_MOCK_MS >= 0) or not PG_DSN:
        from langgraph.checkpoint.memory import InMemorySaver
        _checkpointer = InMemorySaver()
    else:
        from langgraph.checkpoint.postgres import PostgresSaver
        from psycopg_pool import ConnectionPool
        _pool = ConnectionPool(
            PG_DSN, min_size=1, max_size=4,
            kwargs={"autocommit": True, "prepare_threshold": 0})
        _pool.wait()
        _checkpointer = PostgresSaver(_pool)
        _checkpointer.setup()
    _compiled = _g.compile(checkpointer=_checkpointer)
    return _compiled


def run_query(query: str, thread_id: str) -> str:
    app = _get_compiled()
    result = app.invoke(
        {"messages": [{"role": "user", "content": query}]},
        config={"configurable": {"thread_id": thread_id}, "recursion_limit": 25},
    )
    msgs = result.get("messages") or []
    last = next(
        (m for m in reversed(msgs) if m.get("role") == "assistant"), None)
    return last["content"] if last else ""


if __name__ == "__main__":
    import time, uuid
    q = "사과에 대해서 조사해줘"
    t0 = time.perf_counter()
    out = run_query(q, f"smoke-{uuid.uuid4().hex[:8]}")
    elapsed = time.perf_counter() - t0
    print(f"[langgraph] {elapsed:.2f}s · {len(out)} chars")
    print(out[:200])
