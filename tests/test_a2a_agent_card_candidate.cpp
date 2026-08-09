#include <neograph/a2a/agent_card_candidate.h>
#include <neograph/a2a/client.h>
#include <neograph/a2a/server.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>

#include <gtest/gtest.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace neograph;
using namespace neograph::a2a;

namespace {

struct CandidateCardServer {
    httplib::Server  server;
    std::thread      thread;
    int              port = 0;
    std::atomic<int> card_requests{0};
    std::atomic<int> rpc_requests{0};
    json             card = json::parse(R"json(
{
  "name": "source-hello-world",
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

    CandidateCardServer() {
        server.Get("/.well-known/agent-card.json",
                   [this](const httplib::Request&, httplib::Response& response) {
                       card_requests.fetch_add(1, std::memory_order_relaxed);
                       response.status = 200;
                       response.set_content(card.dump(), "application/json");
                   });
        server.Post("/", [this](const httplib::Request&, httplib::Response& response) {
            rpc_requests.fetch_add(1, std::memory_order_relaxed);
            response.status = 500;
            response.set_content("unexpected RPC", "text/plain");
        });

        port   = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_NE(port, 0);
        EXPECT_TRUE(server.is_running());
    }

    ~CandidateCardServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }
};

AgentCardSourceProvenance source_provenance() {
    return {
        {},
        "https://github.com/a2aproject/a2a-samples/blob/0123456789abcdef0123456789abcdef01234567/"
        "samples/python/agents/helloworld/__main__.py",
        "0123456789abcdef0123456789abcdef01234567",
        "Apache-2.0",
    };
}

CollectedAgentCard collect_local_card(CandidateCardServer& server) {
    AgentCardCollectionPolicy policy;
    policy.allow_loopback_http = true;
    return AgentCardCollector(policy).collect(server.origin(), source_provenance());
}

std::shared_ptr<neograph::graph::GraphEngine> build_copy_ninja_local_engine(
    std::shared_ptr<const CopyNinjaHarness> harness, json node = json::object()) {
    auto& factory = neograph::graph::NodeFactory::instance();
    factory.register_type(
        "a2a_copy_ninja_candidate",
        [harness = std::move(harness)](const std::string& name, const neograph::json&,
                                       const neograph::graph::NodeContext&) {
            return std::make_unique<CopyNinjaNode>(name, harness);
        },
        json::parse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        json::parse(R"({"reads":["prompt"],"writes":["response"]})"));

    node["type"]          = "a2a_copy_ninja_candidate";
    const json definition = {
        {"schema_version", 1},
        {"name", "a2a-copy-ninja-candidate"},
        {"channels",
         {
             {"prompt", {{"reducer", "overwrite"}}},
             {"response", {{"reducer", "overwrite"}}},
         }},
        {"nodes", {{"copy_ninja", std::move(node)}}},
        {"edges", json::array({
                      json{{"from", "__start__"}, {"to", "copy_ninja"}},
                      json{{"from", "copy_ninja"}, {"to", "__end__"}},
                  })},
    };
    return std::shared_ptr<neograph::graph::GraphEngine>(
        neograph::graph::GraphEngine::compile(definition, neograph::graph::NodeContext{}));
}

TEST(AgentCardCandidate, CollectsOnlyWellKnownCardAndCompilesSafeDescriptor) {
    CandidateCardServer server;
    const auto          collected = collect_local_card(server);

    ASSERT_EQ(server.card_requests.load(), 1);
    ASSERT_EQ(server.rpc_requests.load(), 0);
    EXPECT_EQ(collected.provenance.discovery_url, server.origin());
    EXPECT_EQ(collected.card_sha256.rfind("sha256:", 0), 0U);
    EXPECT_EQ(collected.raw_card.at("url"), "https://source-agent.example.invalid/rpc");

    const auto candidate  = AgentCardCandidateCompiler::compile(collected);
    const auto descriptor = candidate.descriptor.dump();
    EXPECT_EQ(candidate.descriptor.at("state"), "unadmitted");
    EXPECT_EQ(candidate.descriptor.at("behavioral_equivalence"), "unproven");
    EXPECT_TRUE(candidate.descriptor.at("authority").at("requires_new_admission").get<bool>());
    EXPECT_EQ(candidate.descriptor.at("declared_contract").at("skill_ids"),
              json::array({"echo-bot"}));
    EXPECT_FALSE(descriptor.find("UNTRUSTED_CARD_TEXT") != std::string::npos);
    EXPECT_FALSE(descriptor.find("UNTRUSTED_SKILL_NAME") != std::string::npos);
    EXPECT_FALSE(descriptor.find("source-agent.example.invalid") != std::string::npos);
    EXPECT_EQ(server.rpc_requests.load(), 0);
}

TEST(AgentCardCandidate, MaterializesOnlyDigestPinnedVerifiedLocalTemplate) {
    CandidateCardServer server;
    const auto          candidate = AgentCardCandidateCompiler::compile(collect_local_card(server));

    CopyNinjaBehavioralProfile profile;
    profile.source_card_sha256 = candidate.source_card_sha256;
    profile.template_id        = "copy-ninja.hello-world-echo.v1";
    profile.development_probes = {{"Ada", "Hello, World! I have received your request (Ada)"}};

    const auto harness = materialize_copy_ninja(candidate, profile);
    EXPECT_EQ(harness.respond("Lin"), "Hello, World! I have received your request (Lin)");

    const auto local_card = harness.agent_card("http://127.0.0.1:4890");
    ASSERT_TRUE(local_card.raw.is_object());
    EXPECT_EQ(local_card.raw.at("name"), candidate.id);
    EXPECT_EQ(local_card.raw.at("url"), "http://127.0.0.1:4890");
    EXPECT_EQ(local_card.raw.at("skills").at(0).at("id"), "echo-bot");
    const auto local_serialized = local_card.raw.dump();
    EXPECT_FALSE(local_serialized.find("source-agent.example.invalid") != std::string::npos);
    EXPECT_FALSE(local_serialized.find("UNTRUSTED_CARD_TEXT") != std::string::npos);
    EXPECT_FALSE(local_serialized.find("source_url") != std::string::npos);
    EXPECT_EQ(server.rpc_requests.load(), 0);
}

TEST(AgentCardCandidate, ServesVerifiedLocalHarnessWithoutSourceDispatch) {
    CandidateCardServer source_server;
    const auto candidate = AgentCardCandidateCompiler::compile(collect_local_card(source_server));

    CopyNinjaBehavioralProfile profile;
    profile.source_card_sha256 = candidate.source_card_sha256;
    profile.template_id        = "copy-ninja.hello-world-echo.v1";
    profile.development_probes = {
        {"Ada", "Hello, World! I have received your request (Ada)"},
    };
    auto harness =
        std::make_shared<CopyNinjaHarness>(materialize_copy_ninja(candidate, std::move(profile)));

    A2AServer local_server(build_copy_ninja_local_engine(harness),
                           harness->agent_card("http://127.0.0.1:0"));
    ASSERT_TRUE(local_server.start_async("127.0.0.1", 0));

    A2AClient  local_client("http://127.0.0.1:" + std::to_string(local_server.port()));
    const auto local_card = local_client.fetch_agent_card();
    ASSERT_TRUE(local_card.raw.is_object());
    EXPECT_EQ(local_card.name, candidate.id);
    EXPECT_EQ(local_card.raw.at("url"), "http://127.0.0.1:0");
    EXPECT_EQ(local_card.raw.at("skills").at(0).at("id"), "echo-bot");
    EXPECT_EQ(local_card.raw.dump().find("source-agent.example.invalid"), std::string::npos);

    const auto task = local_client.send_message_sync("Grace");
    local_server.stop();

    ASSERT_EQ(task.status.state, TaskState::Completed);
    ASSERT_FALSE(task.history.empty());
    ASSERT_FALSE(task.history.back().parts.empty());
    EXPECT_EQ(task.history.back().parts.front().text,
              "Hello, World! I have received your request (Grace)");
    EXPECT_EQ(source_server.rpc_requests.load(), 0);
}

TEST(AgentCardCandidate, RejectsUnreviewedLocalNodeConfiguration) {
    CandidateCardServer source_server;
    const auto candidate = AgentCardCandidateCompiler::compile(collect_local_card(source_server));

    CopyNinjaBehavioralProfile profile;
    profile.source_card_sha256 = candidate.source_card_sha256;
    profile.template_id        = "copy-ninja.hello-world-echo.v1";
    profile.development_probes = {
        {"Ada", "Hello, World! I have received your request (Ada)"},
    };
    const auto harness =
        std::make_shared<CopyNinjaHarness>(materialize_copy_ninja(candidate, std::move(profile)));

    EXPECT_THROW(build_copy_ninja_local_engine(harness, {{"untrusted_config", true}}),
                 std::exception);
    EXPECT_EQ(source_server.rpc_requests.load(), 0);
}
TEST(AgentCardCandidate, RejectsUnpinnedBehavioralProfile) {
    CandidateCardServer server;
    const auto          candidate = AgentCardCandidateCompiler::compile(collect_local_card(server));

    CopyNinjaBehavioralProfile profile;
    profile.source_card_sha256 = "sha256:wrong";
    profile.template_id        = "copy-ninja.hello-world-echo.v1";
    profile.development_probes = {{"Ada", "Hello, World! I have received your request (Ada)"}};

    EXPECT_THROW(materialize_copy_ninja(candidate, std::move(profile)), std::invalid_argument);
    EXPECT_EQ(server.rpc_requests.load(), 0);
}

TEST(AgentCardCandidate, RejectsNullLocalGraphBinding) {
    EXPECT_THROW(
        CopyNinjaNode("a2a_copy_ninja_candidate", std::shared_ptr<const CopyNinjaHarness>{}),
        std::invalid_argument);
}

TEST(AgentCardCandidate, RejectsNonTextCardAndUnapprovedPlainHttp) {
    CandidateCardServer server;

    AgentCardCollector secure_collector;
    EXPECT_THROW(secure_collector.collect(server.origin(), source_provenance()),
                 std::invalid_argument);
    EXPECT_EQ(server.card_requests.load(), 0);

    server.card["defaultInputModes"] = json::array({"image/png"});
    AgentCardCollectionPolicy policy;
    policy.allow_loopback_http = true;
    EXPECT_THROW(AgentCardCollector(policy).collect(server.origin(), source_provenance()),
                 std::invalid_argument);
    EXPECT_EQ(server.card_requests.load(), 1);
    EXPECT_EQ(server.rpc_requests.load(), 0);
}

}  // namespace
