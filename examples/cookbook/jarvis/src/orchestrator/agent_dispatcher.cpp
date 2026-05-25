// jarvis/src/orchestrator/agent_dispatcher.cpp
//
// AgentDispatcher 구현 —
//   - agent_registry.json 파싱 → 에이전트별 A2AClient 생성
//   - fetch_card_on_start=true 인 항목에 대해 AgentCard 조회 (실패해도 진행)
//   - 라우터 LLM 에 주입할 전문가 목록 텍스트 생성
//   - 자비스 자신을 A2AServer 로 노출

#include "agent_dispatcher.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace jarvis::orchestrator {

// ─────────────────────────────────────────────────────────────────────────────
// 내부 헬퍼
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// JSON 파일을 통째로 읽어 neograph::json 으로 돌려준다.
neograph::json load_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("agent_registry.json 열기 실패: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return neograph::json::parse(ss.str());
}

/// JSON 배열(문자열)을 std::vector<std::string> 으로 변환.
/// 해당 키가 없거나 배열이 아니면 빈 벡터 반환.
std::vector<std::string> to_string_vec(const neograph::json& j,
                                       const std::string&    key) {
    std::vector<std::string> result;
    if (!j.contains(key) || !j[key].is_array()) return result;
    for (const auto& item : j[key]) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// AgentDispatcher::load  — 정적 팩토리 함수
// ─────────────────────────────────────────────────────────────────────────────

AgentDispatcher AgentDispatcher::load(const std::string& registry_json_path) {
    AgentDispatcher dispatcher;

    neograph::json registry = load_json_file(registry_json_path);

    // ── 1) "agents" 배열 처리 ──────────────────────────────────────────────
    if (registry.contains("agents") && registry["agents"].is_array()) {
        for (const auto& item : registry["agents"]) {
            // enabled=false 인 항목은 건너뜀
            if (item.contains("enabled") && !item["enabled"].get<bool>()) {
                continue;
            }

            AgentEntry entry;
            entry.name    = item.value("name",      "");
            entry.url     = item.value("url",       "");
            entry.expertise = item.value("expertise", "");
            entry.delegate_keywords = to_string_vec(item, "delegate_keywords");

            // timeout_seconds → std::chrono::seconds
            int timeout_sec = item.value("timeout_seconds", 60);
            entry.timeout = std::chrono::seconds(timeout_sec);

            // A2AClient 인스턴스 생성 + timeout 설정
            entry.client = std::make_shared<neograph::a2a::A2AClient>(entry.url);
            entry.client->set_timeout(entry.timeout);

            // fetch_card_on_start=true 이면 AgentCard 조회 시도 (실패해도 계속)
            bool fetch_card = item.value("fetch_card_on_start", false);
            if (fetch_card) {
                try {
                    auto card   = entry.client->fetch_agent_card();
                    entry.card  = std::move(card);
                    std::cerr << "[AgentDispatcher] AgentCard 조회 성공: "
                              << entry.name << " (" << entry.url << ")\n";
                } catch (const std::exception& e) {
                    // 카드 조회 실패 — entry.card 는 nullopt 그대로
                    std::cerr << "[AgentDispatcher] AgentCard 조회 실패 ("
                              << entry.name << "): " << e.what()
                              << " — 계속 진행\n";
                }
            }

            dispatcher.entries_.push_back(std::move(entry));
        }
    }

    // ── 2) "self" 섹션 보관 (start_self_server 에서 사용) ─────────────────
    if (registry.contains("self")) {
        dispatcher.self_config_ = registry["self"];
    }

    return dispatcher;
}

// ─────────────────────────────────────────────────────────────────────────────
// render_for_router_prompt  — 라우터 LLM 시스템 프롬프트용 텍스트
// ─────────────────────────────────────────────────────────────────────────────

std::string AgentDispatcher::render_for_router_prompt() const {
    if (entries_.empty()) {
        return "## Available specialist agents (A2A delegation)\n\n"
               "(등록된 전문가 에이전트 없음)\n";
    }

    std::ostringstream out;
    out << "## Available specialist agents (A2A delegation)\n\n";

    for (auto& entry : entries_) {
        out << "### " << entry.name << "\n";
        out << "- URL: " << entry.url << "\n";
        out << "- Expertise: " << entry.expertise << "\n";

        // 위임 판단 키워드 목록
        if (!entry.delegate_keywords.empty()) {
            out << "- Delegate when user says: ";
            for (std::size_t i = 0; i < entry.delegate_keywords.size(); ++i) {
                if (i > 0) out << ", ";
                out << entry.delegate_keywords[i];
            }
            out << "\n";
        }

        // AgentCard fetch 에 성공한 경우 → 카드의 skills 목록도 표시
        if (entry.card.has_value()) {
            auto& card = entry.card.value();
            if (!card.skill_names.empty()) {
                out << "- Skills (from AgentCard): ";
                for (std::size_t i = 0; i < card.skill_names.size(); ++i) {
                    if (i > 0) out << ", ";
                    out << card.skill_names[i];
                }
                out << "\n";
            }
            if (!card.description.empty()) {
                out << "- Description: " << card.description << "\n";
            }
        }

        out << "\n";
    }

    return out.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// find  — 이름으로 AgentEntry 찾기
// ─────────────────────────────────────────────────────────────────────────────

AgentEntry* AgentDispatcher::find(const std::string& name) {
    for (auto& entry : entries_) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// start_self_server  — 자비스 자신을 A2A 서버로 노출
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<neograph::a2a::A2AServer>
AgentDispatcher::start_self_server(
    std::shared_ptr<neograph::graph::GraphEngine> engine) const {

    // self 섹션이 없거나 enabled=false 이면 서버 안 띄움
    if (self_config_.is_null()) return nullptr;
    if (!self_config_.value("enabled", false)) return nullptr;

    // bind 주소 + 포트
    std::string bind_host = self_config_.value("bind_host", "127.0.0.1");
    int         bind_port = self_config_.value("bind_port", 8200);

    // AgentCard 구성 — self_config_.agent_card 필드에서 읽음
    neograph::a2a::AgentCard card;
    if (self_config_.contains("agent_card") &&
        self_config_["agent_card"].is_object()) {
        const auto& ac = self_config_["agent_card"];
        card.name        = ac.value("name",        "jarvis");
        card.description = ac.value("description", "");
        card.url         = "http://" + bind_host + ":" +
                           std::to_string(bind_port) + "/";
        card.version          = ac.value("version",          "0.1.0");
        card.protocol_version = ac.value("protocol_version", "0.3.0");
        card.preferred_transport = "JSONRPC";
        card.default_input_modes  = {"text/plain"};
        card.default_output_modes = {"text/plain"};

        // skills 배열 (문자열 목록)
        card.skill_names = to_string_vec(ac, "skills");
    } else {
        // agent_card 섹션이 아예 없는 경우 — 기본값으로 채움
        card.name             = "jarvis";
        card.description      = "Jarvis personal meta-orchestrator.";
        card.url              = "http://" + bind_host + ":" +
                                std::to_string(bind_port) + "/";
        card.version          = "0.1.0";
        card.protocol_version = "0.3.0";
        card.preferred_transport = "JSONRPC";
    }

    // A2AServer 생성 + 비동기 listen 시작 (38_a2a_server.cpp 패턴)
    auto server = std::make_unique<neograph::a2a::A2AServer>(engine, card);
    if (!server->start_async(bind_host, bind_port)) {
        std::cerr << "[AgentDispatcher] A2A self-server 바인딩 실패: "
                  << bind_host << ":" << bind_port << "\n";
        return nullptr;
    }

    std::cerr << "[AgentDispatcher] 자비스 A2A self-server 기동: "
              << "http://" << bind_host << ":" << server->port() << "\n";

    return server;
}

}  // namespace jarvis::orchestrator
