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
    std::vector<ChannelWrite> execute(const GraphState&) override { return {}; }
    NodeResult execute_full(const GraphState&) override {
        NodeResult nr;
        for (int i = 0; i < fanout_; ++i) {
            Send s;
            s.target_node = "worker";
            s.input = {{"task_idx", i}};
            nr.sends.push_back(std::move(s));
        }
        return nr;
    }
    std::string get_name() const override { return "planner"; }
private:
    int fanout_;
};

// Worker node: writes result. Stateless, thread-safe.
class WorkerNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        json v = state.get("task_idx");
        int idx = v.is_number_integer() ? v.get<int>() : -1;
        return {ChannelWrite{"results", json::array({idx})}};
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
