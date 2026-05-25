// jarvis/src/orchestrator/intent_router_node.h
//
// 자비스의 두뇌. 작은/빠른 LLM 한 번으로 다음 JSON 을 만들어 route_decision 채널에 씀.
//
//   {
//     "mode": "direct" | "delegate" | "parallel",
//     "tool_calls": [ { "tool": "<name>", "args": { ... } } ],
//     "delegate_to": "<agent_name>" | null,
//     "skip_synthesis": bool
//   }
//
// 기존 IntentClassifier 와 다른 점:
//   - 단순 라벨 출력이 아니라 구조화 JSON 출력
//   - 시스템 프롬프트가 런타임에 합성됨 (McpCatalog + AgentDispatcher 의 render*)
//   - JSON 파싱 실패 시 "direct, skip_synthesis=true, tool_calls=[]" 로 안전 폴백
//     → 빈 응답 합성 단계로 흘러가서 자비스가 "Sir, 다시 말씀해 주시겠습니까?" 같은
//        템플릿 답변. 무한루프 방지.
//
// 등록 (main.cpp):
//   NodeFactory::register_type("intent_classifier", [&](...) {
//       return std::make_unique<IntentRouterNode>(name, cfg, catalog_ref, dispatcher_ref);
//   });
#pragma once

#include <neograph/neograph.h>

namespace jarvis::orchestrator {

class McpCatalog;
class AgentDispatcher;

class IntentRouterNode : public neograph::graph::GraphNode {
  public:
    IntentRouterNode(std::string name,
                     const neograph::json& cfg,
                     const McpCatalog& catalog,
                     const AgentDispatcher& dispatcher,
                     std::shared_ptr<neograph::Provider> provider);

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override;

    std::string get_name() const override { return name_; }

  private:
    std::string compose_system_prompt() const;
    neograph::json safe_parse_or_fallback(const std::string& raw) const;

    std::string name_;
    std::string model_;
    std::string output_channel_;
    std::string prompt_file_;
    std::string prompt_section_;
    const McpCatalog& catalog_;
    const AgentDispatcher& dispatcher_;
    std::shared_ptr<neograph::Provider> provider_;
};

}  // namespace jarvis::orchestrator
