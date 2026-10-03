#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/cancel.h>
#include "fixtures/typed_provider.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

using namespace neograph;
using namespace neograph::graph;
using namespace std::chrono_literals;
namespace {
constexpr std::size_t width = 3;
struct Observation {
    std::mutex mutex;
    std::condition_variable cv;
    std::array<bool, width> entered{}, completed{}, cancelled{};
};
class LiveWorker final : public GraphNode {
public:
    LiveWorker(std::string name, std::shared_ptr<Provider> provider, std::shared_ptr<Observation> observation, std::string model)
        : name_(std::move(name)), provider_(std::move(provider)), observation_(std::move(observation)), model_(std::move(model)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto index = input.state.get("i").get<std::size_t>();
        if (index >= width) throw std::invalid_argument("fanout index invalid");
        sp::chat::Request payload; payload.model = model_; payload.max_output_tokens = 400;
        payload.canonical_messages.push_back(test::message("Write a detailed 300-word essay about historical event #" + std::to_string(index), sp::Role::User));
        ProviderRequest request; request.payload = std::move(payload); request.mode = ProviderMode::Stream;
        request.cancel_token = input.ctx.cancel_token;
        {
            std::lock_guard lock(observation_->mutex); observation_->entered[index] = true; observation_->cv.notify_all();
        }
        const auto result = co_await provider_->invoke_async(std::move(request));
        if (const auto* failure = std::get_if<sp::Failure>(result.get()); failure && failure->error.kind == sp::ErrorKind::Cancelled) {
            std::lock_guard lock(observation_->mutex); observation_->cancelled[index] = true; observation_->cv.notify_all();
            throw CancelledException();
        }
        outcome_or_throw(result);
        { std::lock_guard lock(observation_->mutex); observation_->completed[index] = true; }
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::shared_ptr<Provider> provider_;
    std::shared_ptr<Observation> observation_;
    std::string model_;
};
class Dispatcher final : public GraphNode {
public:
    explicit Dispatcher(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput output;
        for (std::size_t i = 0; i < width; ++i) output.sends.push_back({"worker", json{{"i", i}}});
        co_return output;
    }
    std::string get_name() const override { return name_; }
private: std::string name_;
};
}

TEST(LiveLLMCancelFanout, MultiSendBranchesAbortAtSocketLayer) {
    const auto* enabled = std::getenv("NEOGRAPH_LIVE_LLM");
    if (!enabled || std::string_view(enabled) != "1") GTEST_SKIP() << "Explicit live-model opt-in required";
    const auto* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) GTEST_SKIP() << "Live-model credential not configured";
    const auto* configured_model = std::getenv("OPENAI_MODEL");
    const auto model = std::string(configured_model ? configured_model : "gpt-4o-mini");
    const auto* configured_origin = std::getenv("OPENAI_API_BASE");
    const auto origin = std::string(configured_origin ? configured_origin : "https://api.openai.com");
    sp::runtime::Options options; options.api_key = key; options.default_timeout = 20s;
    options.retry_tokens = 0; options.retry_tokens_per_second = 0;
    std::shared_ptr<Provider> provider = llm::SchemaProvider::create(test::descriptor("openai.chat", origin), options);
    auto observation = std::make_shared<Observation>();
    NodeFactory::instance().register_type("typed_live_dispatcher", [](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<Dispatcher>(name);
    });
    NodeFactory::instance().register_type("typed_live_worker", [provider, observation, model](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<LiveWorker>(name, provider, observation, model);
    });
    const json graph = {{"name", "typed_live_fanout"}, {"channels", {{"i", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"dispatcher", {{"type", "typed_live_dispatcher"}}}, {"worker", {{"type", "typed_live_worker"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "dispatcher"}}})}};
    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig config; config.thread_id = "typed-live-fanout"; config.cancel_token = std::make_shared<CancelToken>();
    auto run = std::async(std::launch::async, [engine = std::move(engine), config] {
        try { engine->run(config); } catch (const CancelledException&) {}
    });
    bool entered;
    { std::unique_lock lock(observation->mutex);
      entered = observation->cv.wait_for(lock, 12s, [&] { return std::all_of(observation->entered.begin(), observation->entered.end(), [](bool v) { return v; }); }); }
    if (entered) std::this_thread::sleep_for(700ms);
    config.cancel_token->cancel();
    const auto status = run.wait_for(3s);
    run.get();
    ASSERT_TRUE(entered); EXPECT_EQ(status, std::future_status::ready);
    std::lock_guard lock(observation->mutex);
    for (std::size_t i = 0; i < width; ++i) {
        EXPECT_FALSE(observation->completed[i]) << "branch " << i;
        EXPECT_TRUE(observation->cancelled[i]) << "branch " << i;
    }
}
