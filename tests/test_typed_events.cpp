#include <neograph/graph/types.h>

#include <gtest/gtest.h>

#include <variant>

using namespace neograph;
using namespace neograph::graph;

TEST(TypedGraphEvent, ConvertsStandardEventPayloads) {
    auto started = to_typed_event(
        GraphEvent{GraphEvent::Type::NODE_START, "worker", json{{"retry_attempt", 2}}});
    ASSERT_TRUE(std::holds_alternative<NodeStartEvent>(started));
    EXPECT_EQ(std::get<NodeStartEvent>(started).node_name, "worker");
    EXPECT_EQ(std::get<NodeStartEvent>(started).retry_attempt, 2);

    auto ended = to_typed_event(GraphEvent{GraphEvent::Type::NODE_END, "worker",
                                           json{{"command_goto", "review"}, {"sends", 2}}});
    ASSERT_TRUE(std::holds_alternative<NodeEndEvent>(ended));
    EXPECT_EQ(std::get<NodeEndEvent>(ended).command_goto, "review");
    EXPECT_EQ(std::get<NodeEndEvent>(ended).send_count, 2u);

    auto token = to_typed_event(GraphEvent{GraphEvent::Type::LLM_TOKEN, "model", json("hello")});
    ASSERT_TRUE(std::holds_alternative<LlmTokenEvent>(token));
    EXPECT_EQ(std::get<LlmTokenEvent>(token).token, "hello");

    auto write = to_typed_event(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, "worker",
                                           json{{"channel", "answer"}, {"value", 42}}});
    ASSERT_TRUE(std::holds_alternative<ChannelWriteEvent>(write));
    EXPECT_EQ(std::get<ChannelWriteEvent>(write).channel, "answer");
    EXPECT_EQ(std::get<ChannelWriteEvent>(write).value.get<int>(), 42);

    auto error = to_typed_event(
        GraphEvent{GraphEvent::Type::ERROR, "worker", json{{"error", "failed"}, {"attempts", 3}}});
    ASSERT_TRUE(std::holds_alternative<ErrorEvent>(error));
    EXPECT_EQ(std::get<ErrorEvent>(error).message, "failed");
    EXPECT_EQ(std::get<ErrorEvent>(error).data["attempts"].get<int>(), 3);

    auto interrupt =
        to_typed_event(GraphEvent{GraphEvent::Type::INTERRUPT, "approval",
                                  json{{"phase", "before"}, {"checkpoint_id", "cp-1"}}});
    ASSERT_TRUE(std::holds_alternative<InterruptEvent>(interrupt));
    EXPECT_EQ(std::get<InterruptEvent>(interrupt).phase, "before");
    EXPECT_EQ(std::get<InterruptEvent>(interrupt).checkpoint_id, "cp-1");
}

TEST(TypedGraphEvent, ConvertsStateRoutingAndSendSentinels) {
    auto snapshot = to_typed_event(
        GraphEvent{GraphEvent::Type::CHANNEL_WRITE, "__state__", json{{"global_version", 4}}});
    ASSERT_TRUE(std::holds_alternative<StateSnapshotEvent>(snapshot));
    EXPECT_EQ(std::get<StateSnapshotEvent>(snapshot).state["global_version"].get<int>(), 4);

    auto routing = to_typed_event(
        GraphEvent{GraphEvent::Type::NODE_START, "__routing__",
                   json{{"next_nodes", json::array({"left", "right"})}, {"step", 5}}});
    ASSERT_TRUE(std::holds_alternative<RoutingEvent>(routing));
    EXPECT_EQ(std::get<RoutingEvent>(routing).next_nodes,
              (std::vector<std::string>{"left", "right"}));
    EXPECT_EQ(std::get<RoutingEvent>(routing).step, 5);

    auto command = to_typed_event(
        GraphEvent{GraphEvent::Type::NODE_START, "__routing__", json{{"command_goto", "review"}}});
    ASSERT_TRUE(std::holds_alternative<RoutingEvent>(command));
    EXPECT_EQ(std::get<RoutingEvent>(command).command_goto, "review");

    json sends = json::array();
    sends.push_back(json{{"target", "worker"}, {"input", {{"task", 7}}}});
    auto dispatch = to_typed_event(
        GraphEvent{GraphEvent::Type::NODE_START, "__send__", json{{"sends", sends}}});
    ASSERT_TRUE(std::holds_alternative<SendDispatchEvent>(dispatch));
    ASSERT_EQ(std::get<SendDispatchEvent>(dispatch).sends.size(), 1u);
    EXPECT_EQ(std::get<SendDispatchEvent>(dispatch).sends[0].target_node, "worker");
    EXPECT_EQ(std::get<SendDispatchEvent>(dispatch).sends[0].input["task"].get<int>(), 7);

    auto empty_dispatch = to_typed_event(
        GraphEvent{GraphEvent::Type::NODE_START, "__send__", json{{"sends", json::array()}}});
    ASSERT_TRUE(std::holds_alternative<SendDispatchEvent>(empty_dispatch));
    EXPECT_TRUE(std::get<SendDispatchEvent>(empty_dispatch).sends.empty());

    json sends_without_input = json::array();
    sends_without_input.push_back(json{{"target", "worker"}});
    auto missing_input = to_typed_event(
        GraphEvent{GraphEvent::Type::NODE_START, "__send__", json{{"sends", sends_without_input}}});
    ASSERT_TRUE(std::holds_alternative<SendDispatchEvent>(missing_input));
    ASSERT_EQ(std::get<SendDispatchEvent>(missing_input).sends.size(), 1u);
    EXPECT_TRUE(std::get<SendDispatchEvent>(missing_input).sends[0].input.is_null());
}

TEST(TypedGraphEvent, MalformedPayloadFallsBackToRawEvent) {
    GraphEvent malformed{GraphEvent::Type::CHANNEL_WRITE, "worker", json{{"channel", 12}}};
    auto       typed = to_typed_event(malformed);

    ASSERT_TRUE(std::holds_alternative<RawGraphEvent>(typed));
    EXPECT_EQ(std::get<RawGraphEvent>(typed).event.node_name, "worker");
    EXPECT_EQ(std::get<RawGraphEvent>(typed).event.data["channel"].get<int>(), 12);

    json malformed_sends = json::array();
    malformed_sends.push_back(json{{"target", 12}});
    auto malformed_dispatch = to_typed_event(
        GraphEvent{GraphEvent::Type::NODE_START, "__send__", json{{"sends", malformed_sends}}});
    EXPECT_TRUE(std::holds_alternative<RawGraphEvent>(malformed_dispatch));
}

TEST(TypedGraphEvent, AdapterPreservesExistingCallbackSurface) {
    bool saw_token = false;
    auto callback  = adapt_typed_stream([&](const TypedGraphEvent& event) {
        if (const auto* token = std::get_if<LlmTokenEvent>(&event)) {
            saw_token = token->token == "chunk";
        }
    });

    callback(GraphEvent{GraphEvent::Type::LLM_TOKEN, "model", json("chunk")});
    EXPECT_TRUE(saw_token);
    EXPECT_FALSE(static_cast<bool>(adapt_typed_stream({})));
}
