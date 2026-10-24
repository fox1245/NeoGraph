// jarvis/specialists/researcher/server.cpp
//
// Researcher specialist for JARVIS delegation — exposed as an A2A server.
// When the JARVIS router chooses mode="delegate", delegate_to="researcher",
// this server (default port 8211) receives the request and replies.
//
// Usage (launched automatically by run_session.sh):
//   ./cookbook_jarvis_specialist_researcher [port]
//
// Must be registered in JARVIS's agent_registry.json with url=http://127.0.0.1:8211
// so the router can select "delegate_to=researcher".
//
// Ends the reply with a single "[SUMMARY] <≤25 words>" line per the specialist
// contract in persona.txt; JARVIS's response_synth extracts only that line for TTS.
//
// Graph structure — two stages:
//   1. ResearchNode   : calls the LLM with the persona.txt [specialist_researcher] prompt
//   2. SummaryEnforcer: makes a follow-up LLM call if the reply lacks [SUMMARY]
//
// External web search tools (Tavily / Brave / SerpAPI) are omitted here
// so the A2A contract can be verified without extra external API keys.

#include <neograph/neograph.h>
#include <neograph/a2a/server.h>
#include "../../src/provider_support.h"

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
#include <chrono>

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

// ---------------------------------------------------------------------------
// Global shutdown signal — catch SIGINT / SIGTERM to exit the main loop
// ---------------------------------------------------------------------------
std::atomic<bool> g_shutdown{false};

void on_signal(int) { g_shutdown.store(true, std::memory_order_release); }

// ---------------------------------------------------------------------------
// Read an entire file — used to parse persona.txt
// ---------------------------------------------------------------------------
std::string slurp_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("파일 읽기 실패: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// Extract a named section from persona.txt.
// Returns the text from the line after "===<section>===" up to the next "===" header or EOF.
// ---------------------------------------------------------------------------
std::string extract_section(const std::string& text, const std::string& section) {
    std::string header = "===" + section + "===";
    auto start = text.find(header);
    if (start == std::string::npos) return "";
    start = text.find('\n', start);
    if (start == std::string::npos) return "";
    ++start; // start of the line after the header

    auto end = text.find("\n===", start);
    if (end == std::string::npos) end = text.size();

    return text.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// MockProvider — used when OPENROUTER_API_KEY is not set.
// Simulates a research reply plus the [SUMMARY] contract.
// ---------------------------------------------------------------------------
using MockProvider = jarvis::providers::MockProvider;

// ---------------------------------------------------------------------------
// ResearchNode — single LLM call; writes the research result to the `response` channel.
// External web search tools are omitted so this runs without extra API keys.
// ---------------------------------------------------------------------------
class ResearchNode : public GraphNode, public neograph::RuntimeInterpositionConsumer {
public:
    ResearchNode(std::string name,
                 std::shared_ptr<Provider> provider,
                 std::string system_prompt)
        : name_(std::move(name)),
          provider_(std::move(provider)),
          system_prompt_(std::move(system_prompt)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string user_text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        // Extension point: invoke a web search tool (Tavily / Brave / SerpAPI)
        // and append the result to user_text to enrich the context.
        //   Example:
        //   auto search_result = web_search_tool_->execute({{"query", user_text}});
        //   user_text += "\n\n[Search results]\n" + search_result;

        std::vector<sp::Message> messages;
        messages.push_back(examples::message(sp::Role::System, system_prompt_));
        messages.push_back(examples::message(sp::Role::User, user_text));
        auto request = jarvis::providers::contextual_request(
            jarvis::providers::request(*provider_, messages, 0.3), in.ctx);
        auto reply = co_await observe_provider_result(in.ctx,
            invoke_provider(provider_, std::move(request), {}, {},
                provider_call_broker(in.ctx), make_provider_call_identity(in.ctx, name_)));
        record_usage(in.ctx, reply);
        reply = neograph::outcome_or_throw(std::move(reply));
        const auto& generated = neograph::outcome_messages(*reply);
        messages.insert(messages.end(), generated.begin(), generated.end());

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(neograph::outcome_text(*reply))});
        out.writes.push_back(neograph::graph::provider_messages_write(
            std::move(messages), ChannelWrite::Mode::Overwrite));
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string              name_;
    std::shared_ptr<Provider> provider_;
    std::string              system_prompt_;
};

// ---------------------------------------------------------------------------
// SummaryEnforcer — reads `response` and makes one more LLM call to append
// [SUMMARY] if missing; passes through unchanged when already present.
// ---------------------------------------------------------------------------
class SummaryEnforcer : public GraphNode, public neograph::RuntimeInterpositionConsumer {
public:
    SummaryEnforcer(std::string name,
                    std::shared_ptr<Provider> provider)
        : name_(std::move(name)),
          provider_(std::move(provider)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("response");
        std::string text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        // Pass through unchanged if [SUMMARY] is already present
        if (text.find("[SUMMARY]") != std::string::npos) {
            NodeOutput out;
            out.writes.push_back(ChannelWrite{"response", json(text)});
            co_return out;
        }

        // Continue the exact research conversation, including all native parts.
        auto messages = in.state.get_provider_messages();
        if (messages.empty())
            throw std::logic_error("research summary requires the original native conversation");
        messages.push_back(examples::message(sp::Role::User,
            "Given your research text, append ONE line at the end: "
            "[SUMMARY] <≤25 words natural-language summary>. "
            "Output the original text unchanged, then the [SUMMARY] line."));
        auto request = jarvis::providers::contextual_request(
            jarvis::providers::request(*provider_, messages, 0.2), in.ctx);
        auto reply = co_await observe_provider_result(in.ctx,
            invoke_provider(provider_, std::move(request), {}, {},
                provider_call_broker(in.ctx), make_provider_call_identity(in.ctx, name_)));
        record_usage(in.ctx, reply);
        reply = neograph::outcome_or_throw(std::move(reply));
        std::string enforced = neograph::outcome_text(*reply);
        const auto& generated = neograph::outcome_messages(*reply);
        messages.insert(messages.end(), generated.begin(), generated.end());

        // Last-resort fallback in case the mock still omitted [SUMMARY]
        if (enforced.find("[SUMMARY]") == std::string::npos) {
            enforced += "\n[SUMMARY] research complete";
        }

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(enforced)});
        out.writes.push_back(neograph::graph::provider_messages_write(
            std::move(messages), ChannelWrite::Mode::Overwrite));
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string              name_;
    std::shared_ptr<Provider> provider_;
};

