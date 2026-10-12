// The one-time "fan-out ... executing serially" notice (stderr) says the
// branches of a fan-out run one after another on a single thread because the
// engine has no worker pool. It must not appear when branches overlap on that
// thread because they suspend, such as coroutine nodes waiting on I/O: those
// really run at the same time, and a worker pool would not change that.

#include <gtest/gtest.h>
#include <neograph/neograph.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace neograph;
using namespace neograph::graph;
using namespace std::chrono_literals;

namespace {

constexpr const char* kNotice = "executing serially";

// How many branches were inside run() at once, at most.
struct Overlap {
    std::atomic<int> inside{0};
    std::atomic<int> peak{0};
    void enter() {
        const int now = ++inside;
        int seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
    }
    void leave() { --inside; }
};

Overlap& overlap() {
    static Overlap value;
    return value;
}

// A node whose body is plain computation: it runs to the end without
// suspending, so branches of this kind cannot overlap on one thread.
class ComputeNode final : public SyncGraphNode {
public:
    explicit ComputeNode(std::string name) : SyncGraphNode(std::move(name)) {}

protected:
    NodeOutput run_sync(NodeInput) override {
        overlap().enter();
        std::this_thread::sleep_for(5ms);
        overlap().leave();
        return NodeOutput{{ChannelWrite{"out", json::array({1})}}};
    }
};

// A coroutine node that waits on a timer, as a node waiting on a socket does:
// while it waits, the thread runs the other branches.
class WaitingNode final : public GraphNode {
public:
    explicit WaitingNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        overlap().enter();
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(60ms);
        co_await timer.async_wait(asio::use_awaitable);
        overlap().leave();
        co_return NodeOutput{{ChannelWrite{"out", json::array({1})}}};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Fans out to the `target` node type with `width` Sends.
class SplitNode final : public GraphNode {
public:
    SplitNode(std::string name, int width) : name_(std::move(name)), width_(width) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        for (int i = 0; i < width_; ++i) out.sends.push_back(Send{"leaf", json{{"i", i}}});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    int width_;
};

void register_types() {
    static std::once_flag once;
    std::call_once(once, [] {
        NodeFactory::instance().register_type(
            "fanout_notice.compute",
            [](const std::string& name, const json&, const NodeContext&)
                -> std::unique_ptr<GraphNode> { return std::make_unique<ComputeNode>(name); });
        NodeFactory::instance().register_type(
            "fanout_notice.waiting",
            [](const std::string& name, const json&, const NodeContext&)
                -> std::unique_ptr<GraphNode> { return std::make_unique<WaitingNode>(name); });
        NodeFactory::instance().register_type(
            "fanout_notice.split",
            [](const std::string& name, const json& config, const NodeContext&)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<SplitNode>(name, config.value("width", 2));
            });
    });
}

// Two parallel edges out of __start__ to branches of `type`.
json static_fan_out(const std::string& type, int width = 2) {
    json nodes = json::object();
    json edges = json::array();
    for (int i = 0; i < width; ++i) {
        const auto name = "branch" + std::to_string(i);
        nodes[name] = {{"type", type}};
        edges.push_back({{"from", "__start__"}, {"to", name}});
        edges.push_back({{"from", name}, {"to", "__end__"}});
    }
    return {{"name", "fan_out_notice"},
            {"channels", {{"out", {{"reducer", "append"}}}}},
            {"nodes", nodes},
            {"edges", edges}};
}

// A node that sends `width` Sends to a `leaf` of `type`.
json send_fan_out(const std::string& type, int width = 2) {
    return {{"name", "send_notice"},
            {"channels", {{"out", {{"reducer", "append"}}}}},
            {"nodes", {{"split", {{"type", "fanout_notice.split"}, {"width", width}}},
                       {"leaf", {{"type", type}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "split"}},
                                   {{"from", "split"}, {"to", "__end__"}}})}};
}

// Runs @p definition once and returns what the run wrote to stderr.
struct Observed {
    std::string stderr_text;
    int peak = 0;
};

Observed run_graph(const json& definition, std::size_t worker_count = 1, int runs = 1) {
    register_types();
    overlap().peak = 0;
    auto engine = GraphEngine::compile(definition, NodeContext{});
    if (worker_count != 1) engine->set_worker_count(worker_count);
    testing::internal::CaptureStderr();
    for (int i = 0; i < runs; ++i) {
        RunConfig config;
        config.thread_id = "notice-" + std::to_string(i);
        config.max_steps = 10;
        (void)engine->run(config);
    }
    Observed observed;
    observed.stderr_text = testing::internal::GetCapturedStderr();
    observed.peak = overlap().peak.load();
    return observed;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++count;
    return count;
}

void set_suppress(const char* value) {
#if defined(_WIN32)
    _putenv_s("NEOGRAPH_SUPPRESS_FANOUT_WARNING", value);
#else
    if (*value)
        setenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", value, 1);
    else
        unsetenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING");
#endif
}

// The notice is on by default; other tests in this binary may have switched
// it off for the process.
class FanOutSerialNotice : public testing::Test {
protected:
    void SetUp() override {
        if (const char* value = std::getenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING")) saved_ = value;
        set_suppress("");
    }
    void TearDown() override { set_suppress(saved_ ? saved_->c_str() : ""); }

private:
    std::optional<std::string> saved_;
};

}  // namespace

TEST_F(FanOutSerialNotice, SynchronousBranchesRunSeriallyAndSayWhy) {
    for (const auto& definition :
         {static_fan_out("fanout_notice.compute"), send_fan_out("fanout_notice.compute")}) {
        const auto observed = run_graph(definition);
        EXPECT_EQ(observed.peak, 1) << "plain computation on one thread cannot overlap";
        EXPECT_EQ(count_of(observed.stderr_text, kNotice), 1u) << observed.stderr_text;
        EXPECT_NE(observed.stderr_text.find("worker_count"), std::string::npos)
            << "the notice names the setting that changes it";
    }
}

TEST_F(FanOutSerialNotice, CoroutineBranchesThatOverlapAreNotReportedAsSerial) {
    for (const auto& definition :
         {static_fan_out("fanout_notice.waiting", 3), send_fan_out("fanout_notice.waiting", 3)}) {
        const auto observed = run_graph(definition);
        EXPECT_EQ(observed.peak, 3) << "the branches wait for their timers at the same time";
        EXPECT_EQ(observed.stderr_text.find(kNotice), std::string::npos) << observed.stderr_text;
    }
}

TEST_F(FanOutSerialNotice, TheNoticeIsStillOneShotPerEngine) {
    const auto observed = run_graph(static_fan_out("fanout_notice.compute"), 1, 3);
    EXPECT_EQ(count_of(observed.stderr_text, kNotice), 1u) << observed.stderr_text;
}

TEST_F(FanOutSerialNotice, AWorkerPoolOrASingleBranchHasNothingToReport) {
    EXPECT_EQ(run_graph(static_fan_out("fanout_notice.compute"), 2).stderr_text.find(kNotice),
              std::string::npos);
    EXPECT_EQ(run_graph(static_fan_out("fanout_notice.compute", 1)).stderr_text.find(kNotice),
              std::string::npos);
    EXPECT_EQ(run_graph(send_fan_out("fanout_notice.compute", 1)).stderr_text.find(kNotice),
              std::string::npos);
}
