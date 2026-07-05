#!/usr/bin/env python3
"""jarvis 오케스트레이션 코어의 LangGraph 쌍둥이 — 프레임워크 오버헤드 벤치용.

NeoGraph cookbook_jarvis(mock 빌드, 텍스트 모드)와 **동일 토폴로지**를 LangGraph 로
미러링한다. 노드 구성, 라우터/합성 프롬프트, 결정 검증(chat 강등), 메모리 저장
포맷(JsonFileStore), 복창 가드, stdout 마커까지 전부 동일 — 다른 것은 그래프
프레임워크와 언어뿐이다.

  mic_capture → stt(한글 휴리스틱 언어감지) → text_or_voice → memory_lookup
  → router(LLM) → [chat|direct|parallel|delegate] → response_synth(LLM)/synth_skip
  → memory_commit → tts(콘솔 마커)

모드 (env BENCH_MODE):
  mock — LLM 0ms 스텁 (C++ MockProvider 와 동일 응답) → 순수 프레임워크 오버헤드
  api  — OpenAI 호환 엔드포인트 (OPENAI_API_KEY / OPENAI_BASE_URL /
         JARVIS_ROUTER_MODEL / JARVIS_SYNTH_MODEL) — Groq/Cerebras 등

프로토콜: stdin 한 줄 = 한 턴. 기동 완료 시 "[jarvis] 온라인." 라인,
턴 완료 시 "[jarvis:tts][<lang>] <final_text>" 라인 출력 (C++ 와 동일 마커).
"""
import json
import operator
import os
import sys
import time
from typing import Annotated, TypedDict

from langgraph.graph import StateGraph, START, END

BENCH_MODE = os.environ.get("BENCH_MODE", "mock")
MEMORY_FILE = os.environ.get("JARVIS_MEMORY_FILE", "jarvis_memory.json")
PERSONA_FILE = os.environ.get("JARVIS_PERSONA_FILE", "config/persona.txt")
NS = ["jarvis", "tony"]
RECENT_TURNS = 6
MAX_KEEP = 24


# ─────────────────────────────────────────────────────────────────────────────
# persona 섹션 로더 — C++ load_prompt_section 과 동일 규칙
# ─────────────────────────────────────────────────────────────────────────────

def load_prompt_section(path: str, section: str) -> str:
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return ""
    target = f"==={section}==="
    buf, in_section = [], False
    for line in lines:
        is_header = len(line) >= 6 and line.startswith("=") and line.endswith("=")
        if is_header:
            if in_section:
                break
            if line == target:
                in_section = True
            continue
        if in_section:
            buf.append(line)
    return "\n".join(buf).rstrip("\n")


# ─────────────────────────────────────────────────────────────────────────────
# JsonFileStore 호환 파일 스토어 — [{ns, key, value}] 포맷 동일
# ─────────────────────────────────────────────────────────────────────────────

def store_load() -> list:
    try:
        with open(MEMORY_FILE, encoding="utf-8") as f:
            data = json.load(f)
            return data if isinstance(data, list) else []
    except (OSError, ValueError):
        return []


def store_get(key: str):
    for item in store_load():
        if item.get("ns") == NS and item.get("key") == key:
            return item.get("value")
    return None


def store_put(key: str, value) -> None:
    data = store_load()
    for item in data:
        if item.get("ns") == NS and item.get("key") == key:
            item["value"] = value
            break
    else:
        data.append({"ns": NS, "key": key, "value": value})
    with open(MEMORY_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)


# ─────────────────────────────────────────────────────────────────────────────
# LLM 어댑터 — mock(0ms 스텁, C++ MockProvider 동일) / api(OpenAI 호환)
# ─────────────────────────────────────────────────────────────────────────────

MOCK_ROUTER_JSON = ('{"mode":"direct","tool_calls":[],"delegate_to":null,'
                    '"skip_synthesis":true,"reasoning_short":"mock router"}')

