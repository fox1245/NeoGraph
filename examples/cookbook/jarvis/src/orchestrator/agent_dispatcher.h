// jarvis/src/orchestrator/agent_dispatcher.h
//
// config/agent_registry.json 을 읽고:
//   1) 각 A2A 서브에이전트의 AgentCard 조회 (fetch_card_on_start=true 만)
//   2) 자비스 자신의 A2A 서버 (self 섹션) 띄움
//   3) "a2a_delegate" 그래프 노드 등록
//
// 라우터가 delegate_to="coder" 같은 이름을 주면, 이 모듈이 url 을 찾아서
// A2AClient::send_message 로 user_text 통째 전송, 응답을 delegated_reply 채널에 씀.
//
// 자비스의 A2A 서버는 jarvis_graph 와 같은 엔진 인스턴스를 그대로 노출 —
// 외부 호출이 들어오면 STT 단계 건너뛰고 text_in 채널로 주입됨.
#pragma once

#include <neograph/neograph.h>
#include <neograph/a2a/client.h>
#include <neograph/a2a/server.h>

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jarvis::orchestrator {

struct AgentEntry {
    std::string name;
    std::string url;
    std::string expertise;
    std::vector<std::string> delegate_keywords;
    std::chrono::seconds timeout{60};
    std::shared_ptr<neograph::a2a::A2AClient> client;
    std::optional<neograph::a2a::AgentCard> card;  // fetch 실패하면 nullopt
};

class AgentDispatcher {
  public:
    static AgentDispatcher load(const std::string& registry_json_path);

    // 라우터 LLM 에 주입할 텍스트 — 각 에이전트 이름 + expertise + keywords.
    std::string render_for_router_prompt() const;

    AgentEntry* find(const std::string& name);
    const AgentEntry* find(const std::string& name) const;

    // 자비스 자신을 외부에 노출 (self 섹션). engine 은 jarvis_graph 컴파일 결과.
    std::unique_ptr<neograph::a2a::A2AServer>
    start_self_server(std::shared_ptr<neograph::graph::GraphEngine> engine) const;

    const std::vector<AgentEntry>& entries() const { return entries_; }

  private:
    std::vector<AgentEntry> entries_;
    neograph::json self_config_;
};

}  // namespace jarvis::orchestrator
