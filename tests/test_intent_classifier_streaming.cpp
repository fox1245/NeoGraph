// IntentClassifierNode used to override only execute() — never
// execute_stream(). Callers running a graph in streaming mode
// therefore saw LLM_TOKEN events from every LLMCallNode but none
// from IntentClassifier, even though both are plain LLM calls. The
// fix adds a proper execute_stream override that routes through the
// provider's streaming API and emits tokens.
//
// This test pins the new behaviour with a scripted provider that
// delivers three tokens to the StreamCallback; the test asserts
// every token surfaced as an LLM_TOKEN event on the GraphStreamCallback.
//
// PR 9a: this test now drives ``IntentClassifierNode::run`` with
// and without a streaming sink to compare both code paths.
#include <neograph/api.h>  // NEOGRAPH_PUSH_IGNORE_DEPRECATED
NEOGRAPH_PUSH_IGNORE_DEPRECATED

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/async/run_sync.h>

#include <atomic>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

class TokenStreamingProvider : public Provider {
public:
    explicit TokenStreamingProvider(std::vector<std::string> tokens,
                                    std::string final_output)
        : tokens_(std::move(tokens)), final_output_(std::move(final_output)) {}

    ChatCompletion complete(const CompletionParams&) override {
        ChatCompletion c;
        c.message.role = "assistant";
        c.message.content = final_output_;
        return c;
    }

    ChatCompletion complete_stream(const CompletionParams&,
                                   const StreamCallback& on_chunk) override {
        for (const auto& t : tokens_) {
            if (on_chunk) on_chunk(t);
        }
        ChatCompletion c;
        c.message.role = "assistant";
        c.message.content = final_output_;
        return c;
    }

    std::string get_name() const override { return "token-streamer"; }

private:
    std::vector<std::string> tokens_;
    std::string              final_output_;
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

NEOGRAPH_POP_IGNORE_DEPRECATED
