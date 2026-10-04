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

from _common import ask_text, ng, schema_provider  # noqa: E402

CRAWL4AI_URL = os.environ.get("CRAWL4AI_URL", "").rstrip("/")
PG_DSN       = os.environ.get("NEOGRAPH_PG_DSN", "")
DR_MODEL     = os.environ.get("DR_MODEL", "gpt-5.4-mini")
# Match LangGraph's HTTP Chat endpoint by default. HTTP Responses deliberately
# compares a different wire API; it is not a transport-only isolation run.
NG_TRANSPORT = os.environ.get("NG_TRANSPORT", "http-chat")

# Bench-mode env knobs (engine-throughput isolation):
#   LLM_MOCK_MS   — if >=0, use plain text orchestration computation with this
#                   delay per node call. No provider requests/outcomes/network.
#   MOCK_SEARCH   — "1" → skip Crawl4AI, return canned evidence string.
#   FANOUT        — number of sub-questions to generate (default 5).
#   NG_WORKER_COUNT — worker pool size for Send fan-out (default 4).
LLM_MOCK_MS = int(os.environ.get("LLM_MOCK_MS", "-1"))  # -1 = real LLM
MOCK_SEARCH = os.environ.get("MOCK_SEARCH", "0") == "1"
FANOUT      = int(os.environ.get("FANOUT", "5"))
NG_WORKER_COUNT = int(os.environ.get("NG_WORKER_COUNT", "4"))
USE_INMEMORY = os.environ.get("USE_INMEMORY_CP", "0") == "1"

RESEARCH_TRIGGER_PATTERN = re.compile(
    r"(조사|리서치|연구|research|investigate|deep[- ]?dive)", re.IGNORECASE)


def _mock_text(messages):
    """Canned orchestration workload, not a Provider or provider evidence."""
    if LLM_MOCK_MS > 0:
        import time
        time.sleep(LLM_MOCK_MS / 1000.0)
    prompt = messages[-1]["content"] if messages else ""
    if "sub-question" in prompt:
        return "\n".join(f"sub-question {i+1}" for i in range(FANOUT))
    if "마크다운 종합 보고서" in prompt:
        return "# Mock Report\n\n## 개요\nmock\n\n## 결론\nmock"
    return "Mock answer for: " + prompt[:80]


if NG_TRANSPORT not in {"http-chat", "http-responses"}:
    raise ValueError(f"unsupported NG_TRANSPORT: {NG_TRANSPORT}; "
                     "choose http-chat or http-responses")
PROVIDER = (None if LLM_MOCK_MS >= 0 else schema_provider(
    schema="openai" if NG_TRANSPORT == "http-chat" else "openai_responses"))


def _workload_text(messages, *, temperature=None):
    if LLM_MOCK_MS >= 0:
        return _mock_text(messages)
    return ask_text(PROVIDER, messages, model=DR_MODEL, temperature=temperature)


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
    def run(self, input):
        state = input.state
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
    def run(self, input):
        state = input.state
        text = _workload_text(state.get("messages") or [])
        return [ng.ChannelWrite("messages", [{
            "role": "assistant", "content": text}])]


class ResearchPlanNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def run(self, input):
        state = input.state
        topic = state.get("research_topic") or ""
        text = _workload_text(
            [{"role": "user", "content": (
                "다음 주제를 심층 조사하기 위한 sub-question 3-5개로 분해. "
                "각각 독립적으로 답변 가능한 형태. 한 줄에 하나, 번호/글머리표 없이.\n\n"
                f"주제: {topic}")}],
            temperature=0.0)
        qs = [
            line.strip().lstrip("-•0123456789. ")
            for line in text.strip().splitlines()
            if line.strip()
        ][:FANOUT]
        return [ng.ChannelWrite("sub_questions", qs)]


class FanOutNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def run(self, input):
        state = input.state
        return [ng.Send("researcher", {"current_question": q})
                for q in (state.get("sub_questions") or [])]


class ResearcherNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def run(self, input):
        state = input.state
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
        text = _workload_text([{"role": "user", "content": prompt}])
        return [
            ng.ChannelWrite("research_findings", [{
                "question": q,
                "answer":   text.strip(),
                "had_web_evidence": bool(SEARCH_CLIENT),
            }]),
            ng.Command(goto_node="synthesize"),
        ]


class SynthesizeNode(ng.GraphNode):
    def __init__(self, name): super().__init__(); self._n = name
    def get_name(self): return self._n
    def run(self, input):
        state = input.state
        topic    = state.get("research_topic") or ""
        findings = state.get("research_findings") or []
        sections = "\n\n".join(
            f"### {f['question']}\n\n{f['answer']}" for f in findings)
        prompt = (
            f"아래는 '{topic}'에 대한 sub-question별 조사 결과입니다. 이를 통합해서 "
            "마크다운 종합 보고서를 작성하세요. 구조: 개요(2-3 문장) → 주요 발견 → 결론.\n\n"
            f"--- 조사 결과 ---\n\n{sections}")
        text = _workload_text([{"role": "user", "content": prompt}])
        return [ng.ChannelWrite("messages", [{
            "role": "assistant", "content": text}])]


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
_engine.set_worker_count(NG_WORKER_COUNT)
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
    print(f"[neograph] workload={'mock orchestration' if LLM_MOCK_MS >= 0 else NG_TRANSPORT}"
          f" · mock_delay_ms={LLM_MOCK_MS}")
    t0 = time.perf_counter()
    out = run_query(q, f"smoke-{uuid.uuid4().hex[:8]}")
    elapsed = time.perf_counter() - t0
    print(f"[neograph] {elapsed:.2f}s · {len(out)} chars")
    print(out[:200])
