// jarvis/src/memory/conversation_store.cpp
//
// MemoryLookupNode  — 매 턴 시작 시 Store 에서 최근 N 턴 + 사용자 선호 +
//   마지막 주제를 꺼내 memory_context 채널에 push 한다.
//
// MemoryCommitNode  — 매 턴 끝에 user_text + final_text + 사용된 도구 이름들을
//   Store turns 리스트에 append 한다.
//
// Store 네임스페이스 구조 (기본 "jarvis.tony"):
//   <ns>.turns     — 대화 턴 append-only 리스트 (json 배열을 통째로 저장)
//   <ns>.prefs     — 사용자 선호 키-값 객체
//   <ns>.last_topic — 짧은 주제 요약 문자열

#include "conversation_store.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace jarvis::memory {

// ─────────────────────────────────────────────────────────────────────────────
// 내부 헬퍼 — 네임스페이스 문자열 → Store Namespace(벡터) 변환
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// "jarvis.tony" 같은 점으로 구분된 문자열을 {"jarvis", "tony"} 벡터로 쪼갠다.
/// Store 의 계층 Namespace 는 std::vector<std::string> 이므로 이렇게 변환이 필요.
neograph::graph::Namespace split_ns(const std::string& ns_str) {
    neograph::graph::Namespace parts;
    std::string                buf;
    for (char c : ns_str) {
        if (c == '.') {
            if (!buf.empty()) { parts.push_back(buf); buf.clear(); }
        } else {
            buf += c;
        }
    }
    if (!buf.empty()) parts.push_back(buf);
    return parts;
}

/// 현재 시각을 Unix epoch 초 단위로 반환한다.
int64_t now_epoch_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

