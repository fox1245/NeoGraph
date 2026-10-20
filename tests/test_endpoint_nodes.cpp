// The built-in SchemaProvider endpoint nodes (#531) in compiled graphs, against loopback peers.
#include <gtest/gtest.h>
#include "fixtures/endpoint_peer.h"
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>

#include <atomic>

using namespace neograph;
using namespace neograph::graph;
namespace wire = neograph::test::wire;
namespace endpoints = neograph::test::endpoints;
using namespace std::chrono_literals;

namespace {
// One endpoint node between start and end; every channel it names is declared.
json one_node(json node, std::vector<std::string> channels, int schema_version = 0) {
    auto declared = json::object();
    for (const auto& channel : channels) declared[channel] = {{"reducer", "overwrite"}};
    json graph = {{"name", "endpoint-node"}, {"channels", std::move(declared)},
                  {"nodes", {{"call", std::move(node)}}},
                  {"edges", json::array({json{{"from", "__start__"}, {"to", "call"}},
                                         json{{"from", "call"}, {"to", "__end__"}}})}};
    if (schema_version) graph["schema_version"] = schema_version;
    return graph;
}

NodeContext context_of(std::shared_ptr<Provider> provider) {
    NodeContext context;
    context.provider = std::move(provider);
    return context;
}

RunResult run_once(const json& graph, std::shared_ptr<Provider> provider, json input,
                   std::shared_ptr<CheckpointStore> store = nullptr, std::string thread = "endpoint-node") {
    auto engine = GraphEngine::compile(graph, context_of(std::move(provider)), std::move(store));
    RunConfig config;
    config.thread_id = std::move(thread);
    config.input = std::move(input);
    return engine->run(config);
}

json channel(const RunResult& result, const char* name) { return result.output["channels"][name]["value"]; }

json last_request(const wire::Peer& peer) {
    std::lock_guard lock(peer.state->mutex);
    return peer.state->requests.empty() ? json() : peer.state->requests.back();
}
std::string last_path(const wire::Peer& peer) {
    std::lock_guard lock(peer.state->mutex);
    return peer.state->paths.empty() ? std::string() : peer.state->paths.back();
}

// OpenRouter video jobs: a submit answers pending, polls answer from `polls` in order, and the content is fixed.
class VideoPeer {
public:
    explicit VideoPeer(std::vector<std::string> polls) : polls_(std::move(polls)) {
        server_.Post("/api/v1/videos", [this](const httplib::Request&, httplib::Response& response) {
            ++submits;
            response.set_content(R"({"id":"job-1","status":"pending"})", "application/json");
        });
        server_.Get(R"(/api/v1/videos/job-1)", [this](const httplib::Request&, httplib::Response& response) {
            const auto index = std::min<std::size_t>(polls_seen++, polls_.size() - 1);
            json body = {{"id", "job-1"}, {"status", polls_[index]}};
            if (polls_[index] == "completed")
                body["unsigned_urls"] = json::array({origin() + "/api/v1/videos/job-1/content?index=0"});
            response.set_content(body.dump(), "application/json");
        });
        server_.Get(R"(/api/v1/videos/job-1/content)", [this](const httplib::Request&, httplib::Response& response) {
            ++downloads;
            response.set_content("fixture-video-bytes", "video/mp4");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        worker_ = std::thread([this] { server_.listen_after_bind(); });
        for (int i = 0; i != 200 && !server_.is_running(); ++i) std::this_thread::sleep_for(5ms);
    }
    ~VideoPeer() { server_.stop(); worker_.join(); }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::atomic<int> submits{0}, downloads{0};
    std::atomic<std::size_t> polls_seen{0};
private:
    std::vector<std::string> polls_;
    httplib::Server server_;
    int port_ = 0;
    std::thread worker_;
};
}  // namespace

TEST(EndpointNodes, EmbedWritesOneVectorPerInputAndRecordsTheUsage) {
    wire::Peer peer(endpoints::embedding_body());
    auto result = run_once(one_node({{"type", "embed"}, {"model", "fixture-embed"}}, {"input", "embeddings"}),
                           endpoints::provider_at(peer.origin()), {{"input", "a graph of agents"}});
    EXPECT_EQ(channel(result, "embeddings"), json::parse("[[0.25,-0.5]]"));
    ASSERT_TRUE(result.usage.input_total);
    EXPECT_EQ(result.usage.input_total->value, 3u);
    EXPECT_EQ(last_path(peer), "/v1/embeddings");
    EXPECT_EQ(last_request(peer)["model"], "fixture-embed");
}

TEST(EndpointNodes, ChannelNamesAndOptionsComeFromTheConfig) {
    wire::Peer peer(endpoints::embedding_body());
    auto result = run_once(one_node({{"type", "embed"}, {"model", "fixture-embed"}, {"input", "texts"},
                                     {"output", "vectors"}, {"dimensions", 2}},
                                    {"texts", "vectors"}),
                           endpoints::provider_at(peer.origin()), {{"texts", json::array({"one"})}});
    EXPECT_EQ(channel(result, "vectors"), json::parse("[[0.25,-0.5]]"));
    EXPECT_EQ(last_request(peer)["dimensions"], 2);
}

TEST(EndpointNodes, ImageGenerateWritesProviderMedia) {
    wire::Peer peer(endpoints::image_body());
    auto result = run_once(one_node({{"type", "image_generate"}, {"model", "fixture-image"}, {"size", "1024x1024"}},
                                    {"prompt", "images"}),
                           endpoints::provider_at(peer.origin()), {{"prompt", "a square"}});
    const auto images = channel(result, "images");
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0]["type"], "media");
    EXPECT_EQ(images[0]["kind"], static_cast<int>(sp::MediaKind::Image));
    EXPECT_EQ(images[0]["data"], endpoints::png);
    EXPECT_EQ(last_path(peer), "/v1/images/generations");
    EXPECT_EQ(last_request(peer)["prompt"], "a square");
    EXPECT_EQ(last_request(peer)["size"], "1024x1024");
}

