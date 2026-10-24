// jarvis/src/orchestrator/agent_dispatcher.cpp
//
// AgentDispatcher implementation —
//   - Parses agent_registry.json → creates an A2AClient per specialist agent
//   - Fetches AgentCard at startup when fetch_card_on_start=true (continues on failure)
//   - Renders the specialist list text injected into the router LLM prompt
//   - Exposes JARVIS itself as an A2AServer

#include "agent_dispatcher.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace jarvis::orchestrator {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Read an entire JSON file and return it as neograph::json.
neograph::json load_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("agent_registry.json 열기 실패: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return neograph::json::parse(ss.str());
}

/// Convert a JSON string array into std::vector<std::string>.
/// Returns an empty vector if the key is missing or not an array.
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
// AgentDispatcher::load  — static factory function
// ─────────────────────────────────────────────────────────────────────────────

AgentDispatcher AgentDispatcher::load(const std::string& registry_json_path) {
    AgentDispatcher dispatcher;

    neograph::json registry = load_json_file(registry_json_path);

    // ── 1) Process the "agents" array ──────────────────────────────────────
    if (registry.contains("agents") && registry["agents"].is_array()) {
        for (const auto& item : registry["agents"]) {
            // Skip entries with enabled=false
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

            // Create A2AClient instance and configure timeout
            entry.client = std::make_shared<neograph::a2a::A2AClient>(entry.url);
            entry.client->set_timeout(entry.timeout);

            // Attempt AgentCard fetch when fetch_card_on_start=true (continue on failure)
            bool fetch_card = item.value("fetch_card_on_start", false);
            if (fetch_card) {
                try {
                    auto card   = entry.client->fetch_agent_card();
                    entry.card  = std::move(card);
                    std::cerr << "[AgentDispatcher] AgentCard 조회 성공: "
                              << entry.name << " (" << entry.url << ")\n";
                } catch (const std::exception& e) {
                    // Card fetch failed — leave entry.card as nullopt
                    std::cerr << "[AgentDispatcher] AgentCard 조회 실패 ("
                              << entry.name << "): " << e.what()
                              << " — 계속 진행\n";
                }
            }

            dispatcher.entries_.push_back(std::move(entry));
        }
    }

    // ── 2) Store the "self" section (used by start_self_server) ────────────
    if (registry.contains("self")) {
        dispatcher.self_config_ = registry["self"];
    }

    return dispatcher;
}

// ─────────────────────────────────────────────────────────────────────────────
// render_for_router_prompt  — text for the router LLM system prompt
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

        // Delegation trigger keywords
        if (!entry.delegate_keywords.empty()) {
            out << "- Delegate when user says: ";
            for (std::size_t i = 0; i < entry.delegate_keywords.size(); ++i) {
                if (i > 0) out << ", ";
                out << entry.delegate_keywords[i];
            }
            out << "\n";
        }

        // If AgentCard fetch succeeded, also list the card's skills
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
// find  — look up AgentEntry by name
// ─────────────────────────────────────────────────────────────────────────────

AgentEntry* AgentDispatcher::find(const std::string& name) {
    for (auto& entry : entries_) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

const AgentEntry* AgentDispatcher::find(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// start_self_server  — expose JARVIS itself as an A2A server
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<neograph::a2a::A2AServer>
AgentDispatcher::start_self_server(
    std::shared_ptr<neograph::graph::GraphEngine> engine) const {

    // Do not start the server if the self section is missing or enabled=false
    if (self_config_.is_null()) return nullptr;
    if (!self_config_.value("enabled", false)) return nullptr;

    // Bind host + port
    std::string bind_host = self_config_.value("bind_host", "127.0.0.1");
    int         bind_port = self_config_.value("bind_port", 8200);

    // Populate AgentCard from self_config_.agent_card
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

        // skills array (string list)
        card.skill_names = to_string_vec(ac, "skills");
    } else {
        // Fill in defaults when the agent_card section is absent
        card.name             = "jarvis";
        card.description      = "Jarvis personal meta-orchestrator.";
        card.url              = "http://" + bind_host + ":" +
                                std::to_string(bind_port) + "/";
        card.version          = "0.1.0";
        card.protocol_version = "0.3.0";
        card.preferred_transport = "JSONRPC";
    }

    // Create A2AServer and start async listener (same pattern as 38_a2a_server.cpp)
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
