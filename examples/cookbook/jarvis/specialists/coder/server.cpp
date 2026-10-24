// jarvis/specialists/coder/server.cpp
//
// Coder specialist for JARVIS delegation — exposed as an A2A server.
//
// When the JARVIS router chooses mode="delegate", delegate_to="coder",
// A2AClient forwards user_text to this server (default port 8210).
// A graph with a single PersonaNode generates the LLM reply, followed by
// SummaryEnforcerNode which ensures a [SUMMARY] marker is present (appending a trailing line if missing).
// JARVIS's response_synth reads only that [SUMMARY] line aloud via TTS.
//
// Usage:
//   ./cookbook_jarvis_specialist_coder [port]    (default 8210)
//
// Falls back to MockProvider when OPENROUTER_API_KEY is not set in .env or the environment.
//
// Structured after the same pattern as ai-assembly/member_server.cpp.

#include <neograph/neograph.h>
#include <neograph/a2a/server.h>
#include "../../src/provider_support.h"
#include <neograph/async/run_sync.h>
#include <neograph/graph/node.h>
#include <neograph/graph/loader.h>

#include <cppdotenv/dotenv.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::GraphState;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

namespace {

std::atomic<bool> g_shutdown{false};

void on_signal(int) { g_shutdown.store(true, std::memory_order_release); }

// ──────────────────────────────────────────────────────────────────────────
// Simple Mock Provider used when no API key is configured
// ──────────────────────────────────────────────────────────────────────────
using MockProvider = jarvis::providers::MockProvider;

// ──────────────────────────────────────────────────────────────────────────
// Extract an ===<section>=== block from persona.txt
// ──────────────────────────────────────────────────────────────────────────
std::string extract_persona_section(const std::string& file_path,
                                    const std::string& section_name) {
    std::ifstream f(file_path);
    if (!f) return "";          // return empty string if missing — do not fail hard

    const std::string marker = "===" + section_name + "===";
    std::string line;
    bool in_section = false;
    std::ostringstream buf;

    while (std::getline(f, line)) {
        if (!in_section) {
            if (line == marker) { in_section = true; }
            continue;
        }
        // Stop at the next === header
        if (line.size() >= 3 && line[0] == '=' && line[1] == '=' && line[2] == '=')
            break;
        buf << line << '\n';
    }
    return buf.str();
}

// ──────────────────────────────────────────────────────────────────────────
// PersonaNode — applies the coder persona system prompt and sends the
//               user prompt (from the "prompt" channel) to the LLM
// ──────────────────────────────────────────────────────────────────────────
class PersonaNode : public GraphNode, public neograph::RuntimeInterpositionConsumer {
public:
    PersonaNode(std::string node_name,
                std::shared_ptr<Provider> provider,
                std::string system_prompt)
        : name_(std::move(node_name)),
          provider_(std::move(provider)),
          system_prompt_(std::move(system_prompt)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string user_text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        std::vector<sp::Message> messages;
        messages.push_back(examples::message(sp::Role::System, system_prompt_));
        messages.push_back(examples::message(sp::Role::User, user_text));
        auto request = jarvis::providers::contextual_request(
            jarvis::providers::request(*provider_, std::move(messages), 0.7), in.ctx);
        auto reply = co_await observe_provider_result(in.ctx,
            invoke_provider(provider_, std::move(request), {}, {},
                provider_call_broker(in.ctx), make_provider_call_identity(in.ctx, name_)));
        record_usage(in.ctx, reply);
        reply = neograph::outcome_or_throw(std::move(reply));

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(neograph::outcome_text(*reply))});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string             name_;
    std::shared_ptr<Provider> provider_;
    std::string             system_prompt_;
};

// ──────────────────────────────────────────────────────────────────────────
// SummaryEnforcerNode — ensures the LLM reply contains [SUMMARY] (appending a trailing line if missing)
//   - If present: pass through unchanged
//   - If missing: append the first 25 words of the body as the summary line
// ──────────────────────────────────────────────────────────────────────────
class SummaryEnforcerNode : public GraphNode {
public:
    explicit SummaryEnforcerNode(std::string node_name)
        : name_(std::move(node_name)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("response");
        std::string text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        // Check whether [SUMMARY] is present
        if (text.find("[SUMMARY]") == std::string::npos) {
            // Take the first 25 words and append them as the summary line
            std::istringstream iss(text);
            std::string word;
            std::vector<std::string> words;
            while (iss >> word && words.size() < 25)
                words.push_back(word);

            std::string summary;
            for (size_t i = 0; i < words.size(); ++i) {
                if (i) summary += ' ';
                summary += words[i];
            }
            // Append a trailing period if not already present
            if (!summary.empty() && summary.back() != '.')
                summary += '.';

            text += "\n[SUMMARY] " + summary;
        }

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(text)});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

}  // namespace

