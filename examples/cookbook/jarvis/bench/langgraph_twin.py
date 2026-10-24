#!/usr/bin/env python3
"""LangGraph twin of the JARVIS orchestration core — for framework overhead benchmarks.

Mirrors the **exact topology** of NeoGraph cookbook_jarvis (mock build, text mode)
in LangGraph. Node layout, router/synthesizer prompts, decision validation (chat
demotion), memory storage format (JsonFileStore), repetition guard, and stdout
markers are all identical — only the graph framework and language differ.

  mic_capture → stt(Hangul heuristic language detection) → text_or_voice → memory_lookup
  → router(LLM) → [chat|direct|parallel|delegate] → response_synth(LLM)/synth_skip
  → memory_commit → tts(console marker)

Modes (env BENCH_MODE):
  mock — 0ms LLM stub (same responses as C++ MockProvider) → pure framework overhead
  api  — DeepSeek API via OpenRouter (OPENROUTER_API_KEY) → live network path


Protocol: one stdin line = one turn. Prints the "[jarvis]" Korean "online" ready banner (see main()) and
"[jarvis:tts][<lang>] <final_text>" when a turn completes (same markers as C++).
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
# Persona section loader — same rules as C++ load_prompt_section
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
# JsonFileStore-compatible file store — same [{ns, key, value}] format
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


# LLM adapter — mock (0ms stub, same as C++ MockProvider) / api (OpenRouter)

MOCK_ROUTER_JSON = ('{"mode":"direct","tool_calls":[],"delegate_to":null,'
                    '"skip_synthesis":true,"reasoning_short":"mock router"}')

if BENCH_MODE == "api":
    from langchain_openai import ChatOpenAI

    OPENROUTER_MODEL = "~deepseek/deepseek-v4-flash-latest"

    # Same request body as the C++ side (provider_support.h): the output cap is
    # shared by hidden reasoning and the visible reply, so both add the same
    # 1024-token reasoning allowance and ask for low reasoning effort.
    REASONING_ALLOWANCE = 1024

    def _mk(temp: float, visible_tok: int):
        api_key = os.environ.get("OPENROUTER_API_KEY")
        if not api_key:
            raise RuntimeError("OPENROUTER_API_KEY is required when BENCH_MODE=api")
        return ChatOpenAI(
            model=OPENROUTER_MODEL,
            api_key=api_key,
            base_url=os.environ.get(
                "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1"),
            temperature=temp,
            max_tokens=visible_tok + REASONING_ALLOWANCE,
            timeout=120,
            extra_body={"provider": {"zdr": True}, "reasoning_effort": "low"},
        )

    ROUTER_LLM = _mk(0.1, 300)
    SYNTH_LLM = _mk(0.4, 220)

    def call_router(system: str, user: str) -> str:
        return ROUTER_LLM.invoke([("system", system), ("human", user)]).content

    def call_synth(messages: list) -> str:
        # Streaming — emit [jarvis:ttft] marker on the first chunk, matching the C++ synthesizer.
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
        # C++ MockProvider: return mock routing JSON for router calls
        return MOCK_ROUTER_JSON

    def call_synth(messages: list) -> str:
        # C++ MockProvider: echo the last user message. Simulates streaming — immediate marker.
        print("[jarvis:ttft]", flush=True)
        user_text = ""
        for role, content in messages:
            if role == "user":
                user_text = content
        return "[mock] " + user_text


# ─────────────────────────────────────────────────────────────────────────────
# MCP client (E2E bench) — persistent session via the official mcp SDK when MCP_URL is set.
# Same as C++ McpCatalog: collects the tool list at startup → injects into the router prompt;
# direct/parallel branches invoke live tools. Catalog metadata (name/description/tags/hints)
# comes from JARVIS_MCP_CATALOG (JSON) — the same file read by C++.
# ─────────────────────────────────────────────────────────────────────────────

MCP_URL = os.environ.get("MCP_URL", "")
MCP_TOOLS: dict = {}      # "demo.get_weather" -> description
MCP_ENTRY: dict = {}      # catalog entry (name/description/tags/hints)
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
                await asyncio.Event().wait()      # keep alive for the lifetime of the process

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
    """Submit a tool call to the event loop and return a Future (submitted concurrently in parallel mode)."""
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
    """Mirror of C++ McpCatalog::render_for_router_prompt / routing_hints_text."""
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
# Graph state — mirrors channels in jarvis_graph.json (only tool_results uses append)
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
    # Same as C++ detect_language_heuristic: returns "ko" if Hangul syllables (U+AC00–D7A3) are present
    return "ko" if any("가" <= ch <= "힣" for ch in text) else "en"


# ── Nodes ────────────────────────────────────────────────────────────────────

def mic_capture(state: State) -> dict:
    # Same as C++ MicInputNode text mode — keeps input as a voice_in packet
    return {}


def stt(state: State) -> dict:
    packet = state.get("voice_in") or {}
    text = packet.get("mock_text", "") if isinstance(packet, dict) else ""
    return {"user_text": text, "user_lang": detect_lang(text)}


def text_or_voice(state: State) -> dict:
    # channel_merge: prefer user_text, fall back to text_in
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

    # Port of safe_parse_or_fallback
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

    # Decision validation — same as C++: demote non-existent targets to chat
    mode = decision["mode"]
    if mode == "delegate":
        decision["mode"] = "chat"                     # bench: registry is empty
    elif mode in ("direct", "parallel"):
        valid = []
        for c in decision.get("tool_calls") or []:
            if isinstance(c, dict) and c.get("tool") in MCP_TOOLS:
                valid.append(c)
                if mode == "direct":
                    break                             # direct takes at most 1 call
        decision["tool_calls"] = valid
        if not valid:
            decision["mode"] = "chat"
    if decision["mode"] == "chat":
        decision["tool_calls"] = []
        decision["delegate_to"] = None
        decision["skip_synthesis"] = False
    # skip_synthesis enforcement rule — tools with catalog hint=false require synthesis (same as C++)
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
    # Submit concurrently and collect in order — same semantics as C++ make_parallel_group
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

    # Repetition guard — same as C++: regenerate once if the reply matches an earlier answer verbatim
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
# Graph wiring — mirrors edges in jarvis_graph.json
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