TEST(EndpointNodes, ImageEditSendsTheImagesOfItsReferencesChannel) {
    wire::Peer peer(endpoints::image_body());
    auto result = run_once(one_node({{"type", "image_edit"}, {"model", "fixture-image"}},
                                    {"prompt", "references", "images"}),
                           endpoints::provider_at(peer.origin()),
                           {{"prompt", "make it blue"}, {"references", "data:image/png;base64," + endpoints::png}});
    EXPECT_EQ(channel(result, "images").size(), 1u);
    EXPECT_EQ(last_path(peer), "/v1/images/edits");
    EXPECT_NE(last_request(peer).dump().find(endpoints::png), std::string::npos);
}

TEST(EndpointNodes, ModerateWritesAVerdictPerInput) {
    wire::Peer peer(json{{"id", "modr-1"}, {"model", "fixture-moderation"},
        {"results", json::array({{{"flagged", false}, {"categories", {{"violence", false}}},
                                  {"category_scores", {{"violence", 0.01}}}}})}}.dump());
    auto result = run_once(one_node({{"type", "moderate"}, {"model", "fixture-moderation"}}, {"input", "moderation"}),
                           endpoints::provider_at(peer.origin()), {{"input", "a cup of tea"}});
    const auto verdicts = channel(result, "moderation");
    ASSERT_EQ(verdicts.size(), 1u);
    EXPECT_EQ(verdicts[0]["flagged"], false);
    EXPECT_EQ(verdicts[0]["categories"]["violence"], false);
    EXPECT_EQ(last_path(peer), "/v1/moderations");
}

TEST(EndpointNodes, ACallThatReportsNoUsageKeepsTheRunsTotals) {
    wire::Peer peer(endpoints::embedding_body());
    peer.state->next_bodies.push_back(json{{"id", "modr-1"}, {"model", "fixture-moderation"},
        {"results", json::array({{{"flagged", false}, {"categories", json::object()},
                                  {"category_scores", json::object()}}})}}.dump());
    json graph = one_node({{"type", "embed"}, {"model", "fixture-embed"}}, {"input", "embeddings", "moderation"});
    graph["nodes"]["moderate"] = {{"type", "moderate"}, {"model", "fixture-moderation"}};
    graph["edges"] = json::array({json{{"from", "__start__"}, {"to", "call"}}, json{{"from", "call"}, {"to", "moderate"}},
                                  json{{"from", "moderate"}, {"to", "__end__"}}});
    auto result = run_once(graph, endpoints::provider_at(peer.origin()), {{"input", "tea"}});
    EXPECT_EQ(channel(result, "moderation").size(), 1u);
    ASSERT_TRUE(result.usage.input_total);
    EXPECT_EQ(result.usage.input_total->value, 3u);
}

