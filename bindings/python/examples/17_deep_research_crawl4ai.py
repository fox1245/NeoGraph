"""17 — Deep research with real web search (Crawl4AI) + Postgres checkpoints.

The production-grade companion to `16_deep_research_chat.py`. Same
graph shape, but the researcher branches actually search the web —
they POST queries to a local Crawl4AI container, parse the markdown
result list, and hand the snippets back to the LLM. State is
durable in Postgres rather than in-process memory, so a UI restart
preserves the conversation.

Mirrors the C++ example 25_deep_research.cpp's web-search path
(Crawl4AI + DuckDuckGo HTML), now reachable from Python.

## Setup

    # 1. Crawl4AI container — exposes POST /md returning markdown.
    docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \\
        unclecode/crawl4ai:latest

    # 2. Postgres — anywhere libpq can reach.
    docker run -d -p 5432:5432 \\
        -e POSTGRES_PASSWORD=test -e POSTGRES_DB=neograph \\
        --name neograph-pg postgres:16-alpine

    # 3. The binding needs to ship PostgresCheckpointStore. The PyPI
    #    wheel skips it (libpq bundling pending); install from source:
    pip install scikit-build-core pybind11 ninja
    pip install --no-build-isolation \\
        -e /path/to/NeoGraph -C cmake.args=-DNEOGRAPH_BUILD_POSTGRES=ON

    # 4. Env (drop into examples/.env or export):
    export OPENAI_API_KEY=sk-...
    export CRAWL4AI_URL=http://localhost:11235
    export NEOGRAPH_PG_DSN=postgresql://postgres:test@localhost:5432/neograph

    # 5. Run.
    pip install gradio requests
    python 17_deep_research_crawl4ai.py

If `CRAWL4AI_URL` is unset, researchers fall back to LLM-only
answers (same as example 16). If `NEOGRAPH_PG_DSN` is unset OR the
binding wasn't built with NEOGRAPH_BUILD_POSTGRES=ON, the example
falls back to InMemoryCheckpointStore.
"""

from __future__ import annotations

import os
import re
import urllib.parse

import requests

from _common import ng, schema_provider


# ─── Config (env-driven) ─────────────────────────────────────────────

CRAWL4AI_URL = os.environ.get("CRAWL4AI_URL", "").rstrip("/")
PG_DSN = os.environ.get("NEOGRAPH_PG_DSN", "")

RESEARCH_TRIGGER_PATTERN = re.compile(
    r"(조사|리서치|연구|research|investigate|deep[- ]?dive)", re.IGNORECASE)

PROVIDER = schema_provider(
    schema="openai_responses",
    default_model="gpt-4o-mini",
    use_websocket=True,
)


# ─── Crawl4AI client ─────────────────────────────────────────────────

class Crawl4AIClient:
    """Thin POST /md wrapper. Mirrors the C++ Crawl4AIClient in
    examples/25_deep_research.cpp."""

    def __init__(self, base_url: str):
        if not base_url:
            raise ValueError("Crawl4AIClient requires a non-empty base_url")
        self.base_url = base_url.rstrip("/")

    def search_markdown(self, query: str, *, max_chars: int = 8000) -> str:
        """Search via DuckDuckGo HTML, return cleaned markdown of the
        result list. Capped to keep researcher context small."""
        ddg = f"https://duckduckgo.com/html/?q={urllib.parse.quote_plus(query)}"
        resp = requests.post(
            f"{self.base_url}/md",
            json={"url": ddg, "f": "bm25", "q": query},
            timeout=60,
        )
        resp.raise_for_status()
        data = resp.json()
        if not data.get("success"):
            raise RuntimeError(f"Crawl4AI reported failure: {data}")
        md = data.get("markdown", "") or ""
        return md[:max_chars]


SEARCH_CLIENT: Crawl4AIClient | None
if CRAWL4AI_URL:
    SEARCH_CLIENT = Crawl4AIClient(CRAWL4AI_URL)
    print(f"[17] using Crawl4AI at {CRAWL4AI_URL}")
