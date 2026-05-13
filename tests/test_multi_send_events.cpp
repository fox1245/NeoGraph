// Regression coverage for stream-event emission during multi-Send fan-out.
//
// Single-Send dispatch goes through execute_node_with_retry, which emits
// NODE_START / NODE_END events when StreamMode::EVENTS is set. The
// multi-Send Taskflow path historically called execute_full() directly,
// skipping the event-emission block entirely — so callers watching a
// fanned-out graph saw the dispatching node's events but *not* the
// target node's, with no signal that parallel work had run.
//
// These tests pin the expected behaviour: every Send-invoked node
// emits its own NODE_START + NODE_END regardless of whether the
// fan-out fan is 1 or N.
//
// They also pin the __send__ debug meta-event which existed correctly
// before — it must still fire under DEBUG mode.

#include <gtest/gtest.h>
#include <neograph/neograph.h>

#include <atomic>
#include <mutex>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Planner that emits exactly `fanout_` Sends to "worker" with distinct
// task_idx payloads so the targets can be told apart.
class FanoutPlannerNode : public GraphNode {
public:
    explicit FanoutPlannerNode(int fanout) : fanout_(fanout) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        for (int i = 0; i < fanout_; ++i) {
            Send s;
            s.target_node = "worker";
            s.input = {{"task_idx", i}};
            out.sends.push_back(std::move(s));
        }
        co_return out;
    }
    std::string get_name() const override { return "planner"; }
private:
    int fanout_;
};

// Worker node: writes result. Stateless, thread-safe.
class WorkerNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        json v = in.state.get("task_idx");
        int idx = v.is_number_integer() ? v.get<int>() : -1;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({idx})});
        co_return out;
    }
    std::string get_name() const override { return "worker"; }
};

struct EventLog {
    std::mutex mu;
    std::vector<std::pair<GraphEvent::Type, std::string>> events;

    GraphStreamCallback callback() {
        return [this](const GraphEvent& e) {
            std::lock_guard lock(mu);
            events.emplace_back(e.type, e.node_name);
        };
    }

    int count(GraphEvent::Type t, const std::string& name) {
        std::lock_guard lock(mu);
        int n = 0;
        for (auto& [et, en] : events) if (et == t && en == name) ++n;
        return n;
    }
};

void register_once() {
    static std::once_flag f;
    std::call_once(f, []{
        NodeFactory::instance().register_type("__mse_worker",
            [](const std::string&, const json&, const NodeContext&)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<WorkerNode>();
            });
    });
}

