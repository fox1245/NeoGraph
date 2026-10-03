// Intent classification forwards typed text deltas to graph token events and
// chooses the same route in collect and stream modes.
#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/async/run_sync.h>
#include "fixtures/typed_provider.h"

#include <atomic>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

class TokenStreamingProvider : public test::LocalProvider {
public:
    explicit TokenStreamingProvider(std::vector<std::string> tokens,
                                    std::string final_output)
        : LocalProvider([tokens = std::move(tokens), final_output = std::move(final_output)](
              ProviderRequest, const PreparedProviderRequest& prepared,
              const EventCallback& on_event) -> asio::awaitable<sp::runtime::Result> {
            if (prepared.mode() == ProviderMode::Stream && on_event) {
                on_event(sp::Begin{"classification"});
                on_event(sp::MessageBegin{{0}, {}, sp::Role::Assistant});
                on_event(sp::PartBegin{{0}, {0}, sp::PartKind::Text});
                for (const auto& token : tokens)
                    on_event(sp::PartDelta{{0}, {sp::PartKind::Text, token}});
                on_event(sp::PartSeal{{0}, {}});
                on_event(sp::MessageSeal{{0}});
            }
            co_return test::success(final_output);
        }, "token-streamer") {}
};

} // namespace

TEST(IntentClassifierStreaming, EmitsLLMTokenEventsDuringClassification) {
    // Provider dribbles three tokens that together form "shopping".
    auto provider = std::make_shared<TokenStreamingProvider>(
        std::vector<std::string>{"shop", "p", "ing"},
        "shopping");

    NodeContext ctx;
    ctx.provider = provider;
    ctx.model = "test-model";
    IntentClassifierNode node(
        "router", ctx,
        /*prompt=*/"",
        /*valid_routes=*/std::vector<std::string>{"shopping", "support"});

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND,
                       ReducerRegistry::instance().get("append"));
    state.write("messages", json::array({
        json{{"role", "user"}, {"content", "where can I buy X?"}}
    }));

    // Collect every GraphEvent the node emits.
    std::vector<std::pair<GraphEvent::Type, std::string>> events;
    auto cb = [&events](const GraphEvent& e) {
        events.emplace_back(e.type, e.data.is_string()
                                        ? e.data.get<std::string>()
                                        : e.data.dump());
    };

    GraphStreamCallback gscb = cb;
    RunContext run_ctx;
    auto out = neograph::async::run_sync(
        node.run(NodeInput{state, run_ctx, &gscb}));

    // Three tokens → three LLM_TOKEN events tagged "router".
    int token_events = 0;
    std::string reconstructed;
    for (const auto& [type, payload] : events) {
        if (type == GraphEvent::Type::LLM_TOKEN) {
            ++token_events;
            reconstructed += payload;
        }
    }
    EXPECT_EQ(3, token_events)
        << "IntentClassifier streaming path did not forward tokens";
    EXPECT_EQ("shopping", reconstructed);

    // Routing still lands on the full classification result.
    ASSERT_EQ(1u, out.writes.size());
    EXPECT_EQ("__route__", out.writes[0].channel);
    EXPECT_EQ("shopping", out.writes[0].value.get<std::string>());
}

TEST(IntentClassifierStreaming, StreamingMatchesNonStreamingRouting) {
    auto provider = std::make_shared<TokenStreamingProvider>(
        std::vector<std::string>{"sup", "port"},
        "support");

    NodeContext ctx;
    ctx.provider = provider;
    ctx.model = "test-model";
    IntentClassifierNode node(
        "router", ctx,
        /*prompt=*/"",
        /*valid_routes=*/std::vector<std::string>{"shopping", "support"});

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND,
                       ReducerRegistry::instance().get("append"));
    state.write("messages", json::array({
        json{{"role", "user"}, {"content", "my order is missing"}}
    }));

    RunContext run_ctx;
    auto sync_out = neograph::async::run_sync(
        node.run(NodeInput{state, run_ctx, nullptr}));
    GraphStreamCallback empty_cb = [](const GraphEvent&) {};
    auto stream_out = neograph::async::run_sync(
        node.run(NodeInput{state, run_ctx, &empty_cb}));

    ASSERT_EQ(sync_out.writes.size(), stream_out.writes.size());
    ASSERT_EQ(1u, sync_out.writes.size());
    EXPECT_EQ(sync_out.writes[0].channel, stream_out.writes[0].channel);
    EXPECT_EQ(sync_out.writes[0].value.get<std::string>(),
              stream_out.writes[0].value.get<std::string>());
}