// ──────────────────────────────────────────────────────────────────────────
// main
// ──────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // Load .env (OPENROUTER_API_KEY, etc.)
    cppdotenv::auto_load_dotenv();

    int port = (argc >= 2) ? std::atoi(argv[1]) : 8210;

    // Load the coder specialist system prompt from persona.txt.
    // Try ../../config/persona.txt relative to the executable first.
    const std::string persona_file =
        std::string(argv[0]).substr(0, std::string(argv[0]).rfind('/') + 1)
        + "../../config/persona.txt";

    std::string system_prompt = extract_persona_section(persona_file, "specialist_coder");
    if (system_prompt.empty()) {
        // The build tree layout may differ, so also try the source-relative path.
        // Prefer JARVIS_CONFIG_DIR when set; otherwise look two levels up from
        // __FILE__ under config/ (same convention as researcher/server.cpp).
        std::string config_dir;
        if (const char* env_dir = std::getenv("JARVIS_CONFIG_DIR")) {
            config_dir = env_dir;
        } else {
            // __FILE__ = .../specialists/coder/server.cpp
            std::string src_path = __FILE__;
            auto slash = src_path.rfind('/');
            slash = (slash != std::string::npos)
                        ? src_path.rfind('/', slash - 1)
                        : std::string::npos;
            slash = (slash != std::string::npos)
                        ? src_path.rfind('/', slash - 1)
                        : std::string::npos;
            config_dir = (slash != std::string::npos)
                             ? src_path.substr(0, slash + 1) + "config"
                             : "config";
        }
        system_prompt = extract_persona_section(
            config_dir + "/persona.txt", "specialist_coder");
    }
    if (system_prompt.empty()) {
        // Minimal fallback if persona.txt still could not be read
        system_prompt =
            "You are the coder specialist behind JARVIS. "
            "End your reply with a [SUMMARY] line of ≤25 words.";
        std::cerr << "[coder-specialist] persona.txt 를 찾지 못해 기본 프롬프트 사용\n";
    }

    // Provider: OpenRouter when configured, otherwise MockProvider.
    std::shared_ptr<Provider> provider;
    const char* api_key = std::getenv("OPENROUTER_API_KEY");
    if (api_key && *api_key) {
        provider = jarvis::providers::live(api_key);
    } else {
        std::cerr << "[coder-specialist] OPENROUTER_API_KEY 없음 — MockProvider 로 동작\n";
        provider = std::make_shared<MockProvider>(jarvis::providers::Fixture::Coder);
    }

    // Register node type — PersonaNode
    NodeFactory::instance().register_type(
        "coder_persona",
        [provider, system_prompt](
            const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<PersonaNode>(n, provider, system_prompt);
        });

    // Register node type — SummaryEnforcerNode
    NodeFactory::instance().register_type(
        "summary_enforcer",
        [](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<SummaryEnforcerNode>(n);
        });

    // Graph definition: two stages (persona → summary_enforcer)
    json def = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "coder-specialist"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"persona",          {{"type", "coder_persona"}}},
            {"summary_enforcer", {{"type", "summary_enforcer"}}},
        }},
        {"edges", json::array({
            json{{"from", "__start__"},        {"to", "persona"}},
            json{{"from", "persona"},          {"to", "summary_enforcer"}},
            json{{"from", "summary_enforcer"}, {"to", "__end__"}},
        })},
    };

    NodeContext ctx;
    auto        unique_engine =
        GraphEngine::build(def, neograph::graph::EngineConfig{.node_context = ctx});
    auto engine = std::shared_ptr<GraphEngine>(std::move(unique_engine));

    // AgentCard — looked up by the JARVIS router under the name "coder"
    a2a::AgentCard card;
    card.name                = "coder";
    card.description         = "JARVIS coder specialist — code writing/review/debug delegation target";
    card.url                 = "http://127.0.0.1:" + std::to_string(port) + "/";
    card.version             = "0.1.0";
    card.protocol_version    = "0.3.0";
    card.preferred_transport = "JSONRPC";
    card.default_input_modes  = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names = {"code", "debug", "review", "refactor"};

    a2a::A2AServer server(engine, card);
    if (!server.start_async("127.0.0.1", port)) {
        std::cerr << "[coder-specialist] 바인드 실패 — 127.0.0.1:" << port << "\n";
        return 1;
    }

    std::cout << "[coder-specialist] online @ 127.0.0.1:" << server.port() << "\n";

    // Catch SIGINT / SIGTERM for graceful shutdown
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    while (!g_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "[coder-specialist] 종료\n";
    return 0;
}