/// 앞뒤 공백 제거 — 복창(verbatim) 비교용.
std::string trim_ws(const std::string& s) {
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

/// tool_results 채널 값에서 도구 이름 목록을 추출한다.
/// tool_results 는 array([{"tool":"x",...},...]) 또는 object({"tool":"x",...}) 형태
/// 둘 다 처리한다.
std::vector<std::string> extract_tool_names(const neograph::json& tool_results) {
    std::vector<std::string> names;
    if (tool_results.is_null()) return names;

    auto try_name = [&](const neograph::json& item) {
        // "tool", "tool_name", "name" 키 순서로 시도
        for (const char* key : {"tool", "tool_name", "name"}) {
            if (item.contains(key) && item[key].is_string()) {
                names.push_back(item[key].get<std::string>());
                return;
            }
        }
    };

    if (tool_results.is_array()) {
        for (const auto& item : tool_results) {
            if (item.is_object()) try_name(item);
        }
    } else if (tool_results.is_object()) {
        try_name(tool_results);
    }

    return names;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// MemoryLookupNode — 생성자
// ─────────────────────────────────────────────────────────────────────────────

MemoryLookupNode::MemoryLookupNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , namespace_(cfg.value("store_namespace", std::string("jarvis.tony")))
    , recent_turns_(cfg.value("recent_turns", 6))
{}

// ─────────────────────────────────────────────────────────────────────────────
// MemoryLookupNode::run  — Store 에서 기억 끌어오기
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
MemoryLookupNode::run(neograph::graph::NodeInput in) {
    neograph::graph::NodeOutput out;

    // __shutdown__ 신호가 켜진 빈 사이클이면 조용히 빈 컨텍스트 반환
    {
        const auto& sd = in.state.get("__shutdown__");
        if (sd.is_boolean() && sd.get<bool>()) {
            out.writes.push_back({
                "memory_context",
                neograph::json{
                    {"recent_turns", neograph::json::array()},
                    {"prefs",        neograph::json::object()},
                    {"last_topic",   ""}
                }
            });
            co_return out;
        }
    }

    // Store 가 연결되지 않았으면 (ctx.store == nullptr) 빈 memory_context 반환
    if (!in.ctx.store) {
        std::cerr << "[MemoryLookupNode] Store 미설정 — 빈 memory_context 반환\n";
        out.writes.push_back({
            "memory_context",
            neograph::json{
                {"recent_turns", neograph::json::array()},
                {"prefs",        neograph::json::object()},
                {"last_topic",   ""}
            }
        });
        co_return out;
    }

    auto& store     = *in.ctx.store;
    auto  base_ns   = split_ns(namespace_);

    // ── 1) 최근 N 턴 읽기 ─────────────────────────────────────────────────
    // turns 는 json 배열 전체를 단일 키 "list" 에 저장하는 방식 사용.
    // (Store 는 키 하나에 임의 json 저장 가능 — 43_store_personalization.cpp 참고)
    neograph::json recent_turns = neograph::json::array();
    {
        auto turns_ns  = base_ns;           // {"jarvis", "tony"}
        auto item      = store.get(turns_ns, "turns");   // key = "turns"
        if (item && item->value.is_array()) {
            const auto& all = item->value;
            // 뒤에서부터 N개 수집 — repeat_flag(복창 의심) 턴은 컨텍스트에서
            // 제외한다. 복창이 기억에 재주입되면 다음 턴의 앵커가 배가되는
            // 자기강화 루프가 생기므로, 오염 턴은 저장은 하되 회상은 막는다.
            std::vector<neograph::json> picked;
            for (int i = static_cast<int>(all.size()) - 1;
                 i >= 0 && static_cast<int>(picked.size()) < recent_turns_; --i) {
                const auto& t = all[i];
                if (t.is_object() && t.value("repeat_flag", false)) continue;
                picked.push_back(t);
            }
            for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
                recent_turns.push_back(*it);   // 다시 오래된 순으로
            }
        }
    }

    // ── 2) 사용자 선호(prefs) 읽기 ────────────────────────────────────────
    neograph::json prefs = neograph::json::object();
    {
        auto item = store.get(base_ns, "prefs");
        if (item && item->value.is_object()) {
            prefs = item->value;
        }
    }

    // ── 3) 마지막 주제 읽기 ───────────────────────────────────────────────
    std::string last_topic;
    {
        auto item = store.get(base_ns, "last_topic");
        if (item && item->value.is_string()) {
            last_topic = item->value.get<std::string>();
        }
    }

    // ── 4) memory_context 채널에 push ─────────────────────────────────────
    out.writes.push_back({
        "memory_context",
        neograph::json{
            {"recent_turns", recent_turns},
            {"prefs",        prefs},
            {"last_topic",   last_topic}
        }
    });

    co_return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// MemoryCommitNode — 생성자
// ─────────────────────────────────────────────────────────────────────────────

MemoryCommitNode::MemoryCommitNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , namespace_(cfg.value("store_namespace", std::string("jarvis.tony")))
{}

// ─────────────────────────────────────────────────────────────────────────────
// MemoryCommitNode::run  — 이번 턴을 Store turns 에 append
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
MemoryCommitNode::run(neograph::graph::NodeInput in) {
    neograph::graph::NodeOutput out;  // 부수 효과만 — 채널 쓰기 없음

    // __shutdown__ 신호가 켜진 빈 사이클이면 조용히 건너뜀
    {
        const auto& sd = in.state.get("__shutdown__");
        if (sd.is_boolean() && sd.get<bool>()) {
            co_return out;
        }
    }

    // Store 가 연결되지 않았으면 아무것도 안 함 (그래프 죽지 않게)
    if (!in.ctx.store) {
        std::cerr << "[MemoryCommitNode] Store 미설정 — 턴 저장 건너뜀\n";
        co_return out;
    }

    auto& store   = *in.ctx.store;
    auto  base_ns = split_ns(namespace_);

    // ── 상태 채널에서 이번 턴 데이터 읽기 ────────────────────────────────
    auto user_text_v  = in.state.get("user_text");
    auto user_lang_v  = in.state.get("user_lang");
    auto final_text_v = in.state.get("final_text");
    auto tool_res_v   = in.state.get("tool_results");

    std::string user_text  = user_text_v.is_string()
                             ? user_text_v.get<std::string>()  : "";
    std::string user_lang  = user_lang_v.is_string()
                             ? user_lang_v.get<std::string>()  : "";
    std::string final_text = final_text_v.is_string()
                             ? final_text_v.get<std::string>() : "";

    // 빈 턴 커밋 가드 — user_text 나 final_text 가 비었으면 저장 안 함.
    // (마이크 노이즈·STT 실패·도구 오류로 빈 응답이 나온 턴이 기억을 오염시켜
    //  "이전 대화를 기억 못함" 처럼 보이는 것을 막는다. append 리스트가 빈
    //  턴으로 차서 진짜 대화가 24개 상한 밖으로 밀려나던 문제.)
    {
        auto trim = [](const std::string& s) {
            auto b = s.find_first_not_of(" \t\r\n");
            return b == std::string::npos ? std::string() : s.substr(b);
        };
        if (trim(user_text).empty() || trim(final_text).empty()) {
            std::cerr << "[MemoryCommitNode] 빈 턴 — 저장 건너뜀 "
                         "(user_text 또는 final_text 비어있음)\n";
            co_return out;
        }
    }

    // 사용된 도구 이름 목록 추출
    auto tool_names = extract_tool_names(tool_res_v);
    neograph::json tools_json = neograph::json::array();
    for (const auto& t : tool_names) tools_json.push_back(t);

    // ── 새 턴 객체 만들기 ─────────────────────────────────────────────────
    neograph::json new_turn = {
        {"user_text",  user_text},
        {"lang",       user_lang},
        {"final_text", final_text},
        {"tools_used", tools_json},
        {"ts",         now_epoch_seconds()}
    };

    // ── 기존 turns 읽기 + 새 턴 추가 + 다시 쓰기 ────────────────────────
    // 리스트가 recent_turns(기본 6) * 4 = 24 를 초과하면 앞에서 잘라낸다.
    // 너무 오래된 턴은 보관 가치가 낮고 Store 크기도 관리해야 하므로.
    {
        neograph::json all_turns = neograph::json::array();

        auto existing = store.get(base_ns, "turns");
        if (existing && existing->value.is_array()) {
            all_turns = existing->value;
        }

        // 복창 의심 표기 — 다른 질문(user_text 상이)에 과거와 verbatim 동일한
        // 답이 커밋되면 repeat_flag=true. 저장은 하되 MemoryLookupNode 가
        // 회상에서 제외해 앵무새의 자기강화를 끊는다. (같은 질문에 같은 답은
        // 정당한 반복이므로 flag 하지 않는다.)
        {
            const std::string ft = trim_ws(final_text);
            if (!ft.empty()) {
                for (const auto& t : all_turns) {
                    if (!t.is_object()) continue;
                    if (trim_ws(t.value("final_text", std::string(""))) == ft &&
                        trim_ws(t.value("user_text",  std::string(""))) !=
                            trim_ws(user_text)) {
                        new_turn["repeat_flag"] = true;
                        std::cerr << "[MemoryCommitNode] 복창 의심 턴 — "
                                     "repeat_flag=true (회상 제외 대상)\n";
                        break;
                    }
                }
            }
        }

        all_turns.push_back(new_turn);

        // 휴리스틱: 최대 보관 개수 = recent_turns_ * 4 (기본 24)
        // MemoryLookupNode 가 recent_turns_ 개를 쓰므로 그 4배 보관하면 충분.
        // MemoryLookupNode 인스턴스 없이 단독 사용할 경우를 대비해 기본값 24 고정.
        constexpr int kMaxKeep = 24;
        int total = static_cast<int>(all_turns.size());
        if (total > kMaxKeep) {
            // 앞에서 오래된 것 제거 — 끝 kMaxKeep 개만 남김
            neograph::json trimmed = neograph::json::array();
            for (int i = total - kMaxKeep; i < total; ++i) {
                trimmed.push_back(all_turns[i]);
            }
            all_turns = std::move(trimmed);
        }

        store.put(base_ns, "turns", all_turns);
    }

    co_return out;
}

}  // namespace jarvis::memory
