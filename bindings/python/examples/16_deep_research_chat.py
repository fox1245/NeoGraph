"""16 — Multi-turn chat + deep-research dispatch (OpenAI Responses WS + Gradio).

A small chat UI with a research-mode trigger. Type a normal question
and you get a normal LLM reply. Say "조사해줘 / research / investigate"
and the engine fans out parallel sub-question researchers via `Send`,
then synthesizes a markdown report.

Pieces wired together:

  - **OpenAI Responses API over WebSocket** —
    `SchemaProvider(schema="openai-responses", use_websocket=True)`.
    Same /v1/responses path as the C++ example
    34_openai_responses_ws_tools.cpp, just driven through the
    binding's high-level Provider surface instead of raw ws_client.

  - **Multi-turn context** — Gradio's ChatInterface keeps the
    user-visible history. Each turn we hand the engine the FULL prior
    transcript as `messages` input, so the LLM sees what came before.
    InMemoryCheckpointStore captures each turn's final state for
    debug / time-travel; swap to PostgresCheckpointStore (binding
    pending) for durable session persistence:

        engine.set_checkpoint_store(
            ng.PostgresCheckpointStore("postgresql://..."))

  - **Deep-research subgraph** — when the router fires `research`
    mode, the graph plans 3-5 sub-questions, fans them out as parallel
    `Send` branches (each researcher node runs an independent LLM
    call), then a synthesize node merges the findings into a single
    markdown report.

Web search is NOT plumbed in this demo — researchers rely on the LLM's
own knowledge. To wire actual web search, swap the ResearcherNode for
one that hits Tavily / Brave / Crawl4AI / OpenAI's built-in
`web_search_preview` tool. The engine surface stays the same.

Run:
    pip install neograph-engine python-dotenv gradio
    cp .env.example .env   # fill in OPENAI_API_KEY
    python 16_deep_research_chat.py

UI opens at http://localhost:7860.
"""

import re

from _common import ng, schema_provider


# Recognise common Korean / English research triggers in the latest
# user message. Anything else falls through to general_chat.
RESEARCH_TRIGGER_PATTERN = re.compile(
    r"(조사|리서치|연구|research|investigate|deep[- ]?dive)", re.IGNORECASE)


# Single shared provider — WS keeps a connection per call, so a
# top-level instance is fine. SchemaProvider with use_websocket=True
# routes streaming completions over wss://api.openai.com/v1/responses.
PROVIDER = schema_provider(
    schema="openai_responses",   # underscore — built-in schema name
    default_model="gpt-4o-mini",
    use_websocket=True,
)


# ─── Custom nodes ────────────────────────────────────────────────────

class RouterNode(ng.GraphNode):
    """Inspect the latest user message; decide chat vs research."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        msgs = input.state.get("messages") or []
        last_user = next(
            (m for m in reversed(msgs) if m.get("role") == "user"), None)

        if last_user and RESEARCH_TRIGGER_PATTERN.search(last_user.get("content", "")):
            return ng.Command(
                goto_node="research_plan",
                updates=[
                    ng.ChannelWrite("research_topic", last_user["content"]),
                ],
            )
        return ng.Command(goto_node="general_chat")


class GeneralChatNode(ng.GraphNode):
    """Plain WS LLM call against the running messages channel."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        msgs = input.state.get_messages()
        completion = PROVIDER.complete(ng.CompletionParams(messages=msgs))
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": completion.message.content,
        }])]