if BENCH_MODE == "api":
    from langchain_openai import ChatOpenAI

    def _mk(model_env: str, default: str, temp: float, max_tok: int):
        base = os.environ.get("OPENAI_BASE_URL", "")
        kwargs = dict(model=os.environ.get(model_env) or default,
                      temperature=temp, max_tokens=max_tok, timeout=120)
        if base:
            kwargs["base_url"] = base.rstrip("/") + "/v1"
        return ChatOpenAI(**kwargs)

    ROUTER_LLM = _mk("JARVIS_ROUTER_MODEL", "gpt-4o-mini", 0.1, 300)
    SYNTH_LLM = _mk("JARVIS_SYNTH_MODEL", "gpt-4o", 0.4, 220)

    def call_router(system: str, user: str) -> str:
        return ROUTER_LLM.invoke([("system", system), ("human", user)]).content

    def call_synth(messages: list) -> str:
        # 스트리밍 — 첫 청크 도착 시 [jarvis:ttft] 마커. C++ 합성 노드와 동일.
        role_map = {"system": "system", "user": "human", "assistant": "ai"}
        lc = [(role_map[r], c) for r, c in messages]
        parts, emitted = [], False
        for chunk in SYNTH_LLM.stream(lc):
            txt = chunk.content or ""
            if txt and not emitted:
                emitted = True
                print("[jarvis:ttft]", flush=True)
            parts.append(txt)
        return "".join(parts)
else:
    def call_router(system: str, user: str) -> str:  # noqa: ARG001
        # C++ MockProvider: 라우터 호출이면 mock 라우팅 JSON
        return MOCK_ROUTER_JSON

    def call_synth(messages: list) -> str:
        # C++ MockProvider: 마지막 user 메시지 echo. 스트리밍 흉내 — 즉시 마커.
        print("[jarvis:ttft]", flush=True)
        user_text = ""
        for role, content in messages:
            if role == "user":
                user_text = content
        return "[mock] " + user_text


# ─────────────────────────────────────────────────────────────────────────────
# MCP 클라이언트 (E2E 벤치) — env MCP_URL 설정 시 공식 mcp SDK 로 영구 세션.
# C++ McpCatalog 과 동일: 시작 시 도구 목록 수집 → 라우터 프롬프트에 주입,
# direct/parallel 분기가 실호출. 카탈로그 메타(이름/설명/태그/힌트)는
# JARVIS_MCP_CATALOG(json) 에서 — C++ 이 읽는 것과 같은 파일.
# ─────────────────────────────────────────────────────────────────────────────

MCP_URL = os.environ.get("MCP_URL", "")
MCP_TOOLS: dict = {}      # "demo.get_weather" -> description
MCP_ENTRY: dict = {}      # 카탈로그 entry (name/description/tags/hints)
_mcp_loop = None
_mcp_session = None

if MCP_URL:
    import asyncio
    import threading as _threading

    from mcp import ClientSession
    from mcp.client.streamable_http import streamablehttp_client

    _mcp_loop = asyncio.new_event_loop()
    _mcp_ready = _threading.Event()
    _mcp_tools_resp = None

    async def _mcp_holder():
        global _mcp_session, _mcp_tools_resp
        async with streamablehttp_client(
                MCP_URL.rstrip("/") + "/mcp") as (read, write, _):
            async with ClientSession(read, write) as session:
                await session.initialize()
                _mcp_tools_resp = await session.list_tools()
                _mcp_session = session
                _mcp_ready.set()
                await asyncio.Event().wait()      # 프로세스 수명 동안 유지

    def _mcp_thread():
        asyncio.set_event_loop(_mcp_loop)
        _mcp_loop.run_until_complete(_mcp_holder())

    _threading.Thread(target=_mcp_thread, daemon=True).start()
    if not _mcp_ready.wait(timeout=30):
        sys.exit(f"[twin] MCP 연결 실패: {MCP_URL}")

    entry_name = "demo"
    cat_path = os.environ.get("JARVIS_MCP_CATALOG", "")
    if cat_path:
        with open(cat_path, encoding="utf-8") as f:
            cat = json.load(f)
        if cat.get("tools"):
            MCP_ENTRY = dict(cat["tools"][0])
            entry_name = MCP_ENTRY.get("name", "demo")
        MCP_ENTRY["_hints"] = cat.get("routing_hints", {})
    for t in _mcp_tools_resp.tools:
        MCP_TOOLS[f"{entry_name}.{t.name}"] = t.description or ""
    print(f"[twin] MCP 도구 {len(MCP_TOOLS)}개 수집: "
          + ", ".join(MCP_TOOLS), file=sys.stderr)