else:
    SEARCH_CLIENT = None
    print("[17] CRAWL4AI_URL unset — researchers will rely on LLM knowledge "
          "without web search. (See module docstring to wire it up.)")


# ─── Custom nodes (router / chat / research pipeline) ────────────────

class RouterNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute_full(self, state):
        msgs = state.get("messages") or []
        last_user = next(
            (m for m in reversed(msgs) if m.get("role") == "user"), None)
        if last_user and RESEARCH_TRIGGER_PATTERN.search(last_user.get("content", "")):
            return ng.Command(
                goto_node="research_plan",
                updates=[ng.ChannelWrite("research_topic", last_user["content"])],
            )
        return ng.Command(goto_node="general_chat")


class GeneralChatNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        completion = PROVIDER.complete(
            ng.CompletionParams(messages=state.get_messages()))
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": completion.message.content,
        }])]


class ResearchPlanNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        topic = state.get("research_topic") or ""
        completion = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(role="user", content=(
                "다음 주제를 심층 조사하기 위한 sub-question 3-5개로 분해. "
                "각각 독립적으로 답변 가능한 형태. 한 줄에 하나, 번호/글머리표 없이.\n\n"
                f"주제: {topic}"
            ))],
            temperature=0.0,
        ))
        questions = [
            line.strip().lstrip("-•0123456789. ")
            for line in completion.message.content.strip().splitlines()
            if line.strip()
        ][:5]
        return [ng.ChannelWrite("sub_questions", questions)]


class FanOutResearchNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute_full(self, state):
        questions = state.get("sub_questions") or []
        return [ng.Send("researcher", {"current_question": q}) for q in questions]