std::unique_ptr<GraphEngine> compile_fanout_graph(int fanout) {
    register_once();

    // We register the planner per-fanout because it captures fanout_ as
    // ctor arg and the factory doesn't have that in scope otherwise.
    std::string planner_type = "__mse_planner_" + std::to_string(fanout);
    NodeFactory::instance().register_type(planner_type,
        [fanout](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<FanoutPlannerNode>(fanout);
        });

    json def = {
        {"name", "fanout_test"},
        {"channels", {
            {"task_idx", {{"reducer", "overwrite"}}},
            {"results",  {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner", {{"type", planner_type}}},
            {"worker",  {{"type", "__mse_worker"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    return GraphEngine::compile(def, ctx);
}

} // namespace

// Baseline sanity: single-Send still emits worker events (the branch we
// believed already worked). If this ever regresses, both branches have
// drifted.
TEST(MultiSendEvents, SingleSendEmitsWorkerEvents) {
    auto engine = compile_fanout_graph(1);
    EventLog log;

    RunConfig rc;
    rc.max_steps = 10;
    rc.stream_mode = StreamMode::EVENTS;
    auto result = engine->run_stream(rc, log.callback());

    EXPECT_EQ(1, log.count(GraphEvent::Type::NODE_START, "worker"));
    EXPECT_EQ(1, log.count(GraphEvent::Type::NODE_END,   "worker"));
}

// The bug: with fanout=3 the Taskflow multi-Send path bypassed event
// emission entirely, so "worker" never appeared in NODE_START /
// NODE_END — even though it executed 3 times.
TEST(MultiSendEvents, MultiSendEmitsPerInvocationEvents) {
    auto engine = compile_fanout_graph(3);
    EventLog log;

    RunConfig rc;
    rc.max_steps = 10;
    rc.stream_mode = StreamMode::EVENTS;
    auto result = engine->run_stream(rc, log.callback());

    // Three Sends → three NODE_START/END pairs for "worker".
    EXPECT_EQ(3, log.count(GraphEvent::Type::NODE_START, "worker"));
    EXPECT_EQ(3, log.count(GraphEvent::Type::NODE_END,   "worker"));
}

// DEBUG meta-event should still fire exactly once per multi-Send batch,
// regardless of whether per-target NODE_START/END is also emitted.
TEST(MultiSendEvents, DebugModeEmitsSendMetaEvent) {
    auto engine = compile_fanout_graph(3);
    EventLog log;

    RunConfig rc;
    rc.max_steps = 10;
    rc.stream_mode = StreamMode::EVENTS | StreamMode::DEBUG;
    auto result = engine->run_stream(rc, log.callback());

    EXPECT_EQ(1, log.count(GraphEvent::Type::NODE_START, "__send__"));
}

// =========================================================================
// Retry parity: parallel ready-set fan-out retries transient failures.
// Multi-Send used to call execute_full() directly, so retry_policy was
// silently ignored on fan-out to 2+ targets — inconsistent with both
// single-Send and parallel ready-set. These tests pin the normalised
// behaviour: multi-Send now honours the retry policy too.
// =========================================================================

// A worker that throws std::runtime_error on its first `fail_until_`
// invocations (per-instance counter), then succeeds. Shared across the
// fan-out so every Send sees the same counter — models a transient
// upstream failure (e.g. rate limit, flaky socket) that heals on retry.
class FlakyWorkerNode : public GraphNode {
public:
    FlakyWorkerNode(std::atomic<int>* attempts, int fail_until)
        : attempts_(attempts), fail_until_(fail_until) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int n = attempts_->fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= fail_until_) {
            throw std::runtime_error(
                "simulated transient failure #" + std::to_string(n));
        }
        json v = in.state.get("task_idx");
        int idx = v.is_number_integer() ? v.get<int>() : -1;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({idx})});
        co_return out;
    }
    std::string get_name() const override { return "worker"; }
private:
    std::atomic<int>* attempts_;
    int fail_until_;
};

// =========================================================================
// Command.updates parity: run_one applies command->updates after writes;
// run_sends did not — so a Send target's Command(goto, updates) silently
// dropped its `updates` into the void while the analogous pattern in
// ready-set execution worked. Same family of omission as the retry gap.
// =========================================================================

// A Send target that emits Command{updates=[{"side_effect", <idx>}]}. The
// goto_node is immaterial for Send semantics — what matters is that the
// updates make it into the merged state.
class CommandUpdateSendTarget : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        NodeOutput out;
        json v = in.state.get("task_idx");
        int idx = v.is_number_integer() ? v.get<int>() : -1;
        // A normal write (baseline) and a Command.updates entry. If the
        // engine drops command->updates, `side_effects` will stay empty
        // even though `results` accumulates normally.
        out.writes.push_back(ChannelWrite{"results", json::array({idx})});
        Command cmd;
        cmd.goto_node = "__end__";
        cmd.updates.push_back(
            ChannelWrite{"side_effects", json::array({idx * 100})});
        out.command = std::move(cmd);
        co_return out;
    }
    std::string get_name() const override { return "worker"; }
};

TEST(MultiSendEvents, SingleSendAppliesCommandUpdates) {
    NodeFactory::instance().register_type("__mse_cmd_worker",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<CommandUpdateSendTarget>();
        });
    NodeFactory::instance().register_type("__mse_cmd_planner1",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<FanoutPlannerNode>(1);  // single-send
        });

    json def = {
        {"name", "cmd_update_single"},
        {"channels", {
            {"task_idx",     {{"reducer", "overwrite"}}},
            {"results",      {{"reducer", "append"}}},
            {"side_effects", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner", {{"type", "__mse_cmd_planner1"}}},
            {"worker",  {{"type", "__mse_cmd_worker"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    RunConfig rc; rc.max_steps = 10;
    auto result = engine->run_stream(rc, {});

    ASSERT_TRUE(result.output["channels"].contains("side_effects"));
    EXPECT_EQ(1u, result.output["channels"]["side_effects"]["value"].size())
        << "Single-Send must propagate Command.updates, not just .writes";
}

TEST(MultiSendEvents, MultiSendAppliesCommandUpdates) {
    NodeFactory::instance().register_type("__mse_cmd_worker_m",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<CommandUpdateSendTarget>();
        });
    NodeFactory::instance().register_type("__mse_cmd_planner3",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<FanoutPlannerNode>(3);  // multi-send
        });

    json def = {
        {"name", "cmd_update_multi"},
        {"channels", {
            {"task_idx",     {{"reducer", "overwrite"}}},
            {"results",      {{"reducer", "append"}}},
            {"side_effects", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner", {{"type", "__mse_cmd_planner3"}}},
            {"worker",  {{"type", "__mse_cmd_worker_m"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    RunConfig rc; rc.max_steps = 10;
    auto result = engine->run_stream(rc, {});

    ASSERT_TRUE(result.output["channels"].contains("side_effects"));
    // All 3 fanned-out workers must have contributed their Command.updates.
    EXPECT_EQ(3u, result.output["channels"]["side_effects"]["value"].size())
        << "Multi-Send must propagate Command.updates from every worker";
}

TEST(MultiSendEvents, MultiSendHonoursRetryPolicy) {
    std::atomic<int> attempts{0};
    const int fail_until = 2;  // first 2 invocations throw, later ones succeed

    // Register a flaky worker that captures our shared counter.
    NodeFactory::instance().register_type("__mse_flaky",
        [&attempts, fail_until](const std::string&, const json&,
                                const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<FlakyWorkerNode>(&attempts, fail_until);
        });

    // Also register a fanout=3 planner to this flaky worker.
    NodeFactory::instance().register_type("__mse_planner_flaky",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<FanoutPlannerNode>(3);
        });

    json def = {
        {"name", "retry_fanout_test"},
        {"channels", {
            {"task_idx", {{"reducer", "overwrite"}}},
            {"results",  {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner", {{"type", "__mse_planner_flaky"}}},
            {"worker",  {{"type", "__mse_flaky"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        })}
    };
    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);

    RetryPolicy rp;
    rp.max_retries = 3;
    rp.initial_delay_ms = 1;
    rp.backoff_multiplier = 1.0f;
    rp.max_delay_ms = 1;
    engine->set_node_retry_policy("worker", rp);

    RunConfig rc;
    rc.max_steps = 10;
    auto result = engine->run_stream(rc, {});

    // Without multi-Send retry, the very first fan-out invocation would
    // throw and abort the run (exception propagates up run_sends). With
    // retry honoured, attempts > fail_until (we make 3 sends; attempts
    // is expected to be at least fail_until + 3 successful ones).
    EXPECT_GT(attempts.load(), fail_until);
    ASSERT_TRUE(result.output.contains("channels"));
    ASSERT_TRUE(result.output["channels"].contains("results"));
    // All 3 sends eventually succeeded.
    EXPECT_EQ(3u, result.output["channels"]["results"]["value"].size());
}
