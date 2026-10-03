// AI 국회의원 server — exposes a single persona over A2A.
//
// Each invocation runs one OpenRouter-backed DeepSeek persona on a configured
// port. The persona reads the inbound bill text, returns its vote (찬성/반대/
// 기권) plus reasoning. The Speaker reaches it via NeoGraph's A2AClient
// over the standard /.well-known/agent-card.json discovery path.
//
// Usage:
//   member_server <port> <name> <party> <system_prompt_file>
//
// Example:
//   member_server 8101 의원_김진보 진보당 prompts/jinbo.txt
//
// .env (or env vars) must set OPENROUTER_API_KEY. The model is hard-coded to
// ~deepseek/deepseek-v4-flash-latest.

#include <neograph/neograph.h>
#include <neograph/a2a/server.h>
#include "../../provider_example_support.h"
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
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

namespace {

std::atomic<bool> g_shutdown{false};

void on_signal(int) { g_shutdown.store(true, std::memory_order_release); }

std::string slurp_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Explicit local fixture: real SDK preparation, no provider I/O, invented usage,
// or native replay seals. Owned response state survives provider destruction.
class MockMemberProvider final : public Provider {
    std::shared_ptr<sp::runtime::Client> client_ = examples::make_local_client();
public:
    std::string get_name() const override { return "assembly-local-fixture"; }
    std::string_view family() const noexcept override { return "openai.chat"; }
    PreparedProviderRequest prepare(ProviderRequest request) override {
        const auto& messages = examples::request_messages(request);
        std::string bill;
        for (const auto& message : messages)
            if (message.role == sp::Role::User)
                bill = project_message(message).content;
        sp::Completion completion;
        completion.messages.push_back(examples::message(sp::Role::Assistant,
            "기권\nSynthetic offline vote; no model judgment was made.\nBill excerpt: " +
            bill.substr(0, 120)));
        completion.stop = {sp::StopKind::EndTurn, "synthetic-end"};
        auto outcome = std::make_shared<const sp::Outcome>(std::move(completion));
        return prepare_local(client_, std::move(request),
            [outcome = std::move(outcome)](
                const PreparedProviderRequest&,
                const std::function<void(const sp::Event&)>& on_event)
                -> asio::awaitable<sp::runtime::Result> {
                examples::emit_local_events(std::get<sp::Completion>(*outcome), on_event);
                co_return outcome;
            });
    }
};

// PersonaNode — a single LLM call that wears the persona of one
// 국회의원. Reads `prompt` (the bill text + voting instructions from
// the pinned OpenRouter DeepSeek route with the persona's system prompt,
// writes the model's reply to `response` for the A2A server adapter
// to surface as the agent's text response.
class PersonaNode : public GraphNode, public neograph::RuntimeInterpositionConsumer {
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

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string user_text = raw.is_string() ? raw.get<std::string>() : raw.dump();

        ProviderControls controls;
        controls.temperature = 0.7;
        auto request = make_provider_request(
            *provider_, "~deepseek/deepseek-v4-flash-latest",
            {portable_message({"system", system_prompt_}),
             portable_message({"user", user_text})}, {}, std::move(controls));
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        request.on_event = in.ctx.on_provider_event;
        auto reply = co_await observe_provider_result(in.ctx,
            invoke_provider(provider_, std::move(request), {}, {},
                provider_call_broker(in.ctx), make_provider_call_identity(in.ctx, name_)));
        record_usage(in.ctx, reply);
        reply = outcome_or_throw(std::move(reply));
        // A2A intentionally publishes a portable persona summary, not native history.
        std::string text = outcome_text(*reply);

        // Tag with party + name so the Speaker's transcript is readable.
        std::string framed = "[" + party_ + " " + persona_name_ + "]\n" + text;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json(framed)});
        co_return out;
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
    if (argc < 5 || argc > 6 || (argc == 6 && std::string_view(argv[5]) != "--mock")) {
        std::cerr << "Usage: " << argv[0]
                  << " <port> <persona_name> <party> <system_prompt_file> [--mock]\n";
        return 2;
    }
    int         port          = std::atoi(argv[1]);
    std::string persona_name  = argv[2];
    std::string party         = argv[3];
    std::string prompt_path   = argv[4];

    const bool mock = argc == 6;
    const char* api_key = mock ? nullptr : std::getenv("OPENROUTER_API_KEY");
    if (!mock && (!api_key || !*api_key)) {
        std::cerr << "OPENROUTER_API_KEY not set\n";
        return 2;
    }

    std::string system_prompt;
    try {
        system_prompt = slurp_file(prompt_path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }

    // The explicit offline fixture uses the same admitted typed request boundary.
    std::shared_ptr<Provider> provider;
    if (mock) provider = std::make_shared<MockMemberProvider>();
    else provider = examples::make_openrouter_provider(api_key, "chat");

    // Wire the persona node into a one-step graph.
    NodeFactory::instance().register_type(
        "persona",
        [provider, persona_name, party, system_prompt](
            const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<PersonaNode>(n, provider, persona_name,
                                                 party, system_prompt);
        });

    json def = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
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
    auto        unique_engine =
        GraphEngine::build(def, neograph::graph::EngineConfig{.node_context = ctx});
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