class ResearcherNode(ng.GraphNode):
    """Per-branch researcher: optional web search + LLM synthesis."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        q = state.get("current_question") or ""

        if SEARCH_CLIENT:
            try:
                evidence = SEARCH_CLIENT.search_markdown(q)
            except Exception as exc:
                evidence = f"(web search failed: {exc})"
            prompt = (
                f"Question: {q}\n\n"
                f"Web search results (markdown):\n{evidence}\n\n"
                "Using the search results above as primary evidence, write a "
                "detailed factual answer. Cite specific snippets when relevant. "
                "If the search results don't cover the question, say so and "
                "answer from general knowledge with that caveat."
            )
        else:
            prompt = (
                f"Question: {q}\n\n"
                "Answer from your general knowledge. Be detailed and factual. "
                "Note any uncertainty."
            )

        completion = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(role="user", content=prompt)],
        ))
        return [ng.ChannelWrite("research_findings", [{
            "question": q,
            "answer":   completion.message.content.strip(),
            "had_web_evidence": SEARCH_CLIENT is not None,
        }])]


class SynthesizeNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def execute(self, state):
        topic    = state.get("research_topic") or ""
        findings = state.get("research_findings") or []
        sections = "\n\n".join(
            f"### {f['question']}\n\n{f['answer']}" for f in findings)
        prompt = (
            f"아래는 '{topic}'에 대한 sub-question별 조사 결과입니다. 이를 통합해서 "
            "마크다운 종합 보고서를 작성하세요. 구조: 개요(2-3 문장) → 주요 발견 → 결론.\n\n"
            f"--- 조사 결과 ---\n\n{sections}"
        )
        completion = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(role="user", content=prompt)],
        ))
        return [
            ng.ChannelWrite("messages", [{
                "role": "assistant",
                "content": completion.message.content,
            }]),
            ng.ChannelWrite("sub_questions", []),
            ng.ChannelWrite("research_findings", []),
            ng.ChannelWrite("research_topic", ""),
        ]


# ─── Graph wiring ────────────────────────────────────────────────────

for type_name, factory in [
    ("router",          RouterNode),
    ("general_chat",    GeneralChatNode),
    ("research_plan",   ResearchPlanNode),
    ("research_fanout", FanOutResearchNode),
    ("researcher",      ResearcherNode),
    ("synthesize",      SynthesizeNode),
]:
    ng.NodeFactory.register_type(
        type_name, lambda name, config, ctx, _f=factory: _f(name))


definition = {
    "name": "deep_research_crawl4ai",
    "channels": {
        "messages":          {"reducer": "append"},
        "research_topic":    {"reducer": "overwrite"},
        "sub_questions":     {"reducer": "overwrite"},
        "research_findings": {"reducer": "append"},
    },
    "nodes": {
        "router":           {"type": "router"},
        "general_chat":     {"type": "general_chat"},
        "research_plan":    {"type": "research_plan"},
        "research_fanout":  {"type": "research_fanout"},
        "researcher":       {"type": "researcher"},
        "synthesize":       {"type": "synthesize"},
    },
    "edges": [
        {"from": ng.START_NODE,   "to": "router"},
        {"from": "general_chat",  "to": ng.END_NODE},
        {"from": "research_plan", "to": "research_fanout"},
        {"from": "researcher",    "to": "synthesize"},
        {"from": "synthesize",    "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
engine.set_worker_count(4)


# Pick the durable store when available; otherwise fall back.
def _make_checkpoint_store():
    if PG_DSN and getattr(ng, "_HAVE_POSTGRES", False):
        try:
            store = ng.PostgresCheckpointStore(PG_DSN, 4)
            print(f"[17] using PostgresCheckpointStore "
                  f"(pool_size={store.pool_size})")
            return store
        except Exception as exc:
            print(f"[17] Postgres unreachable ({exc}); "
                  f"falling back to InMemoryCheckpointStore.")
    elif PG_DSN:
        print("[17] NEOGRAPH_PG_DSN set but the binding wasn't built with "
              "NEOGRAPH_BUILD_POSTGRES=ON. See module docstring. "
              "Falling back to InMemoryCheckpointStore.")
    else:
        print("[17] NEOGRAPH_PG_DSN unset — using InMemoryCheckpointStore "
              "(state lost when this process exits).")
    return ng.InMemoryCheckpointStore()


engine.set_checkpoint_store(_make_checkpoint_store())


# ─── Gradio frontend ─────────────────────────────────────────────────

def chat(message, history):
    msgs = []
    for h in (history or []):
        if isinstance(h, dict):
            msgs.append({"role": h["role"], "content": h.get("content", "")})
        elif isinstance(h, (list, tuple)) and len(h) == 2:
            user_msg, assistant_msg = h
            if user_msg:
                msgs.append({"role": "user", "content": user_msg})
            if assistant_msg:
                msgs.append({"role": "assistant", "content": assistant_msg})
    msgs.append({"role": "user", "content": message})

    cfg = ng.RunConfig(
        thread_id="gradio-session",
        input={"messages": msgs},
        max_steps=20,
    )
    result = engine.run(cfg)
    out_msgs = result.output["channels"]["messages"]["value"]
    last = next(
        (m for m in reversed(out_msgs) if m.get("role") == "assistant"), None)
    return last["content"] if last else "(no response)"


if __name__ == "__main__":
    try:
        import gradio as gr
    except ImportError:
        print("gradio is required:  pip install gradio")
        raise SystemExit(1)

    title = "NeoGraph — deep research (Crawl4AI + Postgres)"
    if not SEARCH_CLIENT:
        title += " [LLM-only fallback]"
    gr.ChatInterface(
        fn=chat,
        type="messages",
        title=title,
        description=(
            "조사어가 들어가면 web-search 기반 deep research → 마크다운 보고서. "
            "예: `사과에 대해서 조사해줘`"
        ),
        examples=[
            "안녕! 자기소개해줘.",
            "사과에 대해서 조사해줘",
            "비잔틴 제국의 멸망 원인을 deep dive해줘",
        ],
    ).launch()
