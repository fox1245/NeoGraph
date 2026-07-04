// jarvis/src/orchestrator/intent_router_node.cpp
//
// IntentRouterNode 구현 — 자비스의 두뇌.
// 사용자 발화 + 메모리 컨텍스트를 받아 작은 LLM 한 번 호출로
// route_decision 채널에 쓸 JSON 결정을 만든다.
//
// 출력 JSON 형태:
//   {
//     "mode": "direct" | "delegate" | "parallel",
//     "tool_calls": [ { "tool": "<name>", "args": {...} } ],
//     "delegate_to": "<agent_name>" | null,
//     "skip_synthesis": bool,
//     "reasoning_short": "<1문장 영어 로그>"
//   }

#include "intent_router_node.h"
#include "mcp_catalog.h"
#include "agent_dispatcher.h"

#include <neograph/neograph.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace jarvis::orchestrator {

// ─────────────────────────────────────────────────────────────────────────────
// 생성자
// ─────────────────────────────────────────────────────────────────────────────

IntentRouterNode::IntentRouterNode(std::string             name,
                                   const neograph::json&   cfg,
                                   const McpCatalog&       catalog,
                                   const AgentDispatcher&  dispatcher,
                                   std::shared_ptr<neograph::Provider> provider)
    : name_(std::move(name))
    , catalog_(catalog)
    , dispatcher_(dispatcher)
    , provider_(std::move(provider))
{
    // cfg 에서 설정값 추출 — 없으면 안전한 기본값 사용
    model_          = cfg.value("model",          "gpt-4o-mini");
    output_channel_ = cfg.value("output_channel", "route_decision");
    prompt_file_    = cfg.value("prompt_file",    "config/persona.txt");
    prompt_section_ = cfg.value("prompt_section", "router");
}

// ─────────────────────────────────────────────────────────────────────────────
// compose_system_prompt()
// persona.txt 의 ===<section>=== 섹션 추출 + 도구/에이전트 목록 붙이기
// ─────────────────────────────────────────────────────────────────────────────

