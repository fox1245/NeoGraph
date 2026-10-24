// jarvis/src/orchestrator/agent_dispatcher.h
//
// Reads config/agent_registry.json and:
//   1) Fetches each A2A sub-agent's AgentCard (only when fetch_card_on_start=true)
//   2) Starts JARVIS's own A2A server (from the self section)
//   3) Registers the "a2a_delegate" graph node
//
// When the router selects e.g. delegate_to="coder", this module looks up its URL,
// forwards user_text via A2AClient::send_message, and writes the reply to delegated_reply.
//
// JARVIS's A2A server exposes the same compiled jarvis_graph engine instance —
// external requests bypass the STT stage and enter directly on the text_in channel.
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
    std::optional<neograph::a2a::AgentCard> card;  // nullopt if card fetch fails
};

class AgentDispatcher {
  public:
    static AgentDispatcher load(const std::string& registry_json_path);

    // Text injected into the router LLM prompt — agent name + expertise + keywords.
    std::string render_for_router_prompt() const;

    AgentEntry* find(const std::string& name);
    const AgentEntry* find(const std::string& name) const;

    // Expose JARVIS itself externally (self section). `engine` is the compiled jarvis_graph.
    std::unique_ptr<neograph::a2a::A2AServer>
    start_self_server(std::shared_ptr<neograph::graph::GraphEngine> engine) const;

    const std::vector<AgentEntry>& entries() const { return entries_; }

  private:
    std::vector<AgentEntry> entries_;
    neograph::json self_config_;
};

}  // namespace jarvis::orchestrator