def mcp_call_async(tool_fq: str, args: dict):
    """도구 호출을 이벤트루프에 제출하고 Future 반환 (parallel 은 동시 제출)."""
    import asyncio
    name = tool_fq.split(".", 1)[1] if "." in tool_fq else tool_fq
    return asyncio.run_coroutine_threadsafe(
        _mcp_session.call_tool(name, args or {}), _mcp_loop)


def mcp_result_text(fut) -> str:
    res = fut.result(timeout=60)
    for c in res.content:
        text = getattr(c, "text", None)
        if text is not None:
            return text
    return ""


def render_tool_block() -> tuple:
    """C++ McpCatalog::render_for_router_prompt / routing_hints_text 미러."""
    if not MCP_TOOLS:
        return "## Available MCP tools\n", ""
    s = "## Available MCP tools\n\n### " + MCP_ENTRY.get("name", "demo")
    tags = MCP_ENTRY.get("tags") or []
    if tags:
        s += "  [" + ", ".join(tags) + "]"
    if MCP_ENTRY.get("skip_synthesis_hint"):
        s += " (skip_synthesis ok)"
    s += "\n"
    if MCP_ENTRY.get("description"):
        s += MCP_ENTRY["description"] + "\n"
    for fq, desc in MCP_TOOLS.items():
        s += f"  - {fq}" + (f": {desc}" if desc else "") + "\n"
    hints = ""
    h = MCP_ENTRY.get("_hints") or {}
    for key in ("direct_when", "parallel_when", "delegate_when"):
        arr = h.get(key)
        if isinstance(arr, list) and arr:
            hints += key + ":\n" + "".join(f"  - {x}\n" for x in arr)
    if hints:
        s += "\n## Routing hints\n\n" + hints
    return s, hints


# ─────────────────────────────────────────────────────────────────────────────
# 그래프 상태 — jarvis_graph.json 의 채널 미러 (tool_results 만 append)
# ─────────────────────────────────────────────────────────────────────────────

class State(TypedDict, total=False):
    voice_in: dict
    text_in: str
    user_text: str
    user_lang: str
    memory_context: dict
    route_decision: dict
    tool_results: Annotated[list, operator.add]
    delegated_reply: str
    final_text: str
    tts_audio: dict


def detect_lang(text: str) -> str:
    # C++ detect_language_heuristic 동일: 한글 음절(U+AC00–D7A3) 있으면 ko
    return "ko" if any("가" <= ch <= "힣" for ch in text) else "en"


# ── 노드들 ───────────────────────────────────────────────────────────────────

def mic_capture(state: State) -> dict:
    # C++ MicInputNode 텍스트 모드와 동일 — 입력을 voice_in 패킷으로 유지
    return {}


def stt(state: State) -> dict:
    packet = state.get("voice_in") or {}
    text = packet.get("mock_text", "") if isinstance(packet, dict) else ""
    return {"user_text": text, "user_lang": detect_lang(text)}


def text_or_voice(state: State) -> dict:
    # channel_merge: user_text 우선, 없으면 text_in
    for v in (state.get("user_text"), state.get("text_in")):
        if v:
            return {"user_text": v}
    return {"user_text": ""}


