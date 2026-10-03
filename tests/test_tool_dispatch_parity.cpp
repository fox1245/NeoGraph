// Tool dispatch concurrency and resource-admission parity.
//
// 1. Legacy synchronous Tool::execute implementations run on the bounded
// blocking executor, so distinct tools overlap instead of pinning the graph
// loop.
// 2. The conservative default policy is keyed-exclusive: repeated calls to
// the same tool name serialize until a host explicitly declares a wider
// policy.
// 3. Native asynchronous tools retain their overlap, and Agent shares the
// exact dispatch boundary with ToolDispatchNode.
//
// Timing assertions use generous bounds (a 3x300ms serial run vs a ~300ms
// concurrent one) so they discriminate the two regimes without depending on
// scheduler microseconds.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/llm/agent.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include "fixtures/typed_provider.h"

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

using namespace neograph;
using namespace neograph::graph;
using namespace std::chrono_literals;

namespace {

constexpr auto kToolDelay = 300ms;

// Sleeps on a blocking worker thread. Implements only execute() — the common
// legacy shape that must no longer pin the graph event loop.
class SyncSleepTool : public Tool {
public:
    explicit SyncSleepTool(std::string name) : name_(std::move(name)) {}

    ChatTool get_definition() const override {
        return {name_, "sleeps", json{{"type", "object"}, {"properties", json::object()}}};
    }
    std::string execute(const json& /*args*/) override {
        std::this_thread::sleep_for(kToolDelay);
        return "slept";
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Suspends on a timer — genuinely async, the shape an I/O-bound tool has.
class AsyncSleepTool : public AsyncTool {
public:
    explicit AsyncSleepTool(std::string name) : name_(std::move(name)) {}

    ChatTool get_definition() const override {
        return {name_, "sleeps", json{{"type", "object"}, {"properties", json::object()}}};
    }
    asio::awaitable<std::string> execute_async(const json& /*args*/) override {
        asio::steady_timer t(co_await asio::this_coro::executor);
        t.expires_after(kToolDelay);
        co_await t.async_wait(asio::use_awaitable);
        co_return "slept";
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

struct ToolCancelProbe {
    std::atomic<bool> entered{false};
    std::atomic<bool> cancelled{false};
};

class ContextAwareCancelTool : public Tool, public ContextualAsyncTool {
public:
    explicit ContextAwareCancelTool(std::shared_ptr<ToolCancelProbe> probe)
        : probe_(std::move(probe)) {}

    ChatTool get_definition() const override {
        return {"cancel", "waits for cancellation",
                json{{"type", "object"}, {"properties", json::object()}}};
    }

    std::string execute(const json&) override {
        throw std::logic_error("context-aware async overload was not used");
    }

    asio::awaitable<std::string> execute_async(
        const json&, ToolExecutionContext execution) override {
        if (!execution.cancel_token) {
            throw std::logic_error("missing tool cancellation token");
        }
        probe_->entered.store(true, std::memory_order_release);
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        while (!execution.cancel_token->is_cancelled()) {
            timer.expires_after(1ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
        probe_->cancelled.store(true, std::memory_order_release);
        throw CancelledException("context-aware tool observed cancellation");
    }

    std::string get_name() const override { return "cancel"; }

private:
    std::shared_ptr<ToolCancelProbe> probe_;
};

// An assistant message asking for all three tools in one turn.
ChatMessage assistant_calling(const std::vector<std::string>& names) {
    ChatMessage m;
    m.role = "assistant";
    for (std::size_t i = 0; i < names.size(); ++i) {
        ToolCall tc;
        tc.id        = "call_" + std::to_string(i);
        tc.name      = names[i];
        tc.arguments = "{}";
        m.tool_calls.push_back(tc);
    }
    return m;
}

sp::Message typed_assistant_calling(const std::vector<std::string>& names) {
    sp::Message message;
    message.role = sp::Role::Assistant;
    for (std::size_t index = 0; index < names.size(); ++index) {
        message.parts.emplace_back(sp::ToolCall{
            "call_" + std::to_string(index), names[index],
            sp::ToolCallKind::ClientExecuted, test::document("{}")});
    }
    return message;
}

void seed(GraphState& state, const std::vector<ChatMessage>& msgs) {
    state.init_channel("messages", ReducerType::APPEND,
                       ReducerRegistry::instance().get("append"), json::array());
    json arr = json::array();
    for (const auto& m : msgs) {
        json j;
        to_json(j, m);
        arr.push_back(j);
    }
    state.write("messages", arr);
}

template <typename F>
std::chrono::milliseconds timed(F&& f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
}

NodeOutput drive(GraphNode& node, const GraphState& state) {
    RunContext ctx;
    return neograph::async::run_sync(node.run(NodeInput{state, ctx, nullptr}));
}

}  // namespace

// Distinct legacy synchronous tools overlap after the default async bridge
// offloads each call to the bounded blocking pool.
TEST(ToolDispatchParity, SyncToolsOverlapOnToolNode) {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<SyncSleepTool>("a"));
    tools.push_back(std::make_unique<SyncSleepTool>("b"));
    tools.push_back(std::make_unique<SyncSleepTool>("c"));
    NodeContext ctx;
    ctx.tools = ToolSet(std::move(tools));
    ToolDispatchNode node("tools", ctx);

    GraphState state;
    seed(state, {assistant_calling({"a", "b", "c"})});

    auto elapsed = timed([&] { drive(node, state); });
    EXPECT_LT(elapsed, 2 * kToolDelay)
        << "distinct synchronous tools did not leave the graph event loop";
}

// Claim 2a. Async tools DO overlap on ToolNode. This is the behavior Agent lacks.
TEST(ToolDispatchParity, AsyncToolsOverlapOnToolNode) {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<AsyncSleepTool>("a"));
    tools.push_back(std::make_unique<AsyncSleepTool>("b"));
    tools.push_back(std::make_unique<AsyncSleepTool>("c"));
    NodeContext ctx;
    ctx.tools = ToolSet(std::move(tools));
    ToolDispatchNode node("tools", ctx);

    GraphState state;
    seed(state, {assistant_calling({"a", "b", "c"})});

    auto elapsed = timed([&] { drive(node, state); });
    EXPECT_LT(elapsed, 2 * kToolDelay)
        << "ToolNode did not overlap async tools";
}

TEST(ToolDispatchParity, CancellationReachesContextAwareTool) {
    auto probe = std::make_shared<ToolCancelProbe>();
    NodeContext node_context;
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<ContextAwareCancelTool>(probe));
    node_context.tools = ToolSet(std::move(tools));
    ToolDispatchNode node("tools", node_context);

    GraphState state;
    seed(state, {assistant_calling({"cancel"})});
    auto token = std::make_shared<CancelToken>();
    std::promise<std::exception_ptr> completion;
    auto done = completion.get_future();
    std::thread runner([&] {
        try {
            RunContext context;
            context.cancel_token = token;
            (void)neograph::async::run_sync(node.run(NodeInput{state, context, nullptr}));
            completion.set_value(nullptr);
        } catch (...) {
            completion.set_value(std::current_exception());
        }
    });

    for (int i = 0; i < 200 && !probe->entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(probe->entered.load(std::memory_order_acquire));
    token->cancel();
    const auto status = done.wait_for(500ms);
    EXPECT_EQ(status, std::future_status::ready);
    if (status != std::future_status::ready) {
        runner.join();
        return;
    }
    const auto error = done.get();
    runner.join();

    EXPECT_TRUE(probe->cancelled.load(std::memory_order_acquire));
    ASSERT_NE(error, nullptr);
    EXPECT_THROW(std::rethrow_exception(error), CancelledException);
}

TEST(ToolDispatchParity, AgentOverlapsAsyncTools) {
    // A provider that asks for all three tools once, then answers.
    auto turns = std::make_shared<int>(0);
    auto provider = std::make_shared<test::LocalProvider>(
        [turns](ProviderRequest, const PreparedProviderRequest&,
                const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            if ((*turns)++ == 0)
                co_return test::success(std::vector<sp::Message>{typed_assistant_calling({"a", "b", "c"})});
            co_return test::success("done");
        }, "three-call");

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<AsyncSleepTool>("a"));
    tools.push_back(std::make_unique<AsyncSleepTool>("b"));
    tools.push_back(std::make_unique<AsyncSleepTool>("c"));

    neograph::llm::Agent agent(provider, std::move(tools), "", "fixture-model");

    std::vector<sp::Message> messages{test::message("go", sp::Role::User)};
    auto elapsed = timed([&] { EXPECT_EQ(test::text(agent.run(messages)), "done"); });
    EXPECT_LT(elapsed, 2 * kToolDelay)
        << "Agent ran the tool calls serially; ToolNode overlaps them (#87)";
}

TEST(ToolDispatchParity, AgentRunStreamHonorsConfiguredDeadlineAndTypedEvents) {
    struct Probe {
        std::optional<std::chrono::steady_clock::time_point> deadline;
        ProviderMode mode = ProviderMode::Collect;
    };
    auto probe = std::make_shared<Probe>();
    auto provider = std::make_shared<test::LocalProvider>(
        [probe](ProviderRequest request, const PreparedProviderRequest& prepared,
                const test::LocalProvider::EventCallback& observer) -> asio::awaitable<sp::runtime::Result> {
            probe->deadline = request.options.deadline;
            probe->mode = prepared.mode();
            if (observer) observer(sp::PartDelta{sp::LocalId{1}, {sp::PartKind::Text, "streamed"}});
            co_return test::success("streamed");
        }, "phase-probe");
    neograph::llm::Agent agent(provider, std::vector<std::unique_ptr<Tool>>{}, "", "fixture-model");
    agent.set_tool_detection_timeout_seconds(7);
    std::vector<sp::Message> messages{test::message("hello", sp::Role::User)};
    std::string streamed;
    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(test::text(agent.run_stream(messages, [&](const sp::Event& event) {
        if (const auto* delta = std::get_if<sp::PartDelta>(&event))
            streamed.append(delta->payload.bytes);
    })), "streamed");
    const auto after = std::chrono::steady_clock::now();
    EXPECT_EQ(streamed, "streamed");
    EXPECT_EQ(probe->mode, ProviderMode::Stream);
    ASSERT_TRUE(probe->deadline);
    EXPECT_GE(*probe->deadline, before + 7s);
    EXPECT_LE(*probe->deadline, after + 7s);
}

TEST(ToolDispatchParity, AgentFailureRetainsTypedDeadlineAndPartialHistoryWithoutAnotherTurn) {
    sp::Failure failure;
    failure.error.kind = sp::ErrorKind::DeadlineExceeded;
    failure.error.retry_safety = sp::RetrySafety::OutputObserved;
    failure.error.safe_message = "fixture deadline exceeded";
    failure.partial.messages = {test::message("unfinished")};
    failure.partial.messages.front().parts.emplace_back(sp::Refusal{"policy", "refused"});
    failure.partial.usage = test::usage(3, std::nullopt, std::nullopt, sp::UsageStage::Partial);
    const auto expected = std::make_shared<const sp::Outcome>(std::move(failure));
    auto calls = std::make_shared<int>(0);
    auto provider = std::make_shared<test::LocalProvider>(
        [expected, calls](ProviderRequest, const PreparedProviderRequest&,
                          const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            ++*calls;
            co_return expected;
        }, "deadline");
    neograph::llm::Agent agent(provider, std::vector<std::unique_ptr<Tool>>{}, "", "fixture-model");
    std::vector<sp::Message> messages{test::message("hello", sp::Role::User)};
    try {
        (void)agent.run_stream(messages, [](const sp::Event&) {});
        FAIL() << "a typed provider failure must terminate the Agent turn";
    } catch (const ProviderFailure& error) {
        EXPECT_EQ(error.outcome(), expected);
    }
    EXPECT_EQ(*calls, 1);
    ASSERT_EQ(agent.outcomes().size(), 1u);
    EXPECT_EQ(agent.outcomes().front(), expected);
    EXPECT_EQ(std::get<sp::Failure>(*agent.outcomes().front()).error.kind, sp::ErrorKind::DeadlineExceeded);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(std::get<sp::Text>(messages.back().parts.at(0)).value, "unfinished");
    EXPECT_EQ(std::get<sp::Refusal>(messages.back().parts.at(1)).raw_code, "refused");
    ASSERT_TRUE(agent.usage().input_total);
    EXPECT_EQ(agent.usage().input_total->value, 3u);
    EXPECT_FALSE(agent.usage().output_total);
}

// How far does the win actually scale? Twenty distinct legacy tools should
// stay near one tool's latency, while repeated calls to one tool stay
// keyed-exclusive. Both reference regimes are measured on the same binary.
TEST(ToolDispatchParity, ScalingToTwentyTools) {
    constexpr int kN = 20;

    std::vector<std::string> names;
    for (int i = 0; i < kN; ++i) names.push_back("t" + std::to_string(i));

    // Serial reference: repeated calls to one resource key.
    NodeContext serial_ctx;
    std::vector<std::unique_ptr<Tool>> serial_tools;
    serial_tools.push_back(std::make_unique<SyncSleepTool>("shared"));
    serial_ctx.tools = ToolSet(std::move(serial_tools));
    ToolDispatchNode serial_node("tools", serial_ctx);
    GraphState serial_state;
    seed(serial_state, {assistant_calling(std::vector<std::string>(kN, "shared"))});
    auto serial = timed([&] { drive(serial_node, serial_state); });

    // Concurrent: N distinct synchronous tools through the same dispatch.
    std::vector<std::unique_ptr<Tool>> sync_tools;
    for (const auto& n : names) {
        sync_tools.push_back(std::make_unique<SyncSleepTool>(n));
    }
    NodeContext sync_ctx;
    sync_ctx.tools = ToolSet(std::move(sync_tools));
    ToolDispatchNode sync_node("tools", sync_ctx);
    GraphState sync_state;
    seed(sync_state, {assistant_calling(names)});
    auto concurrent = timed([&] { drive(sync_node, sync_state); });

    const double ratio = static_cast<double>(serial.count()) /
                         static_cast<double>(std::max<long long>(1, concurrent.count()));
    std::cout << "[ MEASURE  ] N=" << kN << " tools x " << kToolDelay.count() << "ms: "
              << "serial " << serial.count() << "ms, "
              << "concurrent " << concurrent.count() << "ms, "
              << "speedup " << ratio << "x\n";

    // The shared pool is deliberately bounded between 2 and 16 workers, so
    // absolute wall time depends on the host. The same-process ratio must
    // still prove overlap against the keyed-exclusive reference path.
    EXPECT_GT(ratio, 1.5)
        << "synchronous tools did not overlap at N=" << kN;
    EXPECT_GE(serial, kN * kToolDelay - 200ms)
        << "same-key tool calls bypassed keyed-exclusive admission";
}
