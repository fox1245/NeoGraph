// Built-in node behavior at the owned typed provider boundary.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/loader.h>
#include <neograph/async/run_sync.h>
#include <neograph/controlled_provider.h>
#include <neograph/graph/executor.h>
#include <neograph/runtime_turn_assembler.h>
#include "fixtures/typed_provider.h"

using namespace neograph;
using namespace neograph::graph;

namespace {
// Drive a built-in node's awaitable ``run`` to completion on a
// fresh single-threaded io_context — the right shape for unit
// tests that exercise just one node out of an engine context.
NodeOutput drive_run(GraphNode& node, const GraphState& state) {
    RunContext ctx;
    return neograph::async::run_sync(
        node.run(NodeInput{state, ctx, nullptr}));
}

class NodeSettlementFailure final : public std::runtime_error {
public:
    NodeSettlementFailure() : std::runtime_error("node outcome storage unavailable") {}
};

class NodeSettlementFaultStore final : public ProviderDispatchReceiptStore,
                                       public ProviderDispatchOutcomeStore {
public:
    explicit NodeSettlementFaultStore(std::exception_ptr failure) : failure_(std::move(failure)) {}
    ProviderDispatchReceiptPutResult persist(const ProviderDispatchReceipt& receipt) override {
        return journal.persist(receipt);
    }
    ProviderDispatchReceiptPutResult persist(std::string_view owner,
                                             const ProviderDispatchReceipt& receipt) override {
        return journal.persist(owner, receipt);
    }
    ProviderDispatchOutcomePutResult settle(std::string_view,
                                            const ProviderDispatchOutcomeReceipt&) override {
        ++settlements;
        std::rethrow_exception(failure_);
    }
    std::optional<ProviderDispatchOutcomeReceipt> outcome(
        std::string_view owner, std::string_view dispatch) const override {
        return journal.outcome(owner, dispatch);
    }
    InMemoryProviderDispatchReceiptStore journal;
    int settlements = 0;
private:
    std::exception_ptr failure_;
};

ContextAssemblyReceipt node_assembly(const PreparedProviderRequest& request) {
    ContextEpochData epoch_data;
    epoch_data.run_id = "graph-settlement";
    epoch_data.sequence = 1;
    epoch_data.raw_window_digest = "sha256:" + std::string(64, 'a');
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    ContextAssemblyReceiptData data;
    data.context_epoch_id = epoch.id();
    data.normalized_request_digest = RuntimeTurnAssembler::normalized_request_digest(request);
    data.message_window_digest = "sha256:" + std::string(64, 'b');
    return ContextAssemblyReceipt::create(std::move(data), epoch, {});
}
}  // namespace

static ReducerFn overwrite_fn() { return ReducerRegistry::instance().get("overwrite"); }
static ReducerFn append_fn()    { return ReducerRegistry::instance().get("append"); }

class MockProvider : public test::LocalProvider {
    explicit MockProvider(std::shared_ptr<sp::Message> response)
        : LocalProvider([response](ProviderRequest, const PreparedProviderRequest&,
                                  const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            co_return test::success(std::vector<sp::Message>{*response});
        }, "mock"), response_(std::move(response)), next_response(*response_) {}
    std::shared_ptr<sp::Message> response_;
public:
    MockProvider() : MockProvider(std::make_shared<sp::Message>()) {}
    sp::Message& next_response;
};

// ── Mock Tool ──

class MockTool : public Tool {
public:
    ChatTool get_definition() const override {
        return {"mock_tool", "A mock tool", json{{"type", "object"}, {"properties", json::object()}}};
    }
    std::string execute(const json& /*args*/) override {
        return "mock_result";
    }
    std::string get_name() const override { return "mock_tool"; }
};

// ── LLMCallNode ──

TEST(NodeTest, LLMCallNodeExecute) {
    auto provider = std::make_shared<MockProvider>();
    provider->next_response = test::message("Hello from LLM");

    NodeContext ctx;
    ctx.provider = provider;
    ctx.model = "fixture-model";

    LLMCallNode node("llm", ctx);

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    json user_msg;
    to_json(user_msg, ChatMessage{"user", "Hi"});
    state.write("messages", json::array({user_msg}));

    auto out = drive_run(node, state);
    ASSERT_EQ(out.writes.size(), 1);
    EXPECT_EQ(out.writes[0].channel, "messages");

    // The written value should contain the assistant response
    auto msgs = out.writes[0].value;
    ASSERT_TRUE(msgs.is_array());
    ASSERT_EQ(msgs.size(), 1);
    EXPECT_EQ(msgs[0]["role"], "assistant");
    EXPECT_EQ(msgs[0]["content"], "Hello from LLM");
}

