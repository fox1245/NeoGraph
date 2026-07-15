#include <gtest/gtest.h>
#include <neograph/graph/deep_research_graph.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

class ScriptedFailureProvider : public Provider {
public:
    ScriptedFailureProvider(std::string message,
                            std::shared_ptr<CancelToken> expected_token,
                            bool cancel = false)
        : message_(std::move(message))
        , expected_token_(std::move(expected_token))
        , cancel_(cancel) {}

    asio::awaitable<ChatCompletion>
    invoke(const CompletionParams& params, StreamCallback) override {
        const int call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call == 1) {
            saw_cancel_token.store(params.cancel_token == expected_token_,
                                   std::memory_order_relaxed);
            if (cancel_) throw CancelledException(message_);
            throw std::runtime_error(message_);
        }
        throw std::logic_error("unexpected downstream provider call");
        co_return ChatCompletion{};
    }

    std::string get_name() const override { return "scripted-failure"; }

    std::atomic<int> calls{0};
    std::atomic<bool> saw_cancel_token{false};

private:
    std::string message_;
    std::shared_ptr<CancelToken> expected_token_;
    bool cancel_;
};

struct EventLog {
    void push(const GraphEvent& event) {
        std::lock_guard lock(mu);
        events.push_back(event);
    }

    int count(GraphEvent::Type type, const std::string& node) const {
        std::lock_guard lock(mu);
        int result = 0;
        for (const auto& event : events) {
            if (event.type == type && event.node_name == node) ++result;
        }
        return result;
    }

    std::string error_for(const std::string& node) const {
        std::lock_guard lock(mu);
        for (const auto& event : events) {
            if (event.type == GraphEvent::Type::ERROR
                && event.node_name == node) {
                return event.data.value("error", std::string{});
            }
        }
        return {};
    }

    mutable std::mutex mu;
    std::vector<GraphEvent> events;
};

void expect_provider_failure(bool enable_clarification,
                             const std::string& failing_node,
                             const std::string& message) {
    auto token = std::make_shared<CancelToken>();
    auto provider = std::make_shared<ScriptedFailureProvider>(message, token);

    DeepResearchConfig graph_config;
    graph_config.enable_clarification = enable_clarification;
    auto engine = create_deep_research_graph(provider, {}, graph_config);

    EventLog log;
    RunConfig run_config;
    run_config.input = {{"user_query", "Investigate provider failures"}};
    run_config.cancel_token = token;
    run_config.stream_mode = StreamMode::EVENTS;

    try {
        engine->run_stream(run_config, [&](const GraphEvent& event) {
            log.push(event);
        });
        FAIL() << "expected provider failure";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(error.what(), message);
    } catch (const std::exception& error) {
        FAIL() << "expected runtime_error, got: " << error.what();
    }

    EXPECT_EQ(provider->calls.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(provider->saw_cancel_token.load(std::memory_order_relaxed));
    EXPECT_EQ(log.count(GraphEvent::Type::ERROR, failing_node), 1);
    EXPECT_EQ(log.error_for(failing_node), message);
    EXPECT_EQ(log.count(GraphEvent::Type::NODE_END, failing_node), 0);
}

void expect_cancellation(bool enable_clarification) {
    auto token = std::make_shared<CancelToken>();
    auto provider = std::make_shared<ScriptedFailureProvider>(
        "provider call", token, true);

    DeepResearchConfig graph_config;
    graph_config.enable_clarification = enable_clarification;
    auto engine = create_deep_research_graph(provider, {}, graph_config);

    RunConfig run_config;
    run_config.input = {{"user_query", "Investigate cancellation"}};
    run_config.cancel_token = token;

    EXPECT_THROW(engine->run(run_config), CancelledException);
    EXPECT_EQ(provider->calls.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(provider->saw_cancel_token.load(std::memory_order_relaxed));
}

} // namespace

TEST(DeepResearchProviderFailure, BriefFailurePropagatesAndStopsGraph) {
    expect_provider_failure(false, "brief", "brief provider failed");
}

TEST(DeepResearchProviderFailure, ClarifyFailurePropagatesAndStopsGraph) {
    expect_provider_failure(true, "clarify", "clarify provider failed");
}

TEST(DeepResearchProviderFailure, BriefCancellationPropagates) {
    expect_cancellation(false);
}

TEST(DeepResearchProviderFailure, ClarifyCancellationPropagates) {
    expect_cancellation(true);
}
