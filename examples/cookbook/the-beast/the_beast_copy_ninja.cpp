// NeoGraph Cookbook — "The Beast × Copy Ninja", LIVE
// ========================================================
// A live LLM authors one tightly constrained topology around a separately
// materialized local Copy Ninja harness. The source card remains untrusted
// discovery data: no card-provided endpoint, text, credential, or source code
// can reach the graph. The generated graph may invoke only CopyNinjaNode.
//
// Setup: put OPENROUTER_API_KEY=sk-or-... in .env beside the binary
// Build: cmake --build build --target cookbook_the_beast_copy_ninja
// Run:   ./build/cookbook_the_beast_copy_ninja "Grace"

#include <neograph/a2a/agent_card_candidate.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/neograph.h>

#include "beast_common.h"
#include <cppdotenv/dotenv.hpp>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using neograph::json;
namespace a2a   = neograph::a2a;
namespace beast = neograph::cookbook::beast;
namespace ng    = neograph::graph;

namespace {

constexpr std::string_view kCopyNinjaNodeType = "copy_ninja_local";
constexpr std::string_view kCopyNinjaTemplate = "copy-ninja.hello-world-echo.v1";

// This is deliberately a local synthetic discovery fixture. Its raw card
// models hostile free-form content and an unreachable advertised RPC endpoint;
// the collector fetches only the well-known card and never follows that URL.
class CandidateCardServer {
public:
    CandidateCardServer() {
        server_.Get("/.well-known/agent-card.json",
                    [this](const httplib::Request&, httplib::Response& response) {
                        card_requests_.fetch_add(1, std::memory_order_relaxed);
                        response.status = 200;
                        response.set_content(card_.dump(), "application/json");
                    });
        server_.Post("/", [this](const httplib::Request&, httplib::Response& response) {
            rpc_requests_.fetch_add(1, std::memory_order_relaxed);
            response.status = 500;
            response.set_content("unexpected source-agent RPC", "text/plain");
        });

        port_   = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        for (int i = 0; i < 200 && !server_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (port_ == 0 || !server_.is_running()) {
            throw std::runtime_error("Copy Ninja discovery fixture failed to start");
        }
    }

    ~CandidateCardServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    CandidateCardServer(const CandidateCardServer&)            = delete;
    CandidateCardServer& operator=(const CandidateCardServer&) = delete;

    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    int         card_requests() const { return card_requests_.load(std::memory_order_relaxed); }
    int         rpc_requests() const { return rpc_requests_.load(std::memory_order_relaxed); }

private:
    httplib::Server  server_;
    std::thread      thread_;
    int              port_ = 0;
    std::atomic<int> card_requests_{0};
    std::atomic<int> rpc_requests_{0};
    json             card_ = json::parse(R"json(
{
  "name": "copy-ninja-demo-source",
  "description": "UNTRUSTED_CARD_TEXT_must_not_become_candidate_code",
  "url": "https://source-agent.example.invalid/rpc",
  "version": "deadbeef",
  "protocolVersion": "1.0",
  "preferredTransport": "JSONRPC",
  "capabilities": {"streaming": false, "pushNotifications": false},
  "defaultInputModes": ["text/plain"],
  "defaultOutputModes": ["text/plain"],
  "skills": [{
    "id": "echo-bot",
    "name": "UNTRUSTED_SKILL_NAME",
    "description": "UNTRUSTED_SKILL_DESCRIPTION",
    "tags": ["a2a", "echo-example"],
    "examples": ["never execute this text"]
  }]
}
)json");
};

a2a::AgentCardSourceProvenance demo_source_provenance() {
    return {
        {},
        "https://github.com/a2aproject/a2a-samples/blob/0123456789abcdef0123456789abcdef01234567/"
        "samples/python/agents/helloworld/__main__.py",
        "0123456789abcdef0123456789abcdef01234567",
        "Apache-2.0",
    };
}

std::shared_ptr<const a2a::CopyNinjaHarness> materialize_local_harness(
    CandidateCardServer& source_server) {
    a2a::AgentCardCollectionPolicy collection_policy;
    collection_policy.allow_loopback_http = true;
    const auto collected                  = a2a::AgentCardCollector(collection_policy)
                               .collect(source_server.origin(), demo_source_provenance());
    const auto candidate = a2a::AgentCardCandidateCompiler::compile(collected);

    a2a::CopyNinjaBehavioralProfile profile;
    profile.source_card_sha256 = candidate.source_card_sha256();
    profile.template_id        = std::string(kCopyNinjaTemplate);
    profile.development_probes = {
        {"Ada", "Hello, World! I have received your request (Ada)"},
    };

    std::cout << "  collected card digest: " << candidate.source_card_sha256() << "\n"
              << "  candidate state: " << candidate.descriptor().at("state") << "\n"
              << "  source discovery GETs: " << source_server.card_requests()
              << ", source RPCs: " << source_server.rpc_requests() << "\n";
    return std::make_shared<a2a::CopyNinjaHarness>(
        a2a::materialize_copy_ninja(candidate, std::move(profile)));
}

void register_local_copy_ninja_node(std::shared_ptr<const a2a::CopyNinjaHarness> harness) {
    ng::NodeFactory::instance().register_type(
        std::string(kCopyNinjaNodeType),
        [harness = std::move(harness)](const std::string& name, const json&,
                                       const ng::NodeContext&) {
            return std::make_unique<a2a::CopyNinjaNode>(name, harness);
        },
        json::parse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        json::parse(R"({"reads":["prompt"],"writes":["response"]})"));
}

// This fourth gate is intentionally application-specific. The three generic
// compiler gates establish a valid graph; this check proves that the generated
// graph cannot substitute another node or add an unreviewed side effect.
std::string local_binding_error(const json& core) {
    if (!core.is_object() || !core.contains("channels") || !core["channels"].is_object()) {
        return "strict Core lockfile has no channel object";
    }
    const auto& channels = core.at("channels");
    if (channels.size() != 2) {
        return "requires exactly the prompt and response channels";
    }
    for (const auto channel_name : {"prompt", "response"}) {
        const auto channel = channels.find(channel_name);
        if (channel == channels.end() || !channel.value().is_object() ||
            channel.value().value("reducer", "") != "overwrite") {
            return std::string("requires an overwrite ") + channel_name + " channel";
        }
    }

    if (!core.contains("nodes") || !core["nodes"].is_object() || core["nodes"].size() != 1) {
        return "requires exactly one local Copy Ninja node";
    }
    const auto node = core["nodes"].begin();
    if (node.key() != "copy_ninja" || !node.value().is_object() || node.value().size() != 1 ||
        node.value().value("type", "") != kCopyNinjaNodeType) {
        return "the sole node must be an unconfigured copy_ninja of type copy_ninja_local";
    }

    if (!core.contains("edges") || !core["edges"].is_array() || core["edges"].size() != 2) {
        return "requires exactly __start__ -> copy_ninja -> __end__";
    }
    const auto& edges    = core.at("edges");
    const auto  has_edge = [&edges](std::string_view from, std::string_view to) {
        for (const auto& edge : edges) {
            if (edge.is_object() && edge.value("from", "") == from && edge.value("to", "") == to) {
                return true;
            }
        }
        return false;
    };
    if (!has_edge("__start__", "copy_ninja") || !has_edge("copy_ninja", "__end__")) {
        return "requires exactly __start__ -> copy_ninja -> __end__";
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();
    const char* key = std::getenv("OPENROUTER_API_KEY");
    if (!key || !*key) {
        std::cerr << "OPENROUTER_API_KEY not set (env or .env beside the binary)\n";
        return 2;
    }

    const std::string   local_prompt = argc > 1 ? argv[1] : "Grace";
    CandidateCardServer source_server;

    std::cout << "============ THE BEAST × COPY NINJA (live) ============\n"
              << "Collecting one local synthetic card; card text and endpoint stay untrusted.\n";
    const auto harness = materialize_local_harness(source_server);
    register_local_copy_ninja_node(harness);

    const std::string model            = "deepseek/deepseek-v4-flash-0731";
    const json        provider_routing = {
        {"zdr", true},
        {"only", json::array({"morph"})},
        {"allow_fallbacks", false},
    };
    auto provider =
        neograph::llm::OpenAIProvider::create_shared({.api_key       = key,
                                                      .base_url      = "https://openrouter.ai/api",
                                                      .default_model = model,
                                                      .timeout_seconds = 180});

    ng::NodeContext context;
    context.provider = provider;

    // `local_prompt` deliberately does not appear in these messages. The LLM
    // authors only topology; the actual caller value is consumed locally after
    // the graph passes all gates.
    const std::string system_prompt =
        "You are the architect of one tightly constrained NeoGraph harness. Output ONLY one "
        "JSON topology object, no prose or markdown. It must have schema_version 1; exactly two "
        "channels named prompt and response, each with reducer overwrite; exactly one node named "
        "copy_ninja with type copy_ninja_local; and exactly two edges __start__ -> copy_ninja and "
        "copy_ninja -> __end__. Do not include legacy composition keys, any other node type, config, source "
        "URL, "
        "tool, credential, or executable text. The node reads prompt and writes response.";
    std::vector<neograph::ChatMessage> conversation = {
        {"system", system_prompt},
        {"user", "Author the exact local Copy Ninja topology now."},
    };

    json accepted_core;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        std::cout << "── Attempt #" << attempt << ": authoring the local topology ──\n";
        neograph::CompletionParams params;
        params.model        = model;
        params.messages     = conversation;
        params.temperature  = 0.0f;
        params.max_tokens   = 4000;
        params.extra_fields = {{"provider", provider_routing}};

        neograph::ChatCompletion response;
        try {
            response = neograph::async::run_sync(provider->invoke(params, nullptr));
        } catch (const std::exception& error) {
            std::cerr << "  LLM error: " << error.what() << "\n";
            return 1;
        }

        json core_candidate;
        try {
            core_candidate = beast::extract_json_object(response.message.content);
        } catch (const std::exception& error) {
            std::cout << "  UNPARSEABLE (" << error.what() << "); asking again.\n";
            conversation.push_back({"assistant", response.message.content});
            conversation.push_back({"user", "Output only the required JSON topology object."});
            continue;
        }

        const auto verdict = beast::validate_harness(core_candidate, context);
        if (!verdict.ok) {
            std::cout << "  REJECTED at generic gate '" << verdict.gate
                      << "': " << verdict.report.substr(0, 400) << "\n";
            conversation.push_back({"assistant", core_candidate.dump()});
            conversation.push_back({"user", "The generic compiler rejected it at '" + verdict.gate +
                                                "': " + verdict.report +
                                                "\nFix only that error. Output only JSON."});
            continue;
        }

        const auto binding_error = local_binding_error(verdict.core);
        if (!binding_error.empty()) {
            std::cout << "  REJECTED at local-binding gate: " << binding_error << "\n";
            conversation.push_back({"assistant", core_candidate.dump()});
            conversation.push_back({"user", "The local-binding gate rejected it: " + binding_error +
                                                ". Output the exact required JSON topology."});
            continue;
        }

        accepted_core = verdict.core;
        std::cout << "  ACCEPTED — three generic gates plus the local-binding gate passed.\n";
        break;
    }

    if (accepted_core.is_null()) {
        std::cerr << "The model could not author the restricted topology in three tries.\n";
        return 1;
    }

    auto checkpoints = std::make_shared<ng::InMemoryCheckpointStore>();
    auto engine      = ng::GraphEngine::build(
        accepted_core, ng::EngineConfig{.node_context = context, .checkpoint_store = checkpoints});
    ng::RunConfig run_config;
    run_config.thread_id = "beast-copy-ninja";
    run_config.input     = {{"prompt", local_prompt}};
    run_config.max_steps = 10;

    const auto result = engine->run(run_config);
    if (!result.has_channel("response")) {
        std::cerr << "Copy Ninja graph completed without a response channel.\n";
        return 1;
    }
    const auto response_value = result.channel<json>("response");
    if (!response_value.is_string()) {
        std::cerr << "Copy Ninja graph response is not text.\n";
        return 1;
    }

    const auto actual   = response_value.get<std::string>();
    const auto expected = harness->respond(local_prompt);
    if (actual != expected) {
        std::cerr << "Local harness mismatch: expected " << expected << ", got " << actual << "\n";
        return 1;
    }
    if (source_server.rpc_requests() != 0) {
        std::cerr << "Invariant failure: graph dispatched to the source agent.\n";
        return 1;
    }

    std::cout << "── Local execution verified ──\n"
              << "  prompt: " << local_prompt << "\n"
              << "  response: " << actual << "\n"
              << "  checkpoints: " << checkpoints->list("beast-copy-ninja").size() << "\n"
              << "  source discovery GETs: " << source_server.card_requests()
              << ", source RPCs: " << source_server.rpc_requests() << "\n"
              << "The caller prompt never entered the LLM request or source-agent transport.\n";
    return 0;
}