TEST(EndpointNodes, ABudgetedRunRefusesAnEndpointNodeBeforeItSendsAnything) {
    wire::Peer peer(endpoints::embedding_body());
    auto engine = GraphEngine::compile(one_node({{"type", "embed"}, {"model", "fixture-embed"}}, {"input", "embeddings"}),
                                       context_of(endpoints::provider_at(peer.origin())));
    RunConfig config;
    config.thread_id = "budgeted";
    config.input = {{"input", "text"}};
    config.model_token_budget = 1000;
    EXPECT_ANY_THROW(engine->run(config));
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(EndpointNodes, AMissingModelOrInputIsRefused) {
    EXPECT_THROW(NodeFactory::instance().create("embed", "e", json{{"type", "embed"}}, NodeContext{}),
                 std::invalid_argument);
    EXPECT_THROW(NodeFactory::instance().create("speak", "s", json{{"type", "speak"}, {"model", "m"}}, NodeContext{}),
                 std::invalid_argument);
    EXPECT_THROW(NodeFactory::instance().create("embed", "e", json{{"type", "embed"}, {"model", "m"}, {"dimensions", 0}},
                                                NodeContext{}),
                 std::invalid_argument);
    wire::Peer peer(endpoints::embedding_body());
    EXPECT_ANY_THROW(run_once(one_node({{"type", "embed"}, {"model", "fixture-embed"}}, {"input", "embeddings"}),
                              endpoints::provider_at(peer.origin()), json::object()));
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(EndpointNodes, AStrictGraphRefusesAnUnknownConfigKey) {
    wire::Peer peer(endpoints::embedding_body());
    const auto graph = one_node({{"type", "embed"}, {"model", "fixture-embed"}, {"dimension", 2}},
                                {"input", "embeddings"}, 1);
    EXPECT_ANY_THROW(GraphEngine::compile(graph, context_of(endpoints::provider_at(peer.origin()))));
}

TEST(EndpointNodes, AVideoJobInterruptsUntilItFinishesAndIsSubmittedOnce) {
    VideoPeer peer({"in_progress", "completed"});
    auto provider = endpoints::provider_at(peer.origin(), true, {"https://openrouter.ai"});
    json graph = {{"name", "video"},
                  {"channels", {{"prompt", {{"reducer", "overwrite"}}}, {"job", {{"reducer", "overwrite"}}},
                                {"video", {{"reducer", "overwrite"}}}}},
                  {"nodes", {{"submit", {{"type", "job_submit"}, {"model", "fixture-video"}, {"duration", 4}}},
                             {"collect", {{"type", "job_collect"}}}}},
                  {"edges", json::array({json{{"from", "__start__"}, {"to", "submit"}},
                                         json{{"from", "submit"}, {"to", "collect"}},
                                         json{{"from", "collect"}, {"to", "__end__"}}})}};
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(graph, context_of(provider), store);
    RunConfig config;
    config.thread_id = "video";
    config.input = {{"prompt", "waves at dusk"}};
    auto paused = engine->run(config);
    ASSERT_TRUE(paused.interrupted);
    EXPECT_EQ(paused.interrupt_node, "collect");
    EXPECT_EQ(paused.interrupt_value["value"]["status"], "running");
    EXPECT_EQ(paused.interrupt_value["value"]["job"]["id"], "job-1");
    EXPECT_EQ(peer.submits.load(), 1);

    auto done = engine->resume(config);
    ASSERT_FALSE(done.interrupted);
    const auto video = channel(done, "video");
    ASSERT_EQ(video.size(), 1u);
    EXPECT_EQ(video[0]["kind"], static_cast<int>(sp::MediaKind::Video));
    EXPECT_EQ(peer.submits.load(), 1);
    EXPECT_EQ(peer.polls_seen.load(), 2u);
    EXPECT_EQ(peer.downloads.load(), 1);
}

TEST(EndpointNodes, AFailedVideoJobFailsTheCollectNode) {
    VideoPeer peer({"failed"});
    auto provider = endpoints::provider_at(peer.origin(), true, {"https://openrouter.ai"});
    const json handle = {{"endpoint", "videos"}, {"origin", peer.origin()}, {"id", "job-1"}, {"model", "fixture-video"}};
    EXPECT_ANY_THROW(run_once(one_node({{"type", "job_collect"}}, {"job", "video"}), provider, {{"job", handle}}));
    EXPECT_EQ(peer.downloads.load(), 0);
}
