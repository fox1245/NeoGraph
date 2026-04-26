"""NeoGraph deep-research as a callable. Same graph as example 17,
stripped of Gradio. Returns the final assistant markdown report.

Used by bench.py to time end-to-end runs."""

from __future__ import annotations

import os
import re
import sys
import urllib.parse
from pathlib import Path

import requests

EX_DIR = Path(__file__).resolve().parents[2] / "bindings" / "python" / "examples"
sys.path.insert(0, str(EX_DIR))

from _common import ng, schema_provider  # noqa: E402

CRAWL4AI_URL = os.environ.get("CRAWL4AI_URL", "").rstrip("/")
PG_DSN       = os.environ.get("NEOGRAPH_PG_DSN", "")
DR_MODEL     = os.environ.get("DR_MODEL", "gpt-5.4-mini")
# NG_TRANSPORT switches the provider transport for apples-to-apples
# comparison vs LangGraph's HTTP chat_completions:
#   ws-responses  (default): WebSocket Responses API — example 17 prod config.
#   http-chat              : HTTP /v1/chat/completions — same endpoint LangGraph
#                            hits via langchain_openai. Use for bench isolation.
NG_TRANSPORT = os.environ.get("NG_TRANSPORT", "ws-responses")

# Bench-mode env knobs (engine-throughput isolation):
#   LLM_MOCK_MS   — if >0, replace PROVIDER with a deterministic mock that
#                   sleeps for the given milliseconds per .complete() call
#                   (simulates LLM latency without hitting the network).
#   MOCK_SEARCH   — "1" → skip Crawl4AI, return canned evidence string.
#   FANOUT        — number of sub-questions to generate (default 5).
LLM_MOCK_MS = int(os.environ.get("LLM_MOCK_MS", "-1"))  # -1 = real LLM
MOCK_SEARCH = os.environ.get("MOCK_SEARCH", "0") == "1"
FANOUT      = int(os.environ.get("FANOUT", "5"))
USE_INMEMORY = os.environ.get("USE_INMEMORY_CP", "0") == "1"

RESEARCH_TRIGGER_PATTERN = re.compile(
    r"(조사|리서치|연구|research|investigate|deep[- ]?dive)", re.IGNORECASE)


class _MockNGProvider:
    """Drop-in stand-in for SchemaProvider — same .complete() shape, no network."""
    def __init__(self, delay_ms: int): self._delay = delay_ms / 1000.0
    def complete(self, params):
        if self._delay > 0:
            import time as _t; _t.sleep(self._delay)
        prompt = params.messages[-1].content if params.messages else ""
        if "sub-question" in prompt:
            text = "\n".join(f"sub-question {i+1}" for i in range(FANOUT))
        elif "마크다운 종합 보고서" in prompt:
            text = "# Mock Report\n\n## 개요\nmock\n\n## 결론\nmock"
        else:
            text = "Mock answer for: " + prompt[:80]
        class _M:  __slots__ = ("content",)
        m = _M(); m.content = text
        class _C:  __slots__ = ("message",)
        c = _C(); c.message = m
        return c


if LLM_MOCK_MS >= 0:
    PROVIDER = _MockNGProvider(LLM_MOCK_MS)
elif NG_TRANSPORT == "http-chat":
    PROVIDER = schema_provider(
        schema="openai", default_model=DR_MODEL, use_websocket=False)
elif NG_TRANSPORT == "ws-responses":
    PROVIDER = schema_provider(
        schema="openai_responses", default_model=DR_MODEL, use_websocket=True)
else:
    raise ValueError(f"unknown NG_TRANSPORT: {NG_TRANSPORT}")


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


class RouterNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute_full(self, state):
        msgs = state.get("messages") or []
        last = next((m for m in reversed(msgs) if m.get("role") == "user"), None)
        if last and RESEARCH_TRIGGER_PATTERN.search(last.get("content", "")):
            return ng.Command(
                goto_node="research_plan",
                updates=[ng.ChannelWrite("research_topic", last["content"])])
        return ng.Command(goto_node="general_chat")


class GeneralChatNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute(self, state):
        c = PROVIDER.complete(ng.CompletionParams(messages=state.get_messages()))
        return [ng.ChannelWrite("messages", [{
            "role": "assistant", "content": c.message.content}])]


class ResearchPlanNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute(self, state):
        topic = state.get("research_topic") or ""
        c = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(role="user", content=(
                "다음 주제를 심층 조사하기 위한 sub-question 3-5개로 분해. "
                "각각 독립적으로 답변 가능한 형태. 한 줄에 하나, 번호/글머리표 없이.\n\n"
                f"주제: {topic}"))],
            temperature=0.0))
        qs = [
            line.strip().lstrip("-•0123456789. ")
            for line in c.message.content.strip().splitlines()
            if line.strip()
        ][:FANOUT]
        return [ng.ChannelWrite("sub_questions", qs)]


class FanOutNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute_full(self, state):
        return [ng.Send("researcher", {"current_question": q})
                for q in (state.get("sub_questions") or [])]


class ResearcherNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute_full(self, state):
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
        c = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(role="user", content=prompt)]))
        return [
            ng.ChannelWrite("research_findings", [{
                "question": q,
                "answer":   c.message.content.strip(),
                "had_web_evidence": bool(SEARCH_CLIENT),
            }]),
            ng.Command(goto_node="synthesize"),
        ]


class SynthesizeNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def execute(self, state):
        topic    = state.get("research_topic") or ""
        findings = state.get("research_findings") or []
        sections = "\n\n".join(
            f"### {f['question']}\n\n{f['answer']}" for f in findings)
        prompt = (
            f"아래는 '{topic}'에 대한 sub-question별 조사 결과입니다. 이를 통합해서 "
            "마크다운 종합 보고서를 작성하세요. 구조: 개요(2-3 문장) → 주요 발견 → 결론.\n\n"
            f"--- 조사 결과 ---\n\n{sections}")
        c = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(role="user", content=prompt)]))
        return [ng.ChannelWrite("messages", [{
            "role": "assistant", "content": c.message.content}])]


for tn, fac in [
    ("router_n",          RouterNode),
    ("general_chat_n",    GeneralChatNode),
    ("research_plan_n",   ResearchPlanNode),
    ("fanout_n",          FanOutNode),
    ("researcher_n",      ResearcherNode),
    ("synthesize_n",      SynthesizeNode),
]:
    ng.NodeFactory.register_type(
        tn, lambda name, config, ctx, _f=fac: _f(name))


_definition = {
    "name": "dr_bench_neograph",
    "channels": {
        "messages":          {"reducer": "append"},
        "research_topic":    {"reducer": "overwrite"},
        "sub_questions":     {"reducer": "overwrite"},
        "research_findings": {"reducer": "append"},
        "current_question":  {"reducer": "overwrite"},
    },
    "nodes": {
        "router":           {"type": "router_n"},
        "general_chat":     {"type": "general_chat_n"},
        "research_plan":    {"type": "research_plan_n"},
        "research_fanout":  {"type": "fanout_n"},
        "researcher":       {"type": "researcher_n"},
        "synthesize":       {"type": "synthesize_n"},
    },
    "edges": [
        {"from": ng.START_NODE,   "to": "router"},
        {"from": "general_chat",  "to": ng.END_NODE},
        {"from": "research_plan", "to": "research_fanout"},
        {"from": "synthesize",    "to": ng.END_NODE},
    ],
}

_engine = ng.GraphEngine.compile(_definition, ng.NodeContext())
_engine.set_worker_count(int(os.environ.get("NG_WORKER_COUNT", "4")))
if USE_INMEMORY or LLM_MOCK_MS >= 0:
    _engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
elif PG_DSN and getattr(ng, "_HAVE_POSTGRES", False):
    _engine.set_checkpoint_store(ng.PostgresCheckpointStore(PG_DSN, 4))
else:
    _engine.set_checkpoint_store(ng.InMemoryCheckpointStore())


def run_query(query: str, thread_id: str) -> str:
    cfg = ng.RunConfig(
        thread_id=thread_id,
        input={"messages": [{"role": "user", "content": query}]},
        max_steps=20,
    )
    result = _engine.run(cfg)
    msgs = result.output["channels"]["messages"]["value"]
    last = next(
        (m for m in reversed(msgs) if m.get("role") == "assistant"), None)
    return last["content"] if last else ""


if __name__ == "__main__":
    import time, uuid
    q = "사과에 대해서 조사해줘"
    t0 = time.perf_counter()
    out = run_query(q, f"smoke-{uuid.uuid4().hex[:8]}")
    elapsed = time.perf_counter() - t0
    print(f"[neograph] {elapsed:.2f}s · {len(out)} chars")
    print(out[:200])
