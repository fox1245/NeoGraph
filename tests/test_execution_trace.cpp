// #424: RunResult::execution_trace lists every node execution, the ones a Send
// started included.
//
// The contract, for every entry point and with or without a worker pool: one
// entry per execution that completed, in the order the engine applied the
// results. A super-step lists its ready nodes in ready order, then its Sends in
// the order they were emitted, whatever order the executions finished in. A
// Send execution is listed as "<target>[send]", so the same node run by a Send
// and by an edge shows two kinds of entry. A step that paused lists nothing,
// and a run that resumed it lists the whole step, replayed executions included.
// A subgraph is one entry.

#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

using Body  = std::function<NodeOutput(NodeInput&)>;
using Trace = std::vector<std::string>;

class BodyNode final : public GraphNode {
public:
    BodyNode(std::string name, Body body) : name_(std::move(name)), body_(std::move(body)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override { co_return body_(in); }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    Body        body_;
};

void register_body(const std::string& type, Body body) {
    NodeFactory::instance().register_type(
        type, [body](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BodyNode>(name, body);
        });
}

// One Send per target, in order, each carrying its index as "item".
NodeOutput sends_to(const std::vector<std::string>& targets) {
    NodeOutput out;
    int        item = 0;
    for (const auto& target : targets) out.sends.push_back(Send{target, json{{"item", item++}}});
    return out;
}

// Appends the "item" it was started with to "results". Item 0 sleeps, so with
// a pool the first Send finishes last.
NodeOutput record_item(NodeInput& in) {
    const auto item = in.state.get("item");
    if (item == json(0)) std::this_thread::sleep_for(std::chrono::milliseconds(60));
    NodeOutput out;
    out.writes.push_back(ChannelWrite{"results", json::array({item})});
    return out;
}

void register_types() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_body("trace.planner",
                      [](NodeInput&) { return sends_to({"worker", "worker", "worker"}); });
        register_body("trace.fanout",
                      [](NodeInput&) { return sends_to({"alpha", "beta", "gamma"}); });
        register_body("trace.goto_planner", [](NodeInput&) {
            auto out    = sends_to({"worker", "worker", "worker"});
            out.command = Command{"after", {}};
            return out;
        });
        register_body("trace.ask_planner", [](NodeInput&) { return sends_to({"asker", "worker"}); });
        register_body("trace.worker", record_item);
        register_body("trace.asker", [](NodeInput& in) {
            if (!in.ctx.resume_value) throw NodeInterrupt("approve", json{{"ask", true}});
            return NodeOutput{};
        });
        register_body("trace.noop", [](NodeInput&) { return NodeOutput{}; });
    });
}

json channels() {
    return json{{"item", json{{"reducer", "overwrite"}}},
                {"results", json{{"reducer", "append"}, {"initial", json::array()}}}};
}

// planner Sends "worker" three times; no edge leads to worker.
json fanout_graph() {
    return json{{"channels", channels()},
                {"nodes", {{"planner", {{"type", "trace.planner"}}},
                           {"worker", {{"type", "trace.worker"}}}}},
                {"edges", json::array({{{"from", "__start__"}, {"to", "planner"}}})}};
}

std::unique_ptr<GraphEngine> build(const json&                      definition,
                                   std::size_t                      workers = 1,
                                   std::shared_ptr<CheckpointStore> store   = nullptr) {
    register_types();
    EngineConfig config;
    config.stderr_notices   = false;
    config.worker_count     = workers;
    config.checkpoint_store = std::move(store);
    return GraphEngine::build(definition, std::move(config));
}

template <typename T>
T drive(asio::awaitable<T> operation) {
    asio::io_context io;
    auto             future = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    return future.get();
}

const Trace kPlannerAndThreeWorkers{"planner", "worker[send]", "worker[send]", "worker[send]"};

}  // namespace

class ExecutionTraceEntryPoints : public testing::TestWithParam<std::size_t> {};

// The issue's graph: a planner fans out to three workers with Send. The trace
// lists the planner and the three worker executions, on every entry point.
TEST_P(ExecutionTraceEntryPoints, SendFanOutListsThePlannerAndEveryWorker) {
    auto engine = build(fanout_graph(), GetParam());

    EXPECT_EQ(engine->run(RunConfig{}).execution_trace, kPlannerAndThreeWorkers);
    EXPECT_EQ(engine->run_stream(RunConfig{}, [](const GraphEvent&) {}).execution_trace,
              kPlannerAndThreeWorkers);
    EXPECT_EQ(drive(engine->run_async(RunConfig{})).execution_trace, kPlannerAndThreeWorkers);
    EXPECT_EQ(
        drive(engine->run_stream_async(RunConfig{}, [](const GraphEvent&) {})).execution_trace,
        kPlannerAndThreeWorkers);

    RunConfig events;
    events.stream_mode = StreamMode::EVENTS;
    EXPECT_EQ(engine->run_stream(events, [](const GraphEvent&) {}).execution_trace,
              kPlannerAndThreeWorkers);
}

INSTANTIATE_TEST_SUITE_P(WithAndWithoutAWorkerPool, ExecutionTraceEntryPoints,
                         testing::Values(std::size_t{1}, std::size_t{3}));