TEST(NodeTest, SettlementFailureRetainsActualOutcomeWithoutNodeRedispatch) {
    const auto cause = std::make_exception_ptr(NodeSettlementFailure{});
    auto store = std::make_shared<NodeSettlementFaultStore>(cause);
    auto effects = std::make_shared<int>(0);
    sp::Completion completion;
    completion.messages = {test::message("delivered")};
    completion.messages.front().parts.emplace_back(sp::InvalidToolCall{
        "partial", "read", sp::ToolCallKind::ClientExecuted, "{\"path\":", sp::InvalidReason::Truncated});
    completion.usage = test::usage(7, std::nullopt, std::nullopt);
    completion.stop = {sp::StopKind::MaxTokens, "length"};
    completion.attempt.request_may_have_left = true;
    completion.attempt.response_head_seen = true;
    completion.attempt.attempts = 2;
    completion.attempt.prior_usage_unknown = true;
    completion.wire_envelope = test::document(R"({"id":"delivered","unknown":{"value":null}})");
    completion.raw_events.push_back({"vendor.unknown", test::document(R"({"opaque":[1,null,3]})")});
    const auto expected = std::make_shared<const sp::Outcome>(std::move(completion));
    auto transport = std::make_shared<test::LocalProvider>(
        [effects, expected](ProviderRequest, const PreparedProviderRequest&,
                            const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            ++*effects;
            co_return expected;
        });
    // Use the actual ControlledProvider settlement path, not an exception
    // manufactured in node code. Only terminal storage is fault-injected.
    auto provider = std::make_shared<test::LocalProvider>(
        [transport, store](ProviderRequest request, const PreparedProviderRequest&,
                           const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            auto prepared = transport->prepare(std::move(request));
            auto assembly = node_assembly(prepared);
            ControlledProvider controlled(transport, store, "sha256:" + std::string(64, 'f'));
            co_return co_await controlled.dispatch_prepared_async(
                "owner", "graph-effect", assembly, std::move(prepared));
        });
    NodeContext context;
    context.provider = provider;
    context.model = "fixture-model";
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    nodes.emplace("llm", std::make_unique<LLMCallNode>("llm", context));
    const std::vector<ChannelDef> channels;
    RetryPolicy policy;
    policy.max_retries = 3;
    policy.initial_delay_ms = 1;
    NodeExecutor executor(nodes, channels, [policy](const std::string&) { return policy; });
    GraphState state;
    const auto original_messages = json::array({{{"role", "user"}, {"content", "question"}}});
    state.init_channel("messages", ReducerType::APPEND, append_fn(), original_messages);
    RunContext run;
    run.usage = std::make_shared<UsageAccumulator>();
    run.provider_outcomes = std::make_shared<ProviderOutcomes>();
    const auto terminal_observer_cause = std::make_exception_ptr(
        std::range_error("terminal error observer unavailable"));
    GraphStreamCallback observer = [terminal_observer_cause](const GraphEvent& event) {
        if (event.type == GraphEvent::Type::ERROR)
            std::rethrow_exception(terminal_observer_cause);
    };
    try {
        (void)neograph::async::run_sync(executor.execute_node_with_retry_async(
            "llm", state, observer, StreamMode::EVENTS, run));
        FAIL() << "post-effect storage failure must remain a node failure";
    } catch (const NodeExecutionError& error) {
        EXPECT_EQ(error.attempts(), 1);
        EXPECT_THROW(std::rethrow_if_nested(error), std::range_error);
        try {
            std::rethrow_exception(error.cause());
            FAIL() << "expected owned provider outcome failure";
        } catch (const ProviderOutcomeError& outcome_error) {
            EXPECT_EQ(outcome_error.outcome(), expected);
            EXPECT_EQ(outcome_error.cause(), cause);
            EXPECT_THROW(std::rethrow_exception(outcome_error.cause()), NodeSettlementFailure);
        }
    }
    EXPECT_EQ(*effects, 1);
    EXPECT_EQ(store->settlements, 1);
    EXPECT_EQ(store->journal.state("owner", "graph-effect"), ProviderDispatchState::AdmittedPending);
    const auto outcomes = run.provider_outcomes->snapshot();
    ASSERT_EQ(outcomes.size(), 1u);
    EXPECT_EQ(outcomes.front(), expected);
    const auto report = run.usage->snapshot();
    ASSERT_TRUE(report.input_total);
    EXPECT_EQ(report.input_total->value, 7u);
    EXPECT_FALSE(report.output_total);
    EXPECT_FALSE(report.total);
    EXPECT_EQ(report.stage, sp::UsageStage::Final);
    EXPECT_EQ(state.get("messages"), original_messages);
}

