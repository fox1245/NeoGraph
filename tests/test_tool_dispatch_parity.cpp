// Tool dispatch: what actually runs concurrently, and on which path (issue #87).
//
// Two claims are under test here, and the point of the file is to hold them to
// numbers rather than to reasoning:
//
//   1. A *sync* tool — one that implements only execute() — never overlaps,
//      even on the "parallel" ToolNode path. Tool::execute_async's default body
//      is `co_return execute(arguments);`, which runs to completion before the
//      coroutine's first suspension, so co_spawn'ing three of them into a
//      parallel_group still runs them end to end. This matters far beyond
//      performance: it means a stateful sync tool cannot race, and unifying the
//      dispatch paths cannot introduce a data race into one.
//
//   2. An *async* tool — one that overrides execute_async and actually suspends
//      — does overlap on ToolNode, and does NOT on Agent, which is the defect.
//
// Timing assertions use generous bounds (a 3x300ms serial run vs a ~300ms
// concurrent one) so they discriminate the two regimes without being flaky.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/llm/agent.h>
#include <neograph/async/run_sync.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace neograph;
using namespace neograph::graph;
using namespace std::chrono_literals;

namespace {

constexpr auto kToolDelay = 300ms;

// Sleeps on the calling thread. Implements only execute() — the common shape.
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

// Claim 1. Sync tools do NOT overlap on ToolNode, despite the parallel_group.
// This is the load-bearing compatibility fact: a stateful sync tool is never
// re-entered, so unifying Agent onto this dispatch cannot race one.
TEST(ToolDispatchParity, SyncToolsDoNotOverlapOnToolNode) {
    SyncSleepTool a("a"), b("b"), c("c");
    NodeContext ctx;
    ctx.tools = {&a, &b, &c};
    ToolDispatchNode node("tools", ctx);

    GraphState state;
    seed(state, {assistant_calling({"a", "b", "c"})});

    auto elapsed = timed([&] { drive(node, state); });
    EXPECT_GE(elapsed, 3 * kToolDelay - 50ms)
        << "sync tools appear to overlap — the no-race assumption behind #87 is wrong";
}

// Claim 2a. Async tools DO overlap on ToolNode. This is the behavior Agent lacks.
TEST(ToolDispatchParity, AsyncToolsOverlapOnToolNode) {
    AsyncSleepTool a("a"), b("b"), c("c");
    NodeContext ctx;
    ctx.tools = {&a, &b, &c};
    ToolDispatchNode node("tools", ctx);

    GraphState state;
    seed(state, {assistant_calling({"a", "b", "c"})});

    auto elapsed = timed([&] { drive(node, state); });
    EXPECT_LT(elapsed, 2 * kToolDelay)
        << "ToolNode did not overlap async tools";
}

// Claim 2b. RED (#87). The same three async tools through Agent run serially,
// because Agent loops over the calls and invokes the *sync* execute() on each.
TEST(ToolDispatchParity, AgentOverlapsAsyncTools) {
    // A provider that asks for all three tools once, then answers.
    class ThreeCallProvider : public Provider {
    public:
        int turns = 0;
        ChatCompletion complete(const CompletionParams& /*p*/) override {
            ChatCompletion comp;
            comp.message = (turns++ == 0) ? assistant_calling({"a", "b", "c"})
                                          : ChatMessage{"assistant", "done"};
            return comp;
        }
        ChatCompletion complete_stream(const CompletionParams& p,
                                       const StreamCallback& /*cb*/) override {
            return complete(p);
        }
        std::string get_name() const override { return "three-call"; }
    };

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<AsyncSleepTool>("a"));
    tools.push_back(std::make_unique<AsyncSleepTool>("b"));
    tools.push_back(std::make_unique<AsyncSleepTool>("c"));

    neograph::llm::Agent agent(std::make_shared<ThreeCallProvider>(), std::move(tools));

    std::vector<ChatMessage> messages{{"user", "go"}};
    auto elapsed = timed([&] { agent.run(messages); });
    EXPECT_LT(elapsed, 2 * kToolDelay)
        << "Agent ran the tool calls serially; ToolNode overlaps them (#87)";
}

// How far does the win actually scale? Three tools gave 3x, but that says
// nothing about twenty — a single-threaded io_context drives these coroutines,
// so anything that does not genuinely suspend serializes on that one thread no
// matter how many calls are in flight.
//
// The serial reference is measured, not assumed: sync tools still run one at a
// time (SyncToolsDoNotOverlapOnToolNode), so N sync tools on this same binary
// and this same machine IS what the old serial Agent cost. Both regimes are
// timed side by side and the ratio is printed.
TEST(ToolDispatchParity, ScalingToTwentyTools) {
    constexpr int kN = 20;

    std::vector<std::string> names;
    for (int i = 0; i < kN; ++i) names.push_back("t" + std::to_string(i));

    // Serial reference: N sync tools through the same dispatch.
    std::vector<std::unique_ptr<SyncSleepTool>> sync_owned;
    std::vector<Tool*> sync_tools;
    for (const auto& n : names) {
        sync_owned.push_back(std::make_unique<SyncSleepTool>(n));
        sync_tools.push_back(sync_owned.back().get());
    }
    NodeContext sync_ctx;
    sync_ctx.tools = sync_tools;
    ToolDispatchNode sync_node("tools", sync_ctx);
    GraphState sync_state;
    seed(sync_state, {assistant_calling(names)});
    auto serial = timed([&] { drive(sync_node, sync_state); });

    // Concurrent: N async tools through the same dispatch.
    std::vector<std::unique_ptr<AsyncSleepTool>> async_owned;
    std::vector<Tool*> async_tools;
    for (const auto& n : names) {
        async_owned.push_back(std::make_unique<AsyncSleepTool>(n));
        async_tools.push_back(async_owned.back().get());
    }
    NodeContext async_ctx;
    async_ctx.tools = async_tools;
    ToolDispatchNode async_node("tools", async_ctx);
    GraphState async_state;
    seed(async_state, {assistant_calling(names)});
    auto concurrent = timed([&] { drive(async_node, async_state); });

    const double ratio = static_cast<double>(serial.count()) /
                         static_cast<double>(std::max<long long>(1, concurrent.count()));
    std::cout << "[ MEASURE  ] N=" << kN << " tools x " << kToolDelay.count() << "ms: "
              << "serial " << serial.count() << "ms, "
              << "concurrent " << concurrent.count() << "ms, "
              << "speedup " << ratio << "x\n";

    // The claim under test is that concurrency holds at N=20 — i.e. the wall
    // clock stays near one tool's latency instead of growing with N.
    EXPECT_LT(concurrent, 2 * kToolDelay)
        << "concurrency collapsed at N=" << kN;
    EXPECT_GE(serial, kN * kToolDelay - 200ms)
        << "the serial reference did not actually serialize";
}