def memory_lookup(state: State) -> dict:
    turns = store_get("turns") or []
    picked = []
    for t in reversed(turns):
        if isinstance(t, dict) and t.get("repeat_flag"):
            continue
        picked.append(t)
        if len(picked) >= RECENT_TURNS:
            break
    picked.reverse()
    prefs = store_get("prefs") or {}
    last_topic = store_get("last_topic") or ""
    return {"memory_context": {"recent_turns": picked, "prefs": prefs,
                               "last_topic": last_topic}}


def router(state: State) -> dict:
    user_text = state.get("user_text", "")
    if not user_text:
        return {"route_decision": {"mode": "direct", "tool_calls": [],
                                   "delegate_to": None,
                                   "skip_synthesis": True,
                                   "reasoning_short": "empty turn — no-op"}}

    section = load_prompt_section(PERSONA_FILE, "router")
    tool_block, hints = render_tool_block()
    agent_block = ("## Available specialist agents (A2A delegation)\n\n"
                   "(등록된 전문가 에이전트 없음)\n")
    system = (section + "\n\n=== Available Tools (MCP) ===\n" + tool_block
              + "\n\n=== Specialist Agents (A2A) ===\n" + agent_block
              + "\n\n"
              + (f"=== Routing Hints ===\n{hints}\n\n" if hints else "")
              + "IMPORTANT: respond with a single JSON object only. "
                "No markdown, no prose, no code fence.")
    user = (f"User said: {user_text}\nLanguage: {state.get('user_lang', 'en')}"
            f"\nMemory: {json.dumps(state.get('memory_context', {}), ensure_ascii=False, sort_keys=True, separators=(',', ':'))}")

    raw = call_router(system, user)

    # safe_parse_or_fallback 포팅
    decision = None
    text = raw.strip()
    if text.startswith("```"):
        text = text.split("\n", 1)[-1]
        if "```" in text:
            text = text[: text.rfind("```")]
        text = text.strip()
    try:
        parsed = json.loads(text)
        if (isinstance(parsed, dict) and isinstance(parsed.get("mode"), str)
                and parsed["mode"] in ("chat", "direct", "delegate", "parallel")):
            parsed.setdefault("tool_calls", [])
            parsed.setdefault("delegate_to", None)
            parsed.setdefault("skip_synthesis", False)
            decision = parsed
    except ValueError:
        pass
    if decision is None:
        decision = {"mode": "chat", "tool_calls": [], "delegate_to": None,
                    "skip_synthesis": False,
                    "reasoning_short": "router parse failed, fallback to chat"}

    # 결정 검증 — C++ 과 동일: 실재하지 않는 대상은 chat 강등
    mode = decision["mode"]
    if mode == "delegate":
        decision["mode"] = "chat"                     # 벤치: 레지스트리 비어있음
    elif mode in ("direct", "parallel"):
        valid = []
        for c in decision.get("tool_calls") or []:
            if isinstance(c, dict) and c.get("tool") in MCP_TOOLS:
                valid.append(c)
                if mode == "direct":
                    break                             # direct 는 정확히 1개
        decision["tool_calls"] = valid
        if not valid:
            decision["mode"] = "chat"
    if decision["mode"] == "chat":
        decision["tool_calls"] = []
        decision["delegate_to"] = None
        decision["skip_synthesis"] = False
    # skip_synthesis 강제 규칙 — 카탈로그 hint=false 도구는 합성 필수 (C++ 동일)
    if decision.get("skip_synthesis") and decision.get("tool_calls") \
            and not MCP_ENTRY.get("skip_synthesis_hint", False):
        decision["skip_synthesis"] = False
    if os.environ.get("JARVIS_DEBUG"):
        print("[twin][debug] parsed=" + json.dumps(decision, ensure_ascii=False),
              file=sys.stderr)
    return {"route_decision": decision}


