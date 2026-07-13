// ChannelWrite::Mode — writes carry their intent (issue #91).
//
// Every mutation of GraphState went through the channel's reducer, with no way
// past it. For a channel with an accumulating reducer — `messages` being the
// obvious one — that means a value can only ever grow: no trimming a
// conversation history, no dropping a poisoned message, no resetting a
// scratchpad.
//
// The tempting fix is a GraphState::overwrite() that reaches in and mutates the
// value directly. It works, and it quietly breaks the property the engine rests
// on: *every mutation is expressible as a ChannelWrite*. That is what makes
// checkpoint + replay deterministic. A node that mutates state through a side
// door produces a change the write log never sees, so replaying from a
// checkpoint does not reproduce the run.
//
// So the intent rides on the write instead. The reducer stays the law for the
// default path; Overwrite is a first-class, recorded write.
//
// OverwriteSurvivesPendingWriteReplay is the test that earns the design. It is
// the one a side-door overwrite() cannot pass.

#include <gtest/gtest.h>
#include <neograph/neograph.h>

#include <atomic>
#include <memory>
#include <stdexcept>

using namespace neograph;
using namespace neograph::graph;

namespace {

ReducerFn append_fn() { return ReducerRegistry::instance().get("append"); }

// ── Engine-level fixture: crash mid-fan-out, resume, check the replay ──

// Seeds `banner` with one entry, so Overwrite and Reduce produce *different*
// results downstream. Without pre-existing content the two are indistinguishable
// and the replay test would pass even with mode dropped on the floor.
class SetupNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"banner", json::array({"setup"})});
        co_return out;
    }
    std::string get_name() const override { return "setup"; }
};

class PlannerNode : public GraphNode {
public:
    explicit PlannerNode(int fanout) : fanout_(fanout) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        for (int i = 0; i < fanout_; ++i) {
            Send s;
            s.target_node = "executor";
            s.input       = {{"task_idx", i}};
            out.sends.push_back(std::move(s));
        }
        co_return out;
    }
    std::string get_name() const override { return "planner"; }
private:
    int fanout_;
};

// Worker 0 overwrites `banner`; the rest append to `log`. Worker `fail_on`
// throws, which is what forces its successful siblings' writes — including
// worker 0's Overwrite — to be persisted as pending writes and replayed on
// resume. If the mode is lost in that round trip, the Overwrite comes back as a
// Reduce and `banner` accumulates instead of being replaced.
class ExecutorNode : public GraphNode {
public:
    ExecutorNode(std::atomic<int>* fail_on, std::atomic<int>* counter)
        : fail_on_(fail_on), counter_(counter) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        json v   = in.state.get("task_idx");
        int  idx = v.is_number_integer() ? v.get<int>() : -999;
        counter_->fetch_add(1, std::memory_order_relaxed);

        if (fail_on_->load(std::memory_order_relaxed) == idx) {
            throw std::runtime_error("simulated failure at idx=" + std::to_string(idx));
        }

        NodeOutput out;
        if (idx == 0) {
            ChannelWrite w{"banner", json::array({"final"})};
            w.mode = ChannelWrite::Mode::Overwrite;
            out.writes.push_back(w);
        } else {
            out.writes.push_back(ChannelWrite{"log", json::array({idx})});
        }
        co_return out;
    }
    std::string get_name() const override { return "executor"; }

private:
    std::atomic<int>* fail_on_;
    std::atomic<int>* counter_;
};

