// AI 국회의원 server — exposes a single persona over A2A.
//
// Each invocation runs one OpenAI-backed persona on a configured port.
// The persona reads the inbound bill text, returns its vote (찬성/반대/
// 기권) plus reasoning. The Speaker reaches it via NeoGraph's A2AClient
// over the standard /.well-known/agent-card.json discovery path.
//
// Usage:
//   member_server <port> <name> <party> <system_prompt_file>
//
// Example:
//   member_server 8101 의원_김진보 진보당 prompts/jinbo.txt
//
// .env (or env vars) must set OPENAI_API_KEY. Model is hard-coded to
// gpt-5.4-mini per project requirement.

#include <neograph/neograph.h>
#include <neograph/a2a/server.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/node.h>
#include <neograph/graph/loader.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using namespace neograph;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::GraphState;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;

namespace {

std::atomic<bool> g_shutdown{false};

void on_signal(int) { g_shutdown.store(true, std::memory_order_release); }

std::string slurp_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// PersonaNode — a single LLM call that wears the persona of one
// 국회의원. Reads `prompt` (the bill text + voting instructions from
// the Speaker), calls OpenAI with the persona's system prompt, and
// writes the model's reply to `response` for the A2A server adapter
// to surface as the agent's text response.
class PersonaNode : public GraphNode {
  public:
    PersonaNode(std::string name,
                std::shared_ptr<Provider> provider,
                std::string persona_name,
                std::string party,
                std::string system_prompt)
        : name_(std::move(name)),
          provider_(std::move(provider)),
          persona_name_(std::move(persona_name)),
          party_(std::move(party)),
          system_prompt_(std::move(system_prompt)) {}

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto raw = state.get("prompt");
        std::string user_text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        CompletionParams p;
        p.model = "gpt-5.4-mini";
        p.temperature = 0.7f;
        p.messages.push_back({"system", system_prompt_});
        p.messages.push_back({"user", user_text});

        auto reply = neograph::async::run_sync(provider_->invoke(p, nullptr));
        std::string text = reply.message.content;

        // Tag with party + name so the Speaker's transcript is readable.
        std::string framed = "[" + party_ + " " + persona_name_ + "]\n" + text;
        return {{"response", framed}};
    }

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    std::shared_ptr<Provider> provider_;
    std::string persona_name_;
    std::string party_;
    std::string system_prompt_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <port> <persona_name> <party> <system_prompt_file>\n";
        return 2;
    }
    int         port          = std::atoi(argv[1]);
    std::string persona_name  = argv[2];
    std::string party         = argv[3];
    std::string prompt_path   = argv[4];

    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key || !*api_key) {
        std::cerr << "OPENAI_API_KEY not set\n";
        return 2;
    }

    std::string system_prompt;
    try {
        system_prompt = slurp_file(prompt_path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }

    // Provider — gpt-5.4-mini per project requirement.
    // create_shared returns shared_ptr<Provider> so the NodeFactory
    // lambda below can capture and reuse the same provider across
    // every node-instantiation call.
    llm::OpenAIProvider::Config cfg;
    cfg.api_key       = api_key;
    cfg.default_model = "gpt-5.4-mini";
    auto provider = llm::OpenAIProvider::create_shared(cfg);

    // Wire the persona node into a one-step graph.
    NodeFactory::instance().register_type(
        "persona",
        [provider, persona_name, party, system_prompt](
            const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<PersonaNode>(n, provider, persona_name,
                                                 party, system_prompt);
        });

    json def = {
        {"name", "member-" + persona_name},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"persona", {{"type", "persona"}}},
        }},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "persona"}},
            json{{"from", "persona"},   {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    auto unique_engine = GraphEngine::compile(def, ctx);
    auto engine = std::shared_ptr<GraphEngine>(std::move(unique_engine));

    // AgentCard advertises this persona to the rest of the assembly.
    a2a::AgentCard card;
    card.name             = persona_name;
    card.description      = "AI 국회의원 (" + party + "). 페르소나 기반 법안 심의 + 투표.";
    card.url              = "http://127.0.0.1:" + std::to_string(port) + "/";
    card.version          = "0.1.0";
    card.protocol_version = "0.3.0";
    card.preferred_transport = "JSONRPC";
    card.default_input_modes  = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names = {"vote-on-bill"};

    a2a::A2AServer server(engine, card);
    if (!server.start_async("127.0.0.1", port)) {
        std::cerr << "[!] failed to bind on 127.0.0.1:" << port << "\n";
        return 1;
    }
    std::cout << "[" << party << " " << persona_name
              << "] listening at http://127.0.0.1:" << server.port() << "\n";

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // Park here until told to leave.
    while (!g_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "[" << persona_name << "] shutting down\n";
    return 0;
}