def direct_branch(state: State) -> dict:
    rd = state.get("route_decision", {})
    calls = rd.get("tool_calls") or []
    if not calls:
        if state.get("tool_results"):
            return {}
        return {"tool_results": [{"text": state.get("user_text", "")}]}
    call = calls[0]
    try:
        result = mcp_result_text(
            mcp_call_async(call.get("tool", ""), call.get("args") or {}))
    except Exception as ex:                          # noqa: BLE001
        result = {"error": str(ex)}
    return {"tool_results": [{"tool": call.get("tool", ""),
                              "result": result}]}


def parallel_branch(state: State) -> dict:
    rd = state.get("route_decision", {})
    calls = rd.get("tool_calls") or []
    if not calls:
        return {"tool_results": []}
    # 동시 제출 후 순서대로 수확 — C++ make_parallel_group 동일 의미
    futs = [(c, mcp_call_async(c.get("tool", ""), c.get("args") or {}))
            for c in calls]
    results = []
    for c, fut in futs:
        try:
            results.append({"tool": c.get("tool", ""),
                            "result": mcp_result_text(fut)})
        except Exception as ex:                      # noqa: BLE001
            results.append({"tool": c.get("tool", ""),
                            "result": {"error": str(ex)}})
    return {"tool_results": results}


def delegate_branch(state: State) -> dict:
    rd = state.get("route_decision", {})
    agent = rd.get("delegate_to") or ""
    return {"delegated_reply":
            f"[mock-delegate] 에이전트 '{agent}' 를 찾을 수 없음. "
            f"입력: {state.get('user_text', '')}"}


def response_synth(state: State) -> dict:
    user_text = state.get("user_text", "")
    user_lang = state.get("user_lang", "en")

    sys_p = load_prompt_section(PERSONA_FILE, "synth")
    if not sys_p:
        sys_p = ("You are JARVIS — Tony Stark's terse, witty AI butler. "
                 "One or two sentences max. No markdown, no JSON, "
                 "plain speech suitable for text-to-speech.")
    sys_p += (f"\n\nReply in the user's language code ({user_lang}). "
              "Spell numbers, dates and times naturally in that language.")

    history = []
    last_ts = 0
    for t in (state.get("memory_context") or {}).get("recent_turns", []):
        if not isinstance(t, dict):
            continue
        history.append((t.get("user_text", ""), t.get("final_text", "")))
        if isinstance(t.get("ts"), int):
            last_ts = t["ts"]
    if last_ts > 0:
        gap_min = (int(time.time()) - last_ts) // 60
        if gap_min >= 15:
            sys_p += (f"\n(Note: the conversation history above your current "
                      f"turn ended about {gap_min} minutes ago — treat it as "
                      "an earlier session, not something the user said just "
                      "now.)")

    messages = [("system", sys_p)]
    for u, a in history:
        if u:
            messages.append(("user", u))
        if a:
            messages.append(("assistant", a))

    usr = user_text
    tr = state.get("tool_results") or []
    if tr:
        usr += ("\n\n[Tool result JSON — use the key fact]: "
                + json.dumps(tr, ensure_ascii=False))
    if state.get("delegated_reply"):
        usr += ("\n\n[Specialist reply — speak it as your own]: "
                + state["delegated_reply"])
    messages.append(("user", usr))

    final_text = call_synth(messages)

    # 복창 가드 — C++ 과 동일: 과거 답변과 verbatim 일치 시 1회 재생성
    trimmed = final_text.strip()
    if trimmed and any(a.strip() == trimmed for _, a in history if a):
        print("[synth] 복창 감지 — 과거 답변과 verbatim 일치, 1회 재생성",
              file=sys.stderr)
        messages.append(("system",
                         "Your draft repeated one of your earlier replies "
                         "word-for-word. Compose a fresh answer to the user's "
                         "CURRENT message, in their language."))
        retry = call_synth(messages)
        if retry.strip():
            final_text = retry
    return {"final_text": final_text}


