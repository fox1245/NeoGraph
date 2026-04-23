// Regression tests for the Sends-vs-interrupt_after ordering contract.
//
// A node in interrupt_after_ can also emit Sends as part of its step's
// output. Pre-fix, the super-step loop ran:
//
//     execute ready nodes → collect Sends → stream VALUES →
//     interrupt_after gate → run_sends
//
// so the interrupt cp captured state BEFORE Sends executed. Resume
// then advances past the super-step (phase=After ⇒ start_step += 1)
// and the Send targets never run. Any state they would have written
// is silently lost.
//
// The fix moves run_sends above the interrupt_after gate:
//
//     execute ready nodes → collect Sends → run_sends → stream VALUES →
//     interrupt_after gate
//
// The committed cp now carries post-Sends state, resume is correct,
// and Stream VALUES shows the complete super-step output.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <atomic>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Planner: produces a single Send to "worker" with a chosen payload.
// No retries, no state writes — just dispatches fan-out.
class SinglePlanner : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState&) override { return {}; }
    NodeResult execute_full(const GraphState&) override {
        NodeResult nr;
        nr.sends.push_back(Send{"worker", json{{"payload", 7}}});
        return nr;
    }
    asio::awaitable<NodeResult>
    execute_full_async(const GraphState& state) override {
        co_return execute_full(state);
    }
    std::string get_name() const override { return "planner"; }
};

// Worker: records that it ran and writes the payload back to an
// append-reduced "results" channel so the test can assert on it.
class Worker : public GraphNode {
public:
    explicit Worker(std::atomic<int>* counter) : counter_(counter) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        counter_->fetch_add(1, std::memory_order_relaxed);
        json p = state.get("payload");
        int v = p.is_number_integer() ? p.get<int>() : -1;
        return {ChannelWrite{"results", json(v)}};
    }
    std::string get_name() const override { return "worker"; }
private:
    std::atomic<int>* counter_;
};

static json make_planner_interrupt_graph() {
    return {
        {"name", "planner_interrupt_after"},
        {"channels", {
            {"payload", {{"reducer", "overwrite"}}},
            {"results", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner", {{"type", "si_planner"}}},
            {"worker",  {{"type", "si_worker"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        })},
        {"interrupt_after", {"planner"}}
    };
}

} // namespace

// A planner with interrupt_after that emits Sends must see its Sends
// executed BEFORE the interrupt cp is saved. Otherwise the cp state
// is missing the Send targets' writes and resume never recovers them.
TEST(SendsInterruptAfter, SingleSendFiresBeforeInterruptCheckpoint) {
    std::atomic<int> worker_runs{0};
    NodeFactory::instance().register_type("si_planner",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SinglePlanner>();
        });
    NodeFactory::instance().register_type("si_worker",
        [&](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<Worker>(&worker_runs);
        });

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_planner_interrupt_graph(),
                                        NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "send-before-iafter";
    auto result = engine->run(cfg);

    ASSERT_TRUE(result.interrupted);
    EXPECT_EQ(result.interrupt_node, "planner");
    EXPECT_EQ(worker_runs.load(), 1)
        << "Send target must run before interrupt_after pauses the graph";

    // The saved cp must carry the worker's write — that is the whole
    // point: state is captured POST-Sends so resume is correct.
    auto cp = store->load_latest("send-before-iafter");
    ASSERT_TRUE(cp.has_value());
    EXPECT_EQ(cp->interrupt_phase, CheckpointPhase::After);
    ASSERT_TRUE(cp->channel_values.contains("channels"));
    ASSERT_TRUE(cp->channel_values["channels"].contains("results"));
    auto results_value = cp->channel_values["channels"]["results"]["value"];
    ASSERT_TRUE(results_value.is_array());
    ASSERT_EQ(results_value.size(), 1u);
    EXPECT_EQ(results_value[0].get<int>(), 7);
}

// End-to-end: after the above interrupt, resume must NOT re-execute
// the worker (its writes are already in the committed cp state) — it
// should just advance to __end__ cleanly.
TEST(SendsInterruptAfter, ResumeDoesNotReexecuteSendTarget) {
    std::atomic<int> worker_runs{0};
    NodeFactory::instance().register_type("si_planner",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SinglePlanner>();
        });
    NodeFactory::instance().register_type("si_worker",
        [&](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<Worker>(&worker_runs);
        });

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_planner_interrupt_graph(),
                                        NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "resume-after-send";
    auto first = engine->run(cfg);
    ASSERT_TRUE(first.interrupted);
    EXPECT_EQ(worker_runs.load(), 1);

    auto resumed = engine->resume("resume-after-send");
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(worker_runs.load(), 1)
        << "Send target must not run a second time on resume";

    // Final state still carries the single write from the initial run.
    auto final_results = resumed.output["channels"]["results"]["value"];
    ASSERT_TRUE(final_results.is_array());
    ASSERT_EQ(final_results.size(), 1u);
    EXPECT_EQ(final_results[0].get<int>(), 7);
}