class ResearchPlanNode(ng.GraphNode):
    """Decompose the topic into 3-5 sub-questions."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        topic = input.state.get("research_topic") or ""
        prompt = (
            "다음 주제를 심층 조사할 수 있도록 서로 보완적인 sub-question 3개에서 "
            "5개로 분해하세요. 각 sub-question은 독립적으로 답변할 수 있어야 합니다.\n\n"
            f"주제: {topic}\n\n"
            "Sub-question을 한 줄에 하나씩, 번호나 글머리표 없이 출력하세요."
        )
        completion = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(role="user", content=prompt)],
            temperature=0.0,
        ))
        questions = [
            line.strip().lstrip("-•0123456789. ")
            for line in completion.message.content.strip().splitlines()
            if line.strip()
        ][:5]
        return [ng.ChannelWrite("sub_questions", questions)]


class FanOutResearchNode(ng.GraphNode):
    """One Send per sub-question → parallel researcher branches."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        questions = input.state.get("sub_questions") or []
        return [
            ng.Send("researcher", {"current_question": q}) for q in questions
        ]


class ResearcherNode(ng.GraphNode):
    """Per-branch LLM call answering a single sub-question."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        question = input.state.get("current_question") or ""
        completion = PROVIDER.complete(ng.CompletionParams(
            messages=[ng.ChatMessage(role="user", content=(
                "다음 질문에 알려진 사실 기반으로 상세하고 정확하게 답하세요. "
                "확실하지 않은 부분은 명시하세요.\n\n"
                f"질문: {question}"
            ))],
        ))
        return [ng.ChannelWrite("research_findings", [{
            "question": question,
            "answer":   completion.message.content.strip(),
        }])]


class SynthesizeNode(ng.GraphNode):
    """Merge research_findings into a markdown report; append to messages."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        topic    = input.state.get("research_topic") or ""
        findings = input.state.get("research_findings") or []

        sections = "\n\n".join(
            f"### {f['question']}\n\n{f['answer']}" for f in findings
        )
        prompt = (
            f"아래는 '{topic}'에 대한 sub-question별 조사 결과입니다. 이를 통합해서 "
            "마크다운 형식의 종합 보고서를 작성하세요. 구조: 개요(2-3 문장) → "
            "주요 발견(섹션별) → 결론(2-3 문장).\n\n"
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
            # Reset research-only channels so the next turn starts
            # clean. messages keeps growing because its reducer is
            # append.
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
        type_name,
        lambda name, config, ctx, _f=factory: _f(name),
    )


definition = {
    "name": "deep_research_chat",
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
        # router emits Command(goto=general_chat | research_plan)
        {"from": "general_chat",  "to": ng.END_NODE},
        {"from": "research_plan", "to": "research_fanout"},
        # research_fanout emits Send list → researcher × N (in parallel)
        {"from": "researcher",    "to": "synthesize"},
        {"from": "synthesize",    "to": ng.END_NODE},
    ],
}


engine = ng.GraphEngine.compile(definition, ng.NodeContext())
engine.set_worker_count(4)  # parallel researchers
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())


# ─── Gradio frontend ─────────────────────────────────────────────────

def chat(message, history):
    """ChatInterface callback.

    `history` is Gradio's persisted conversation state — list of
    {"role", "content"} dicts on Gradio 5+, or list of (user, assistant)
    tuples on older versions. We flatten either shape into the
    `messages` channel input so the LLM sees the whole turn-by-turn
    context. Each turn calls engine.run() with a fresh thread_id-keyed
    super-step; checkpoints are saved post-run for debug / time-travel.
    """
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
        thread_id="gradio-session",   # single user; per-session in production
        input={"messages": msgs},
        max_steps=20,                 # caps research fan-out depth
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
        print("gradio is required for the chat UI:")
        print("    pip install gradio")
        raise SystemExit(1)

    gr.ChatInterface(
        fn=chat,
        title="NeoGraph — multi-turn chat + deep research",
        description=(
            "일반 질문은 그냥 chat. **'조사해줘 / research / "
            "investigate'** 가 들어가면 deep-research 모드가 작동해서 "
            "sub-question N개로 분해 → 병렬 조사 → 마크다운 보고서.\n\n"
            "예: `사과에 대해서 조사해줘`"
        ),
        examples=[
            "안녕! 자기소개해줘.",
            "사과에 대해서 조사해줘",
            "비잔틴 제국의 멸망 원인을 deep dive해줘",
        ],
    ).launch()