def synth_skip(state: State) -> dict:
    tr = state.get("tool_results") or []
    elem = tr[-1] if tr else ""
    if isinstance(elem, dict):
        if isinstance(elem.get("text"), str):
            elem = elem["text"]
        elif "result" in elem:
            res = elem["result"]
            elem = res if isinstance(res, str) else json.dumps(res,
                                                               ensure_ascii=False)
    return {"final_text": elem if isinstance(elem, str) else ""}


def memory_commit(state: State) -> dict:
    user_text = state.get("user_text", "")
    final_text = state.get("final_text", "") or ""
    tools = [t.get("tool") for t in (state.get("tool_results") or [])
             if isinstance(t, dict) and isinstance(t.get("tool"), str)]
    turns = store_get("turns") or []
    new_turn = {"user_text": user_text, "lang": state.get("user_lang", ""),
                "final_text": final_text, "tools_used": tools,
                "ts": int(time.time())}
    ft = final_text.strip()
    if ft:
        for t in turns:
            if (isinstance(t, dict)
                    and t.get("final_text", "").strip() == ft
                    and t.get("user_text", "").strip() != user_text.strip()):
                new_turn["repeat_flag"] = True
                break
    turns.append(new_turn)
    store_put("turns", turns[-MAX_KEEP:])
    return {}


def tts(state: State) -> dict:
    final_text = state.get("final_text", "") or ""
    lang = state.get("user_lang", "en")
    print(f"[jarvis:tts][{lang}] {final_text}", flush=True)
    return {"tts_audio": {"played_at": int(time.time())}}


# ─────────────────────────────────────────────────────────────────────────────
# 그래프 배선 — jarvis_graph.json edges 미러
# ─────────────────────────────────────────────────────────────────────────────

def route_by_mode(state: State) -> str:
    m = (state.get("route_decision") or {}).get("mode")
    return m if m in ("chat", "direct", "delegate", "parallel") else "chat"


def route_by_skip_synth(state: State) -> str:
    return "true" if (state.get("route_decision") or {}).get(
        "skip_synthesis") else "false"


g = StateGraph(State)
g.add_node("mic_capture", mic_capture)
g.add_node("stt", stt)
g.add_node("text_or_voice", text_or_voice)
g.add_node("memory_lookup", memory_lookup)
g.add_node("router", router)
g.add_node("direct_branch", direct_branch)
g.add_node("parallel_branch", parallel_branch)
g.add_node("delegate_branch", delegate_branch)
g.add_node("response_synth", response_synth)
g.add_node("synth_skip", synth_skip)
g.add_node("memory_commit", memory_commit)
g.add_node("tts", tts)

g.add_edge(START, "mic_capture")
g.add_edge("mic_capture", "stt")
g.add_edge("stt", "text_or_voice")
g.add_edge("text_or_voice", "memory_lookup")
g.add_edge("memory_lookup", "router")
g.add_conditional_edges("router", route_by_mode,
                        {"chat": "response_synth",
                         "direct": "direct_branch",
                         "parallel": "parallel_branch",
                         "delegate": "delegate_branch"})
g.add_conditional_edges("direct_branch", route_by_skip_synth,
                        {"true": "synth_skip", "false": "response_synth"})
g.add_edge("parallel_branch", "response_synth")
g.add_edge("delegate_branch", "response_synth")
g.add_edge("synth_skip", "memory_commit")
g.add_edge("response_synth", "memory_commit")
g.add_edge("memory_commit", "tts")
g.add_edge("tts", END)

app = g.compile()


def main() -> None:
    print("[jarvis] 온라인. 텍스트를 입력하거나 ^C 로 종료.", flush=True)
    for line in sys.stdin:
        line = line.rstrip("\n")
        if not line.strip():
            continue
        app.invoke({"voice_in": {"mock_text": line, "pcm": [],
                                 "sample_rate": 0}})
    print("[jarvis] 종료 중...", flush=True)


if __name__ == "__main__":
    main()