// ---------------------------------------------------------------------------
// Assemble graph — research → summary_enforcer → __end__
// ---------------------------------------------------------------------------
std::shared_ptr<GraphEngine> build_engine(std::shared_ptr<Provider> provider,
                                          const std::string&         system_prompt) {
    // Register research node
    NodeFactory::instance().register_type(
        "research",
        [provider, system_prompt](
            const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<ResearchNode>(n, provider, system_prompt);
        });

    // Register summary_enforcer node
    NodeFactory::instance().register_type(
        "summary_enforcer",
        [provider](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<SummaryEnforcer>(n, provider);
        });

    json def = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "researcher-agent"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
            {"messages", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"research",          {{"type", "research"}}},
            {"summary_enforcer",  {{"type", "summary_enforcer"}}},
        }},
        {"edges", json::array({
            json{{"from", "__start__"},        {"to", "research"}},
            json{{"from", "research"},         {"to", "summary_enforcer"}},
            json{{"from", "summary_enforcer"}, {"to", "__end__"}},
        })},
    };

    NodeContext ctx;
    auto        unique_engine =
        GraphEngine::build(def, neograph::graph::EngineConfig{.node_context = ctx});
    return std::shared_ptr<GraphEngine>(std::move(unique_engine));
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    int port = (argc >= 2) ? std::stoi(argv[1]) : 8211;

    // Resolve persona.txt relative to __FILE__ rather than the executable path
    // so the config next to the source tree is found regardless of build directory.
    // Can be overridden via the JARVIS_CONFIG_DIR environment variable.
    std::string config_dir;
    if (const char* env_dir = std::getenv("JARVIS_CONFIG_DIR")) {
        config_dir = env_dir;
    } else {
        // __FILE__ = .../specialists/researcher/server.cpp
        // config lives two directories up under config/
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
    std::string persona_path = config_dir + "/persona.txt";

    // Read persona.txt — fall back to a default prompt if missing (keep server startup going)
    std::string system_prompt;
    try {
        std::string full_text = slurp_file(persona_path);
        system_prompt = extract_section(full_text, "specialist_researcher");
        if (system_prompt.empty()) {
            std::cerr << "[researcher-specialist] 경고: persona.txt 에서 "
                         "===specialist_researcher=== 섹션을 찾지 못함. 기본 프롬프트 사용.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[researcher-specialist] 경고: " << e.what()
                  << " — 기본 프롬프트 사용.\n";
    }
    if (system_prompt.empty()) {
        system_prompt =
            "You are a research specialist. Investigate the user's query and "
            "provide a thorough answer. End with [SUMMARY] <≤25 words>.";
    }

    // Provider: OpenRouter when configured, otherwise MockProvider.
    std::shared_ptr<Provider> provider;
    const char* api_key = std::getenv("OPENROUTER_API_KEY");
    if (api_key && *api_key) {
        provider = jarvis::providers::live(api_key);
        std::cout << "[researcher-specialist] OpenRouter provider "
                     "(~deepseek/deepseek-v4-flash-latest)\n";
    } else {
        provider = std::make_shared<MockProvider>(jarvis::providers::Fixture::Researcher);
        std::cout << "[researcher-specialist] OPENROUTER_API_KEY 없음 — mock provider 사용\n";
    }

    // Assemble graph
    auto engine = build_engine(provider, system_prompt);

    // AgentCard — discovered by A2A clients at /.well-known/agent-card.json
    a2a::AgentCard card;
    card.name                = "researcher";
    card.description         = "JARVIS researcher specialist — web search + summary delegation target";
    card.url                 = "http://127.0.0.1:" + std::to_string(port) + "/";
    card.version             = "0.1.0";
    card.protocol_version    = "0.3.0";
    card.preferred_transport = "JSONRPC";
    card.default_input_modes  = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names = {"research", "summarize", "compare", "analyze"};

    // Start A2AServer
    a2a::A2AServer server(engine, card);
    if (!server.start_async("127.0.0.1", port)) {
        std::cerr << "[researcher-specialist] 포트 바인드 실패: 127.0.0.1:" << port << "\n";
        return 1;
    }

    std::cout << "[researcher-specialist] online @ 127.0.0.1:" << server.port() << "\n";

    // Register shutdown signal handlers
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // Wait until a signal arrives
    while (!g_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "[researcher-specialist] 종료\n";
    return 0;
}