std::string IntentRouterNode::compose_system_prompt() const
{
    // ① persona.txt 전체 읽기
    std::ifstream file(prompt_file_);
    if (!file.is_open()) {
        // 파일 없어도 그래프는 죽이지 않는다 — 빈 문자열로 계속
        // (LLM 이 schema-only 로 동작할 수 있도록)
    }

    std::string section_text;
    if (file.is_open()) {
        // ② ===<section>=== 구분자 기준으로 원하는 섹션 추출
        // 규칙: 라인 처음부터 ===<name>=== 이 단독으로 있는 줄이 섹션 시작.
        // 다음 ===...=== 줄이 나오거나 EOF 가 되면 섹션 끝.
        const std::string target_header = "===" + prompt_section_ + "===";

        std::string line;
        bool in_section = false;
        std::ostringstream buf;

        while (std::getline(file, line)) {
            // 빈 줄 포함 — 내용에 포함 여부는 섹션 상태로 판단
            bool is_section_header = (line.size() >= 6 &&
                                      line.front() == '=' &&
                                      line.back()  == '=');

            if (is_section_header) {
                if (in_section) {
                    // 다른 섹션 시작 → 현재 섹션 끝
                    break;
                }
                if (line == target_header) {
                    in_section = true;
                }
                // 다른 섹션 헤더는 그냥 넘김
                continue;
            }

            if (in_section) {
                buf << line << '\n';
            }
        }

        section_text = buf.str();
        // 끝의 불필요한 개행 정리
        while (!section_text.empty() && section_text.back() == '\n') {
            section_text.pop_back();
        }
    }

    // ③ MCP 도구 목록 추가
    std::string tool_block = catalog_.render_for_router_prompt();

    // ④ 전문가 에이전트 목록 추가
    std::string agent_block = dispatcher_.render_for_router_prompt();

    // ⑤ routing_hints 추가 (있으면)
    std::string hints = catalog_.routing_hints_text();

    // ⑥ 전체 시스템 프롬프트 합치기
    std::ostringstream out;

    if (!section_text.empty()) {
        out << section_text << "\n\n";
    }

    if (!tool_block.empty()) {
        out << "=== Available Tools (MCP) ===\n"
            << tool_block << "\n\n";
    }

    if (!agent_block.empty()) {
        out << "=== Specialist Agents (A2A) ===\n"
            << agent_block << "\n\n";
    }

    if (!hints.empty()) {
        out << "=== Routing Hints ===\n"
            << hints << "\n\n";
    }

    // JSON 만 내보내라는 강조 (persona.txt 에도 있지만 여기서 한 번 더)
    out << "IMPORTANT: respond with a single JSON object only. "
           "No markdown, no prose, no code fence.";

    return out.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// safe_parse_or_fallback()
// LLM 응답 텍스트를 JSON 으로 파싱. 실패하거나 필수 필드 없으면 안전 폴백 반환.
// ─────────────────────────────────────────────────────────────────────────────

neograph::json IntentRouterNode::safe_parse_or_fallback(const std::string& raw) const
{
    // 안전 폴백 JSON — 그래프가 직접 실행으로 흘러서 빈 합성 단계로 이어짐
    auto fallback = []() -> neograph::json {
        return {
            {"mode",             "direct"},
            {"tool_calls",       neograph::json::array()},
            {"delegate_to",      nullptr},
            {"skip_synthesis",   true},
            {"reasoning_short",  "router parse failed, fallback"}
        };
    };

    if (raw.empty()) return fallback();

    // ① 마크다운 코드 펜스 제거 (```json ... ``` 또는 ``` ... ```)
    std::string text = raw;
    {
        // 앞뒤 공백 제거
        auto ltrim = text.find_first_not_of(" \t\r\n");
        if (ltrim != std::string::npos) text = text.substr(ltrim);
        auto rtrim = text.find_last_not_of(" \t\r\n");
        if (rtrim != std::string::npos) text = text.substr(0, rtrim + 1);

        // ```json 또는 ``` 로 시작하면 첫 줄과 마지막 ``` 제거
        if (text.size() >= 3 && text.substr(0, 3) == "```") {
            auto first_newline = text.find('\n');
            if (first_newline != std::string::npos) {
                text = text.substr(first_newline + 1);
            }
            // 끝의 ``` 제거
            auto last_fence = text.rfind("```");
            if (last_fence != std::string::npos) {
                text = text.substr(0, last_fence);
            }
            // 다시 앞뒤 공백 정리
            ltrim = text.find_first_not_of(" \t\r\n");
            if (ltrim != std::string::npos) text = text.substr(ltrim);
            rtrim = text.find_last_not_of(" \t\r\n");
            if (rtrim != std::string::npos) text = text.substr(0, rtrim + 1);
        }
    }

    // ② JSON 파싱 시도
    neograph::json parsed;
    try {
        parsed = neograph::json::parse(text);
    } catch (...) {
        return fallback();
    }

    // ③ 필수 필드 "mode" 확인
    if (!parsed.contains("mode") || !parsed["mode"].is_string()) {
        return fallback();
    }

    // ④ mode 값이 허용된 셋 안에 있는지 확인
    const std::string mode = parsed["mode"].get<std::string>();
    if (mode != "direct" && mode != "delegate" && mode != "parallel") {
        return fallback();
    }

    // ⑤ 누락된 선택 필드는 기본값으로 채워서 반환 (견고성)
    if (!parsed.contains("tool_calls") || !parsed["tool_calls"].is_array()) {
        parsed["tool_calls"] = neograph::json::array();
    }
    if (!parsed.contains("delegate_to")) {
        parsed["delegate_to"] = nullptr;
    }
    if (!parsed.contains("skip_synthesis") || !parsed["skip_synthesis"].is_boolean()) {
        parsed["skip_synthesis"] = false;
    }
    if (!parsed.contains("reasoning_short")) {
        parsed["reasoning_short"] = "";
    }

    return parsed;
}

// ─────────────────────────────────────────────────────────────────────────────
// run() — 코루틴 진입점. NodeInput 받아 route_decision 채널에 결정을 쓴다.
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
IntentRouterNode::run(neograph::graph::NodeInput in)
{
    // ① 상태에서 입력값 읽기
    // state.get() 은 neograph::json 반환. null 이거나 문자열이 아니면 기본값 사용.
    const auto& state = in.state;

    auto json_to_str = [](const neograph::json& j,
                          const std::string& fallback) -> std::string {
        if (j.is_string()) return j.get<std::string>();
        return fallback;
    };

    const std::string user_text = json_to_str(state.get("user_text"), "");
    const std::string user_lang = json_to_str(state.get("user_lang"), "en");

    // memory_context 는 MemoryLookupNode 가 객체({recent_turns, prefs,
    // last_topic})로 쓴다 — 문자열만 받으면 항상 "" 가 되어 라우터가
    // 이전 대화를 전혀 못 보는 버그가 있었음. 객체면 dump 로 직렬화.
    std::string memory_context;
    {
        const auto& mc = state.get("memory_context");
        if (mc.is_string())     memory_context = mc.get<std::string>();
        else if (!mc.is_null()) memory_context = mc.dump();
    }

    // ①.5 빈 턴 가드 — EOF 직전 셧다운 사이클이나 빈 입력은 LLM 호출 없이
    //     no-op 라우팅으로 종료. 이게 없으면 종료할 때마다 유령 턴이
    //     라우터+합성+TTS 한 바퀴를 돌며 "your message was empty" 를 말한다.
    //     (tool_dispatch 가 빈 user_text 를 echo → final_text "" → TTS 는
    //      빈 텍스트를 건너뛰므로 이 경로는 완전히 무음이다.)
    {
        const auto& sd = state.get("__shutdown__");
        const bool shutting_down = sd.is_boolean() && sd.get<bool>();
        if (shutting_down || user_text.empty()) {
            neograph::json noop = {
                {"mode",            "direct"},
                {"tool_calls",      neograph::json::array()},
                {"delegate_to",     nullptr},
                {"skip_synthesis",  true},
                {"reasoning_short", "empty turn — no-op"}
            };
            neograph::graph::NodeOutput out;
            out.writes.push_back({output_channel_, noop});
            co_return out;
        }
    }

    // ② 시스템 프롬프트 합성 (파일 I/O + 도구 목록 렌더링)
    // 헤더에 캐시 멤버가 없으므로 매 호출마다 생성.
    // 도구 목록이 런타임에 바뀔 수 있어 오히려 바람직함.
    const std::string system_prompt = compose_system_prompt();
    // [DEBUG] 라우터 시스템 프롬프트 첫 800자 + 총 길이 stderr 로 — LLM 이 도구 카탈로그를
    //         실제로 받고 있는지 확인용. 정상 동작 검증되면 제거 또는 env 가드 뒤로.
    std::cerr << "[router][debug] sys_prompt(" << system_prompt.size()
              << " bytes) head=\n--------\n"
              << system_prompt.substr(0, 800)
              << (system_prompt.size() > 800 ? "\n...[truncated]" : "")
              << "\n--------\n";

    // ③ 사용자 메시지 구성
    std::string user_msg =
        "User said: " + user_text +
        "\nLanguage: " + user_lang +
        "\nMemory: " + memory_context;

    // ④ CompletionParams 구성
    neograph::CompletionParams params;
    params.model       = model_;
    params.temperature = 0.1f;   // 결정론적 분류 — 낮게
    params.max_tokens  = 300;    // 라우팅 JSON 은 작다
    neograph::ChatMessage sys_msg;
    sys_msg.role    = "system";
    sys_msg.content = system_prompt;

    neograph::ChatMessage usr_msg;
    usr_msg.role    = "user";
    usr_msg.content = user_msg;

    params.messages = { sys_msg, usr_msg };

    // ⑤ LLM 호출 (invoke — v0.4 권장 단일 진입점, 스트리밍 불필요)
    neograph::ChatCompletion completion =
        co_await provider_->invoke(params, nullptr);

    // ⑥ 응답 텍스트 추출 → JSON 검증
    const std::string raw_content = completion.message.content;
    // [DEBUG] 시연 진단용 — Fix C: 라우터 LLM 호출 결과 + 파싱 후 결정을 stderr 로 노출.
    //         정상 동작 확인된 뒤 제거 권장.
    std::cerr << "[router][debug] raw="
              << raw_content.substr(0, 240) << "\n";
    neograph::json decision = safe_parse_or_fallback(raw_content);

    // skip_synthesis 강제 규칙 — 카탈로그가 skip_synthesis_hint=false 로
    // 선언한 도구는 LLM 결정과 무관하게 합성을 거친다. raw 도구 출력
    // (ISO 날짜/숫자 나열)이 그대로 TTS 로 가면 비발화 문자열이 읽히는
    // 문제 방지. 힌트는 프롬프트에도 들어가지만 LLM 이 무시할 수 있어
    // 여기서 결정론적으로 덮어쓴다.
    if (decision.value("skip_synthesis", false) &&
        decision.contains("tool_calls") && decision["tool_calls"].is_array()) {
        for (const auto& call : decision["tool_calls"]) {
            const std::string t = call.value("tool", "");
            if (!t.empty() && !catalog_.allows_skip_synthesis(t)) {
                decision["skip_synthesis"] = false;
                std::cerr << "[router] skip_synthesis 강제 해제 — '" << t
                          << "' 은 카탈로그 hint=false\n";
                break;
            }
        }
    }

    // 앵무새 방지 — mode=direct 인데 도구 0개면 합성만이 답을 만들 수 있다.
    // 이때 skip_synthesis=true 로 두면 tool_dispatch 의 user_text echo 폴백이
    // 발화되어 자비스가 사용자 말을 그대로 따라 하게 된다. 파서 fallback 은
    // 예외 — salvage 가 채운 텍스트를 그대로 읽는 것이 의도된 동작.
    if (decision.value("skip_synthesis", false) &&
        decision.value("mode", "") == "direct" &&
        (!decision.contains("tool_calls") || decision["tool_calls"].empty()) &&
        decision.value("reasoning_short", "").find("fallback") == std::string::npos) {
        decision["skip_synthesis"] = false;
        std::cerr << "[router] skip_synthesis 강제 해제 — 도구 0개 direct 는 "
                     "합성 경로 필수\n";
    }
    std::cerr << "[router][debug] parsed=" << decision.dump() << "\n";

    // ⑦ NodeOutput 에 결과 쓰기
    neograph::graph::NodeOutput out;
    out.writes.push_back({output_channel_, decision});

    // ⑧ Fix E — 라우터 LLM 이 분류 임무를 무시하고 자연어 응답으로 답해버렸을 때
    //   (mode 필드 없음 → 안전 폴백) raw 응답에서 의미 있는 텍스트를 뽑아
    //   tool_results 채널에 직접 push. synth_skip(passthrough) 가 그 텍스트를
    //   final_text 로 가져가서 TTS 가 정상적으로 읽도록.
    if (decision.contains("reasoning_short") &&
        decision["reasoning_short"].is_string() &&
        decision["reasoning_short"].get<std::string>().find("fallback") != std::string::npos)
    {
        std::string salvage;
        try {
            auto j = neograph::json::parse(raw_content);
            // 흔히 LLM 이 자기 답을 박는 키 후보들
            for (const char* k : {"response", "answer", "content", "text", "message"}) {
                if (j.contains(k) && j[k].is_string()) {
                    salvage = j[k].get<std::string>();
                    break;
                }
            }
            // JSON 이긴 한데 위 키들이 없으면 통째 dump
            if (salvage.empty()) salvage = j.dump();
        } catch (...) {
            // raw 가 JSON 이 아니면 그 자체가 자연어 — 그대로 사용
            salvage = raw_content;
        }
        if (!salvage.empty()) {
            std::cerr << "[router][debug] salvaged " << salvage.size()
                      << " chars from fallback raw → tool_results\n";
            neograph::json one_result;
            one_result["text"] = salvage;
            neograph::json arr_payload = neograph::json::array();
            arr_payload.push_back(one_result);
            out.writes.push_back({"tool_results", arr_payload});
        }
    }

    co_return out;
}

}  // namespace jarvis::orchestrator
