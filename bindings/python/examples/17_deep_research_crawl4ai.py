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

    # 3. neograph-engine wheel (>= 0.1.3) ships libpq bundled, so
    #    PostgresCheckpointStore works out of the box — no source build:
    pip install 'neograph-engine>=0.1.3'

    # 4. Env (drop into examples/.env or export):
    export OPENAI_API_KEY=sk-...
    export CRAWL4AI_URL=http://localhost:11235
    export NEOGRAPH_PG_DSN=postgresql://postgres:test@localhost:5432/neograph

    # 5. Run.
    pip install gradio requests
    python 17_deep_research_crawl4ai.py

If `CRAWL4AI_URL` is unset, researchers fall back to LLM-only
answers (same as example 16). If `NEOGRAPH_PG_DSN` is unset, the
example falls back to InMemoryCheckpointStore.
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
    default_model=os.environ.get("DR_MODEL", "gpt-5.4-mini"),
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

    def execute_full(self, state):
        q = state.get("current_question") or ""

        evidence = ""
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
        # ChannelWrite + Command(goto=synthesize). Each Send-spawned
        # researcher routes itself to synthesize; the engine joins the
        # five gotos into a single ready=[synthesize] for the next
        # super-step (LangGraph parity).
        return [
            ng.ChannelWrite("research_findings", [{
                "question": q,
                "answer":   completion.message.content.strip(),
                "had_web_evidence": bool(SEARCH_CLIENT),
                "evidence_excerpt": evidence[:1500],
            }]),
            ng.Command(goto_node="synthesize"),
        ]


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
        # Per-researcher input via Send.input — needs an explicit
        # channel definition or the engine drops it before the
        # researcher node sees it.
        "current_question":  {"reducer": "overwrite"},
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


# ─── Gradio frontend (streaming) ────────────────────────────────────
#
# Streaming via run_stream: engine emits GraphEvent on a worker thread,
# a queue bridges them to the Gradio chat generator. We don't have
# token-level streaming here (PROVIDER.complete() is non-stream), but
# node-level progress + the final synthesized message updates live so
# the user sees what's happening instead of staring at a spinner.

import queue
import threading


def _node_label(name: str) -> str:
    return {
        "router":          "🔀 router",
        "general_chat":    "💬 chat",
        "research_plan":   "🧭 sub-question 분해",
        "research_fanout": "📡 fan-out",
        "researcher":      "🔎 researcher",
        "synthesize":      "📝 보고서 합성",
    }.get(name, name)


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
        stream_mode=ng.StreamMode.ALL,
    )

    q: queue.Queue = queue.Queue()
    DONE = object()
    final_assistant: list[str] = []
    sub_questions: list[str] = []
    findings: list[dict] = []  # per-researcher {question, answer, evidence_excerpt}

    def on_event(ev):
        try:
            t = ev.type
            if t == ng.GraphEvent.Type.NODE_START:
                if not ev.node_name.startswith("__"):
                    q.put(("progress", f"▶ {_node_label(ev.node_name)}"))
            elif t == ng.GraphEvent.Type.NODE_END:
                if not ev.node_name.startswith("__"):
                    q.put(("progress", f"✓ {_node_label(ev.node_name)}"))
            elif t == ng.GraphEvent.Type.CHANNEL_WRITE:
                ch = ev.data.get("channel") if isinstance(ev.data, dict) else None
                if ch == "messages":
                    val = ev.data.get("value")
                    items = val if isinstance(val, list) else [val]
                    for m in items:
                        if isinstance(m, dict) and m.get("role") == "assistant":
                            final_assistant.append(m.get("content", ""))
                elif ch == "sub_questions":
                    val = ev.data.get("value")
                    if isinstance(val, list) and val:
                        sub_questions[:] = list(val)
                elif ch == "research_findings":
                    val = ev.data.get("value")
                    items = val if isinstance(val, list) else [val]
                    for f in items:
                        if isinstance(f, dict):
                            findings.append(f)
            elif t == ng.GraphEvent.Type.ERROR:
                q.put(("error", str(ev.data)))
        except Exception as exc:
            q.put(("error", f"event handler crash: {exc}"))

    def runner():
        try:
            engine.run_stream(cfg, on_event)
        except Exception as exc:
            q.put(("error", str(exc)))
        finally:
            q.put(DONE)

    threading.Thread(target=runner, daemon=True).start()

    progress_lines: list[str] = []
    while True:
        item = q.get()
        if item is DONE:
            break
        kind, payload = item
        if kind == "error":
            yield "\n".join(progress_lines + [f"\n❌ {payload}"])
            return
        progress_lines.append(payload)
        yield "\n".join(progress_lines)

    # ── Final render: keep a collapsed trace + show the report ─────────
    parts: list[str] = []

    if sub_questions or findings:
        trace_md = ["<details><summary>🔍 사고 과정 보기 (클릭)</summary>", ""]
        if sub_questions:
            trace_md.append("**Sub-questions**")
            for i, sq in enumerate(sub_questions, 1):
                trace_md.append(f"{i}. {sq}")
            trace_md.append("")
        for i, f in enumerate(findings, 1):
            ev_block = (f.get("evidence_excerpt") or "").strip()
            answer = (f.get("answer") or "").strip()
            web_tag = "🌐 web" if f.get("had_web_evidence") else "🧠 LLM-only"
            trace_md.append(
                f"<details><summary>🔎 researcher #{i} — {web_tag}</summary>\n"
            )
            q_text = f.get("question") or (
                sub_questions[i-1] if i-1 < len(sub_questions) else "")
            if q_text:
                trace_md.append(f"**Q:** {q_text}\n")
            if ev_block:
                trace_md.append("**Web evidence (excerpt):**\n")
                trace_md.append(f"```\n{ev_block}\n```\n")
            if answer:
                trace_md.append("**Researcher answer:**\n")
                trace_md.append(answer + "\n")
            trace_md.append("</details>\n")
        trace_md.append("</details>")
        parts.append("\n".join(trace_md))

    if final_assistant:
        parts.append(final_assistant[-1])
    else:
        parts.append("\n".join(progress_lines + ["\n(no assistant message produced)"]))

    yield "\n\n".join(parts)


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