TEST(NodeTest, SdkPartialFailureStopsOuterRetryAndRetainsEvidence) {
    sp::Failure failure;
    failure.error.kind = sp::ErrorKind::Truncated;
    failure.error.retry_class = sp::RetryClass::Transient;
    failure.error.retry_safety = sp::RetrySafety::OutputObserved;
    failure.error.safe_message = "response ended during a tool call";
    failure.error.attempt.request_may_have_left = true;
    failure.error.attempt.response_head_seen = true;
    failure.error.attempt.attempts = 2;
    failure.error.attempt.transport_internal_resends = 1;
    failure.error.attempt.prior_usage_unknown = true;
    failure.partial.messages = {test::message("partial text")};
    failure.partial.messages.front().parts.emplace_back(sp::InvalidToolCall{
        "partial", "read", sp::ToolCallKind::ClientExecuted, "{\"path\":", sp::InvalidReason::Truncated});
    failure.partial.usage = test::usage(0, 2, std::nullopt, sp::UsageStage::Partial);
    failure.partial.wire_envelope = test::document(R"({"id":"partial-response","metadata":{"unrecognized":true}})");
    failure.partial.raw_events.push_back({"unknown.event", test::document(R"({"partial":[null,{"value":9}]})")});
    const auto expected = std::make_shared<const sp::Outcome>(std::move(failure));
    auto effects = std::make_shared<int>(0);
    auto provider = std::make_shared<test::LocalProvider>(
        [expected, effects](ProviderRequest, const PreparedProviderRequest&,
                            const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            ++*effects;
            co_return expected;
        });
    NodeContext context;
    context.provider = provider;
    context.model = "fixture-model";
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    nodes.emplace("llm", std::make_unique<LLMCallNode>("llm", context));
    const std::vector<ChannelDef> channels;
    RetryPolicy policy;
    policy.max_retries = 3;
    policy.initial_delay_ms = 1;
    NodeExecutor executor(nodes, channels, [policy](const std::string&) { return policy; });
    GraphState state;
    const auto original_messages = json::array({{{"role", "user"}, {"content", "question"}}});
    state.init_channel("messages", ReducerType::APPEND, append_fn(), original_messages);
    RunContext run;
    run.usage = std::make_shared<UsageAccumulator>();
    run.provider_outcomes = std::make_shared<ProviderOutcomes>();
    try {
        (void)neograph::async::run_sync(executor.execute_node_with_retry_async(
            "llm", state, nullptr, StreamMode::ALL, run));
        FAIL() << "an SDK partial failure must not become a successful node";
    } catch (const NodeExecutionError& error) {
        EXPECT_EQ(error.attempts(), 1);
        try {
            std::rethrow_exception(error.cause());
            FAIL() << "expected the original typed SDK failure";
        } catch (const ProviderFailure& sdk_failure) {
            EXPECT_EQ(sdk_failure.outcome(), expected);
        }
    }
    EXPECT_EQ(*effects, 1);
    const auto outcomes = run.provider_outcomes->snapshot();
    ASSERT_EQ(outcomes.size(), 1u);
    EXPECT_EQ(outcomes.front(), expected);
    const auto report = run.usage->snapshot();
    ASSERT_TRUE(report.input_total);
    EXPECT_EQ(report.input_total->value, 0u);
    ASSERT_TRUE(report.output_total);
    EXPECT_EQ(report.output_total->value, 2u);
    EXPECT_FALSE(report.total);
    EXPECT_EQ(report.stage, sp::UsageStage::Partial);
    EXPECT_EQ(state.get("messages"), original_messages);
}

// ── System-message contract (issue #93) ──
//
// Assert the admitted outgoing request's system-message shape.

class RecordingProvider : public test::LocalProvider {
    explicit RecordingProvider(std::shared_ptr<json> request)
        : LocalProvider([](ProviderRequest, const PreparedProviderRequest&,
                           const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            co_return test::success("ok");
        }, "recording"), request_(std::move(request)), last_request(*request_) {}
    std::shared_ptr<json> request_;
public:
    RecordingProvider() : RecordingProvider(std::make_shared<json>()) {}
    PreparedProviderRequest prepare(ProviderRequest request) override {
        auto prepared = LocalProvider::prepare(std::move(request));
        if (prepared.valid()) *request_ = json::parse(prepared.encoded_body());
        return prepared;
    }
    json& last_request;
};

