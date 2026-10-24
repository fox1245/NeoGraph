// jarvis/src/orchestrator/intent_router_node.h
//
// The brain of JARVIS. Makes a single small/fast LLM call to produce the
// following JSON and writes it to the route_decision channel:
//
//   {
//     "mode": "direct" | "delegate" | "parallel",
//     "tool_calls": [ { "tool": "<name>", "args": { ... } } ],
//     "delegate_to": "<agent_name>" | null,
//     "skip_synthesis": bool
//   }
//
// Differences from a plain IntentClassifier:
//   - Outputs structured JSON rather than a single label
//   - Composes the system prompt at runtime (from McpCatalog + AgentDispatcher render*)
//   - Safely falls back on JSON parse failure so the turn still completes cleanly
//     through the synthesizer without entering an infinite loop.
//
// Registration (main.cpp):
//   NodeFactory::register_type("intent_classifier", [&](...) {
//       return std::make_unique<IntentRouterNode>(name, cfg, catalog_ref, dispatcher_ref);
//   });
#pragma once

#include <neograph/neograph.h>

namespace jarvis::orchestrator {

class McpCatalog;
class AgentDispatcher;

class IntentRouterNode : public neograph::graph::GraphNode,
                         public neograph::RuntimeInterpositionConsumer {
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