TEST(ExecutionTrace, SendsAreListedInTheOrderTheyWereEmittedWhateverFinishesFirst) {
    json def{{"channels", channels()},
             {"nodes", {{"planner", {{"type", "trace.fanout"}}},
                        {"alpha", {{"type", "trace.worker"}}},
                        {"beta", {{"type", "trace.worker"}}},
                        {"gamma", {{"type", "trace.worker"}}}}},
             {"edges", json::array({{{"from", "__start__"}, {"to", "planner"}}})}};
    const Trace expected{"planner", "alpha[send]", "beta[send]", "gamma[send]"};

    for (const std::size_t workers : {std::size_t{1}, std::size_t{3}}) {
        auto result = build(def, workers)->run(RunConfig{});
        EXPECT_EQ(result.execution_trace, expected) << "workers=" << workers;
        // The writes fold in the same order.
        EXPECT_EQ(result.output["channels"]["results"]["value"], json::array({0, 1, 2}))
            << "workers=" << workers;
    }
}

TEST(ExecutionTrace, ReadyNodesComeBeforeTheSendsOfTheirStep) {
    json def{{"channels", channels()},
             {"nodes", {{"planner", {{"type", "trace.planner"}}},
                        {"worker", {{"type", "trace.worker"}}},
                        {"side", {{"type", "trace.noop"}}}}},
             {"edges", json::array({{{"from", "__start__"}, {"to", "planner"}},
                                    {{"from", "__start__"}, {"to", "side"}}})}};

    auto result = build(def, 3)->run(RunConfig{});

    EXPECT_EQ(result.execution_trace,
              (Trace{"planner", "side", "worker[send]", "worker[send]", "worker[send]"}));
}

TEST(ExecutionTrace, ACommandAndSendsOfOneNodeAreAllListed) {
    json def{{"channels", channels()},
             {"nodes", {{"planner", {{"type", "trace.goto_planner"}}},
                        {"worker", {{"type", "trace.worker"}}},
                        {"after", {{"type", "trace.noop"}}}}},
             {"edges", json::array({{{"from", "__start__"}, {"to", "planner"}}})}};

    auto result = build(def)->run(RunConfig{});

    EXPECT_EQ(result.execution_trace,
              (Trace{"planner", "worker[send]", "worker[send]", "worker[send]", "after"}));
}

TEST(ExecutionTrace, ANodeRunBySendAndByAnEdgeShowsBothKinds) {
    // The static edge makes `worker` run once more, after the Sends, on the
    // shared state, where no Send payload ever landed (#415).
    auto def = fanout_graph();
    def["edges"].push_back({{"from", "planner"}, {"to", "worker"}});

    auto result = build(def)->run(RunConfig{});

    EXPECT_EQ(result.execution_trace,
              (Trace{"planner", "worker[send]", "worker[send]", "worker[send]", "worker"}));
    EXPECT_EQ(result.output["channels"]["results"]["value"], json::array({0, 1, 2, nullptr}));
}

TEST(ExecutionTrace, ASendTargetNobodyEmittedToIsNotListed) {
    json def{{"channels", channels()},
             {"nodes", {{"planner", {{"type", "trace.noop"}}},
                        {"worker", {{"type", "trace.worker"}}}}},
             {"edges", json::array({{{"from", "__start__"}, {"to", "planner"}}})}};

    EXPECT_EQ(build(def)->run(RunConfig{}).execution_trace, Trace{"planner"});
}

TEST(ExecutionTrace, ACheckpointHoldsTheWritesOfEveryExecutionTheTraceLists) {
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = build(fanout_graph(), 1, store);

    RunConfig config;
    config.thread_id  = "trace-checkpoint";
    const auto result = engine->run(config);

    EXPECT_EQ(result.execution_trace, kPlannerAndThreeWorkers);
    ASSERT_FALSE(result.checkpoint_id.empty());
    const auto saved = store->load_by_id(result.checkpoint_id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->step, 0);
    EXPECT_EQ(saved->channel_values["channels"]["results"]["value"], json::array({0, 1, 2}));
}

TEST(ExecutionTrace, APausedStepListsNothingAndTheResumeListsTheWholeStep) {
    json def{{"channels", channels()},
             {"nodes", {{"planner", {{"type", "trace.ask_planner"}}},
                        {"worker", {{"type", "trace.worker"}}},
                        {"asker", {{"type", "trace.asker"}}}}},
             {"edges", json::array({{{"from", "__start__"}, {"to", "planner"}}})}};
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = build(def, 1, store);

    RunConfig config;
    config.thread_id  = "trace-pause";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_TRUE(paused.execution_trace.empty()) << "the paused step has not completed";

    // The step is re-entered whole: the planner and the Send that finished
    // replay, the Send that asked runs with the answer, and all three are listed.
    const auto done = engine->resume("trace-pause", json("yes"));
    EXPECT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(done.execution_trace, (Trace{"planner", "asker[send]", "worker[send]"}));
}

TEST(ExecutionTrace, ASubgraphIsOneEntryAndItsInnerNodesAreNotListed) {
    register_types();
    EngineConfig quiet;
    quiet.stderr_notices = false;
    auto child = std::shared_ptr<GraphEngine>(GraphEngine::build(fanout_graph(), quiet).release());
    NodeFactory::instance().register_type(
        "trace.subgraph", [child](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(name, child);
        });
    json def{{"channels", channels()},
             {"nodes", {{"outer", {{"type", "trace.subgraph"}}}}},
             {"edges", json::array({{{"from", "__start__"}, {"to", "outer"}}})}};

    EXPECT_EQ(build(def)->run(RunConfig{}).execution_trace, Trace{"outer"});
}