json make_graph(int fanout) {
    (void)fanout;
    return {
        {"name", "channel_write_mode_test"},
        {"channels", {
            {"banner",   {{"reducer", "append"}}},
            {"log",      {{"reducer", "append"}}},
            {"task_idx", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"setup",    {{"type", "cwm_setup"}}},
            {"planner",  {{"type", "cwm_planner"}}},
            {"executor", {{"type", "cwm_executor"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "setup"}},
            {{"from", "setup"},     {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        }}
    };
}

class ChannelWriteModeTest : public ::testing::Test {
protected:
    std::atomic<int> fail_on{-1};
    std::atomic<int> exec_counter{0};
    int fanout = 4;

    void SetUp() override {
        fail_on = -1;
        exec_counter = 0;
        NodeFactory::instance().register_type("cwm_setup",
            [](const std::string&, const json&, const NodeContext&) {
                return std::make_unique<SetupNode>();
            });
        NodeFactory::instance().register_type("cwm_planner",
            [this](const std::string&, const json&, const NodeContext&) {
                return std::make_unique<PlannerNode>(fanout);
            });
        NodeFactory::instance().register_type("cwm_executor",
            [this](const std::string&, const json&, const NodeContext&) {
                return std::make_unique<ExecutorNode>(&fail_on, &exec_counter);
            });
    }
};

}  // namespace

// ── 1. GraphState: the mode decides whether the reducer runs ──

TEST(ChannelWriteMode, OverwriteBypassesTheReducer) {
    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    state.write("messages", json::array({"a"}));
    state.write("messages", json::array({"b"}));
    ASSERT_EQ(state.get("messages").size(), 2u) << "append reducer should accumulate";

    std::vector<ChannelWrite> writes;
    ChannelWrite w{"messages", json::array({"only"})};
    w.mode = ChannelWrite::Mode::Overwrite;
    writes.push_back(w);
    state.apply_writes(writes);

    EXPECT_EQ(state.get("messages"), json::array({"only"}))
        << "Overwrite must replace the accumulated value, not append to it";
}

// Existing behavior — must stay green. A write with no mode set still reduces.
TEST(ChannelWriteMode, ReduceIsTheDefault) {
    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    state.write("messages", json::array({"a"}));
    state.apply_writes({ChannelWrite{"messages", json::array({"b"})}});

    EXPECT_EQ(state.get("messages"), json::array({"a", "b"}));
}

// ── 2. The write log is the truth: mode survives checkpoint + replay ──
//
// This is the test a side-door GraphState::overwrite() cannot pass. A direct
// mutation never enters the write log, so nothing is persisted, and the replay
// silently reconstructs a *different* state than the original run.

TEST_F(ChannelWriteModeTest, OverwriteAppliesOnACleanRun) {
    auto engine = GraphEngine::compile(make_graph(fanout), NodeContext{},
                                       std::make_shared<InMemoryCheckpointStore>());
    RunConfig cfg;
    cfg.thread_id = "clean";
    auto result = engine->run(cfg);

    EXPECT_EQ(result.channel_raw("banner"), json::array({"final"}))
        << "worker 0's Overwrite should have replaced the setup banner";
}

TEST_F(ChannelWriteModeTest, OverwriteSurvivesPendingWriteReplay) {
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_graph(fanout), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "crashy";

    // Worker 2 throws. Workers 0, 1, 3 succeed, and their writes — worker 0's
    // being the Overwrite — are recorded as pending writes against the parent
    // checkpoint.
    fail_on = 2;
    EXPECT_THROW(engine->run(cfg), std::exception);
    ASSERT_EQ(exec_counter.load(), fanout) << "all four workers should have been dispatched";

    // resume() (not run()) is the path that replays pending writes. Only the
    // failed worker re-executes; the successful ones are NOT re-run, so the
    // Overwrite that lands in the final state can only have come back through
    // serialize -> store -> deserialize. That is what gives this test teeth:
    // drop the mode from the wire format and banner reduces to
    // ["setup", "final"] instead of being replaced.
    fail_on = -1;
    auto resumed = engine->resume("crashy");

    ASSERT_EQ(exec_counter.load(), fanout + 1)
        << "exactly one re-execution (the failed worker) — if the successful "
           "workers re-ran, this test would pass even with the mode dropped";

    EXPECT_EQ(resumed.channel_raw("banner"), json::array({"final"}))
        << "the replayed write lost its Overwrite mode and reduced instead";
    EXPECT_EQ(resumed.channel_raw("log").size(), 3u)
        << "the three appending workers' writes should all have replayed";
}