namespace {
std::size_t count_system(const json& msgs) {
    std::size_t count = 0;
    for (const auto& message : msgs)
        if (message.at("role") == "system") ++count;
    return count;
}

// Seed a messages channel with the given messages. GraphState holds a
// shared_mutex, so it is neither copyable nor movable — fill in place.
void seed_messages(GraphState& state, const std::vector<ChatMessage>& msgs) {
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());
    json arr = json::array();
    for (const auto& m : msgs) {
        json j;
        to_json(j, m);
        arr.push_back(j);
    }
    state.write("messages", arr);
}
}  // namespace

// Existing behavior — must stay green. A node with instructions and no system
// message in state prepends exactly one.
TEST(NodeTest, AddsSystemMessageWhenAbsent) {
    auto provider = std::make_shared<RecordingProvider>();
    NodeContext ctx;
    ctx.provider     = provider;
    ctx.model = "fixture-model";
    ctx.instructions = "You are a helpful assistant.";

    LLMCallNode node("llm", ctx);
    GraphState state;
    seed_messages(state, {{"user", "Hi"}});
    drive_run(node, state);

    ASSERT_EQ(count_system(provider->last_request.at("messages")), 1u);
}

// Existing behavior — must stay green. A system message that already matches
// the instructions is not duplicated.
TEST(NodeTest, DoesNotDuplicateMatchingSystemMessage) {
    auto provider = std::make_shared<RecordingProvider>();
    NodeContext ctx;
    ctx.provider     = provider;
    ctx.model = "fixture-model";
    ctx.instructions = "You are a helpful assistant.";

    LLMCallNode node("llm", ctx);
    GraphState state;
    seed_messages(state, {{"system", "You are a helpful assistant."}, {"user", "Hi"}});
    drive_run(node, state);

    EXPECT_EQ(count_system(provider->last_request.at("messages")), 1u);
}

// RED (issue #93). The guard tests role AND content, so a system message whose
// content differs from instructions_ fails the check and a SECOND system
// message is prepended. Two system messages go out: malformed for Anthropic
// (single system prompt), undefined for OpenAI-family.
TEST(NodeTest, DoesNotPrependSecondSystemMessage) {
    auto provider = std::make_shared<RecordingProvider>();
    NodeContext ctx;
    ctx.provider     = provider;
    ctx.model = "fixture-model";
    ctx.instructions = "Instructions B";

    LLMCallNode node("llm", ctx);
    GraphState state;
    seed_messages(state, {{"system", "System prompt A"}, {"user", "Hi"}});
    drive_run(node, state);

    EXPECT_EQ(count_system(provider->last_request.at("messages")), 1u)
        << "a second system message was prepended in front of the existing one";
}

// ── ToolDispatchNode ──

TEST(NodeTest, ToolDispatchExecutes) {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MockTool>());

    NodeContext ctx;
    ctx.tools = ToolSet(std::move(tools));

    ToolDispatchNode node("tools", ctx);
    ctx.tools = ToolSet{};  // The standalone node retains its own tool owner.

    // Create state with an assistant message containing tool_calls
    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = {ToolCall{"tc_1", "mock_tool", "{}"}};

    json msg_json;
    to_json(msg_json, assistant_msg);
    state.write("messages", json::array({msg_json}));

    auto out = drive_run(node, state);
    ASSERT_EQ(out.writes.size(), 1);

    auto tool_msgs = out.writes[0].value;
    ASSERT_TRUE(tool_msgs.is_array());
    ASSERT_EQ(tool_msgs.size(), 1);
    EXPECT_EQ(tool_msgs[0]["role"], "tool");
    EXPECT_EQ(tool_msgs[0]["content"], "mock_result");
}

TEST(NodeTest, ToolDispatchNoToolCalls) {
    NodeContext ctx;
    ToolDispatchNode node("tools", ctx);

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    // No assistant message with tool_calls
    json user_msg;
    to_json(user_msg, ChatMessage{"user", "Hi"});
    state.write("messages", json::array({user_msg}));

    auto out = drive_run(node, state);
    EXPECT_TRUE(out.writes.empty());
}

TEST(NodeTest, ToolDispatchToolNotFound) {
    NodeContext ctx;
    // No tools registered
    ToolDispatchNode node("tools", ctx);

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = {ToolCall{"tc_1", "nonexistent", "{}"}};

    json msg_json;
    to_json(msg_json, assistant_msg);
    state.write("messages", json::array({msg_json}));

    auto out = drive_run(node, state);
    ASSERT_EQ(out.writes.size(), 1);
    // Should contain error message
    auto content = out.writes[0].value[0]["content"].get<std::string>();
    EXPECT_TRUE(content.find("error") != std::string::npos ||
                content.find("not found") != std::string::npos ||
                content.find("Tool not found") != std::string::npos);
}
