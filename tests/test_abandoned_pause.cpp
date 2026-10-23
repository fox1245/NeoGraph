// #600: a pause that follows an abandoned pause resumes.
//
// A NodeInterrupt (pause P1) that its host never resumes is abandoned when the
// host starts another run on the same thread (RunConfig::resume_if_exists).
// That run continues the thread's step numbering after P1 and, as long as no
// step of it has completed, parents every checkpoint it saves on P1. When its
// first super-step pauses too (P2), P2's parent is P1, a NodeInterrupt of an
// earlier step. Resuming P2 collects the pending writes of P2's own super-step
// by walking its parent chain, and used to refuse a chain that reached a
// NodeInterrupt of another step.
//
// The contract: the walk covers the pauses of one super-step. A NodeInterrupt
// ancestor of an earlier step is where the later run started from, so the walk
// takes the writes recorded under it for P2's step (the siblings of P2's step
// record theirs under the checkpoint the run started from) and stops there.
// The abandoned pause's own writes belong to its own step and are not P2's.
//
// The cases put the pausing node in the first super-step of the second run,
// which is the only place the parent of P2 is another pause: a step that
// completes first saves a Completed checkpoint for P2 to hang from, as the
// case named AFinishedStepBetweenThePausesStillSeparatesThem shows. That case
// and the run that restarts without resume_if_exists pass on the engine that
// refused the chain; they pin what the fix must leave alone.

#include "fixtures/checkpoint_backends.h"
#include "fixtures/unique_temp_dir.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/coordinator.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

std::mutex&                 runs_mutex() { static std::mutex m; return m; }
std::map<std::string, int>& runs_map() { static std::map<std::string, int> m; return m; }

void count_run(const std::string& key) {
    std::lock_guard lock(runs_mutex());
    ++runs_map()[key];
}

int runs(const std::string& probe, const std::string& name) {
    std::lock_guard lock(runs_mutex());
    const auto      found = runs_map().find(probe + "/" + name);
    return found == runs_map().end() ? 0 : found->second;
}

std::string unique_probe(const char* name) {
    static std::atomic<int> next{0};
    return std::string(name) + "-" + std::to_string(++next);
}

std::string probe_of(const json& config) { return config.value("probe", std::string()); }

std::string turn_of(const NodeInput& in) {
    const auto turn = in.state.get("turn");
    return turn.is_string() ? turn.get<std::string>() : std::string();
}

// Asks until it is answered. Its question and its answer carry the turn they
// belong to, so a value of an earlier turn is recognisable in the result.
class AskNode final : public GraphNode {
public:
    AskNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        const auto  turn   = turn_of(in);
        const auto& answer = in.ctx.resume_value;
        if (!answer) throw NodeInterrupt("approve " + name_, json{{"asks", name_}, {"turn", turn}});
        NodeOutput out;
        out.writes.push_back(ChannelWrite{
            "results", json::array({name_ + "=" + answer->get<std::string>() + "@" + turn})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Finishes at once and writes its name and the turn.
class PlainNode final : public GraphNode {
public:
    PlainNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({name_ + "@" + turn_of(in)})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

void register_types() {
    static const bool registered = [] {
        auto& factory = NodeFactory::instance();
        factory.register_type(
            "abandoned_pause.ask",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<AskNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "abandoned_pause.plain",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<PlainNode>(name, config);
            },
            json{{"type", "object"}});
        return true;
    }();
    (void)registered;
}

json channels() {
    return json{{"turn", json{{"reducer", "overwrite"}, {"initial", ""}}},
                {"pad", json{{"reducer", "overwrite"}, {"initial", ""}}},
                {"results", json{{"reducer", "append"}, {"initial", json::array()}}}};
}

json typed(const std::string& probe, const char* type) {
    return json{{"type", type}, {"probe", probe}};
}

json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }

json define(const char* name, json nodes, json edges) {
    return json{{"name", name}, {"channels", channels()}, {"nodes", std::move(nodes)},
                {"edges", std::move(edges)}};
}

// One node that asks, in the first super-step.
json single(const std::string& probe) {
    return define("abandoned_pause_single",
                  json{{"ask", typed(probe, "abandoned_pause.ask")}},
                  json::array({edge("__start__", "ask"), edge("ask", "__end__")}));
}

// ask and calm in the first super-step: no checkpoint precedes the step, so
// calm's write is recorded under the pause that the step saves (#385) and, in
// a run that starts after an earlier pause, under that pause.
json entry_fan_out(const std::string& probe) {
    return define("abandoned_pause_fan_out",
                  json{{"ask", typed(probe, "abandoned_pause.ask")},
                       {"calm", typed(probe, "abandoned_pause.plain")}},
                  json::array({edge("__start__", "ask"), edge("__start__", "calm"),
                               edge("ask", "__end__"), edge("calm", "__end__")}));
}

// Two nodes that ask, in the first super-step: the pauses of one step are
// answered one at a time, and each answer saves a pause of the same step.
json entry_two_askers(const std::string& probe) {
    return define("abandoned_pause_two_askers",
                  json{{"left", typed(probe, "abandoned_pause.ask")},
                       {"right", typed(probe, "abandoned_pause.ask")}},
                  json::array({edge("__start__", "left"), edge("__start__", "right"),
                               edge("left", "__end__"), edge("right", "__end__")}));
}

// first finishes in the first super-step; ask pauses in the second.
json two_steps(const std::string& probe) {
    return define("abandoned_pause_two_steps",
                  json{{"first", typed(probe, "abandoned_pause.plain")},
                       {"ask", typed(probe, "abandoned_pause.ask")}},
                  json::array({edge("__start__", "first"), edge("first", "ask"),
                               edge("ask", "__end__")}));
}

EngineConfig config_with(std::shared_ptr<CheckpointStore> store) {
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.stderr_notices   = false;
    config.worker_count     = 2;
    return config;
}

std::unique_ptr<GraphEngine> build(const json& definition, std::shared_ptr<CheckpointStore> store) {
    register_types();
    return GraphEngine::build(definition, config_with(std::move(store)));
}

std::multiset<std::string> results_of(const RunResult& result) {
    std::multiset<std::string> out;
    const auto                 values = result.output["channels"]["results"]["value"];
    for (std::size_t index = 0; index < values.size(); ++index)
        out.insert(values[index].get<std::string>());
    return out;
}

// A new turn on a thread: seeds the state from the thread's latest checkpoint
// and starts again at the entry, as a host does for the next message.
RunResult turn(GraphEngine& engine, const std::string& thread, const std::string& name) {
    RunConfig config;
    config.thread_id        = thread;
    config.resume_if_exists = true;
    config.input            = json{{"turn", name}};
    return engine.run(config);
}

RunResult answer(GraphEngine& engine, const std::string& thread, const std::string& value,
                 const std::string& interrupt_id = {}) {
    RunConfig config;
    config.thread_id = thread;
    RunMetadata metadata;
    metadata.resume_interrupt_id = interrupt_id;
    return engine.resume(config, json(value), nullptr, metadata);
}

std::set<std::string> task_ids_of(const std::vector<PendingWrite>& writes) {
    std::set<std::string> ids;
    for (const auto& write : writes) ids.insert(write.task_id);
    return ids;
}

std::set<std::string> task_ids_of(const ResumeContext& context) {
    std::set<std::string> ids;
    for (const auto& [id, result] : context.replay_results) ids.insert(id);
    return ids;
}

// The values a recorded write carries, to tell the turn it belongs to.
std::string values_of(const PendingWrite& write) { return write.writes.dump(); }

using test_support::Backend;

class AbandonedPause : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() override {
        if (!test_support::backend_available(GetParam()))
            GTEST_SKIP() << "NEOGRAPH_TEST_POSTGRES_URL not set";
        test_support::reset_backend(GetParam());
    }
    void TearDown() override {
        store_.reset();
        if (test_support::backend_available(GetParam())) test_support::reset_backend(GetParam());
    }
    std::shared_ptr<CheckpointStore> store() {
        if (!store_) store_ = test_support::open_backend(GetParam(), dir_.file("checkpoints.sqlite"));
        return store_;
    }
    Checkpoint saved(const std::string& id) {
        auto loaded = store()->load_by_id(id);
        EXPECT_TRUE(loaded) << "no checkpoint " << id;
        return loaded ? std::move(*loaded) : Checkpoint{};
    }

private:
    test_support::UniqueTempDir      dir_{"ng-abandoned-pause-"};
    std::shared_ptr<CheckpointStore> store_;
};

}  // namespace

TEST_P(AbandonedPause, ThePauseOfASecondRunOnTheThreadResumes) {
    const auto probe  = unique_probe("single");
    auto       engine = build(single(probe), store());
    const auto first  = turn(*engine, "t", "t1");
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto second = turn(*engine, "t", "t2");
    ASSERT_EQ(second.status(), RunStatus::Interrupted);

    // The premise: the second pause hangs from the abandoned one, one step on.
    const auto p1 = saved(first.checkpoint_id);
    const auto p2 = saved(second.checkpoint_id);
    ASSERT_EQ(p2.parent_id, p1.id);
    ASSERT_EQ(p2.interrupt_phase, CheckpointPhase::NodeInterrupt);
    ASSERT_EQ(p1.interrupt_phase, CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(p2.step, p1.step + 1);

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"ask=yes@t2"}));
    EXPECT_EQ(runs(probe, "ask"), 3) << "once per turn, and once with the answer";
}

TEST_P(AbandonedPause, ThreePausesInARowResumeTheLast) {
    const auto probe  = unique_probe("three");
    auto       engine = build(single(probe), store());
    std::vector<std::string> pauses;
    for (const char* name : {"t1", "t2", "t3"}) {
        const auto paused = turn(*engine, "t", name);
        ASSERT_EQ(paused.status(), RunStatus::Interrupted) << name;
        pauses.push_back(paused.checkpoint_id);
    }
    EXPECT_EQ(saved(pauses[1]).parent_id, pauses[0]);
    EXPECT_EQ(saved(pauses[2]).parent_id, pauses[1]);

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"ask=yes@t3"}));

    // The thread goes on: the next turn pauses again and resumes.
    const auto fourth = turn(*engine, "t", "t4");
    ASSERT_EQ(fourth.status(), RunStatus::Interrupted);
    const auto last = answer(*engine, "t", "no");
    ASSERT_EQ(last.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(last), (std::multiset<std::string>{"ask=yes@t3", "ask=no@t4"}));
}

TEST_P(AbandonedPause, ASiblingOfTheLaterPauseReplaysAndTheAbandonedOnesRowIsNotItsRow) {
    const auto probe  = unique_probe("fan-out");
    auto       engine = build(entry_fan_out(probe), store());
    const auto first  = turn(*engine, "t", "t1");
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto second = turn(*engine, "t", "t2");
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    const auto p1 = saved(first.checkpoint_id);
    const auto p2 = saved(second.checkpoint_id);
    ASSERT_EQ(p2.parent_id, p1.id);
    ASSERT_EQ(p1.parent_id, "") << "no checkpoint precedes the first step";

    // Both turns' calm are recorded under the abandoned pause: its own, for
    // step 0, and the second turn's, for step 1, whose parent it is.
    const auto under_p1 = store()->get_writes("t", p1.id);
    ASSERT_EQ(under_p1.size(), 2U);
    EXPECT_EQ(task_ids_of(under_p1), (std::set<std::string>{"s0:calm", "s1:calm"}));

    // The writes a resume of the later pause replays: the second turn's calm.
    const auto replayed = resume_pending_writes(*store(), p2);
    ASSERT_EQ(replayed.size(), 1U) << "the abandoned pause's row is not a row of P2's step";
    EXPECT_EQ(replayed[0].task_id, "s1:calm");
    EXPECT_EQ(replayed[0].step, p2.step);
    EXPECT_NE(values_of(replayed[0]).find("calm@t2"), std::string::npos) << values_of(replayed[0]);
    EXPECT_EQ(values_of(replayed[0]).find("t1"), std::string::npos) << values_of(replayed[0]);

    // The abandoned pause still replays its own.
    const auto own = resume_pending_writes(*store(), p1);
    ASSERT_EQ(own.size(), 1U);
    EXPECT_EQ(own[0].task_id, "s0:calm");
    EXPECT_NE(values_of(own[0]).find("calm@t1"), std::string::npos) << values_of(own[0]);

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"calm@t2", "ask=yes@t2"}))
        << "the first turn's calm is not in the second turn's result";
    EXPECT_EQ(runs(probe, "calm"), 2) << "the second turn's calm replays instead of running again";
}

TEST_P(AbandonedPause, TheCoordinatorLoadsTheStepOfTheLaterPauseOnEveryPath) {
    const auto probe  = unique_probe("coordinator");
    auto       engine = build(entry_fan_out(probe), store());
    ASSERT_EQ(turn(*engine, "t", "t1").status(), RunStatus::Interrupted);
    const auto second = turn(*engine, "t", "t2");
    ASSERT_EQ(second.status(), RunStatus::Interrupted);

    const std::set<std::string> expected{"s1:calm"};
    CheckpointCoordinator       coordinator(store(), "t");
    const auto                  blocking = coordinator.load_for_resume();
    ASSERT_TRUE(blocking.have_cp);
    EXPECT_EQ(blocking.checkpoint_id, second.checkpoint_id);
    EXPECT_EQ(task_ids_of(blocking), expected);
    const auto latest = neograph::async::run_sync(coordinator.load_for_resume_async());
    EXPECT_EQ(task_ids_of(latest), expected);
    const auto exact = neograph::async::run_sync(
        coordinator.load_for_resume_by_id_async(second.checkpoint_id));
    EXPECT_EQ(task_ids_of(exact), expected);
}

TEST_P(AbandonedPause, ThreePausesWithASiblingEachReplayOnlyTheLastTurn) {
    const auto probe  = unique_probe("three-fan-out");
    auto       engine = build(entry_fan_out(probe), store());
    for (const char* name : {"t1", "t2", "t3"})
        ASSERT_EQ(turn(*engine, "t", name).status(), RunStatus::Interrupted) << name;

    CheckpointCoordinator coordinator(store(), "t");
    EXPECT_EQ(task_ids_of(neograph::async::run_sync(coordinator.load_for_resume_async())),
              (std::set<std::string>{"s2:calm"}));

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"calm@t3", "ask=yes@t3"}));
    EXPECT_EQ(runs(probe, "calm"), 3);
}

TEST_P(AbandonedPause, PausesOfOneStepStillChainBeforeAndAfterAnAbandonedPause) {
    const auto probe  = unique_probe("two-askers");
    auto       engine = build(entry_two_askers(probe), store());

    // Turn 1: both ask; left is answered, right asks again, and the host
    // walks away from that pause (a pause of step 0 whose parent is a pause
    // of step 0).
    const auto first = turn(*engine, "t", "t1");
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto asked_again = answer(*engine, "t", "L1", "s0:left");
    ASSERT_EQ(asked_again.status(), RunStatus::Interrupted);
    EXPECT_EQ(asked_again.interrupt_id, "s0:right");
    ASSERT_EQ(saved(asked_again.checkpoint_id).parent_id, first.checkpoint_id);

    // Turn 2 starts on top of it and pauses on both; left is answered and
    // right asks again, in a pause whose parent is the turn's first pause.
    const auto second = turn(*engine, "t", "t2");
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    ASSERT_EQ(saved(second.checkpoint_id).parent_id, asked_again.checkpoint_id);
    const auto step_two = answer(*engine, "t", "L2", "s1:left");
    ASSERT_EQ(step_two.status(), RunStatus::Interrupted);
    EXPECT_EQ(step_two.interrupt_id, "s1:right");
    ASSERT_EQ(saved(step_two.checkpoint_id).parent_id, second.checkpoint_id);

    // The last pause reaches the first of its own turn, then the abandoned
    // chain, and takes only the rows of its step: left's answer is replayed.
    CheckpointCoordinator coordinator(store(), "t");
    EXPECT_EQ(task_ids_of(neograph::async::run_sync(coordinator.load_for_resume_async())),
              (std::set<std::string>{"s1:left"}));

    const auto done = answer(*engine, "t", "R2");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"left=L2@t2", "right=R2@t2"}));
    EXPECT_EQ(runs(probe, "left"), 4) << "left's answer in turn 2 replays; it does not run again";
    EXPECT_EQ(runs(probe, "right"), 5);
}

TEST_P(AbandonedPause, ARunThatSharesTheStepNumberButNotThePauseHangsFromNothing) {
    // Without resume_if_exists a second run starts again at step 0 and parents
    // nothing: its pause has the abandoned pause's step and is not in its
    // chain. Its rows are its own.
    //
    // The in-memory and SQLite stores keep a channel's value once per
    // (thread, channel, version), and a run that starts over restarts the
    // versions, so a value that the second run writes at a version the first
    // run used would read back as the first run's. The second run writes "pad"
    // first, which moves "turn" to a version the first run did not use.
    const auto probe  = unique_probe("restart");
    auto       engine = build(entry_fan_out(probe), store());
    RunConfig  first_config;
    first_config.thread_id = "t";
    first_config.input     = json{{"turn", "t1"}};
    const auto first = engine->run(first_config);
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    RunConfig second_config;
    second_config.thread_id = "t";
    second_config.input     = json{{"pad", "p"}, {"turn", "t2"}};
    const auto second = engine->run(second_config);
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    const auto p1 = saved(first.checkpoint_id);
    const auto p2 = saved(second.checkpoint_id);
    ASSERT_EQ(p2.parent_id, "");
    ASSERT_EQ(p2.step, p1.step);

    const auto replayed = resume_pending_writes(*store(), p2);
    ASSERT_EQ(replayed.size(), 1U);
    EXPECT_NE(values_of(replayed[0]).find("calm@t2"), std::string::npos) << values_of(replayed[0]);

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"calm@t2", "ask=yes@t2"}));
    EXPECT_EQ(runs(probe, "calm"), 2);
}

TEST_P(AbandonedPause, AFinishedStepBetweenThePausesStillSeparatesThem) {
    // A step of the second run that completes saves a Completed checkpoint for
    // the later pause to hang from, as before.
    const auto probe  = unique_probe("separated");
    auto       engine = build(two_steps(probe), store());
    const auto first  = turn(*engine, "t", "t1");
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto second = turn(*engine, "t", "t2");
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    const auto p2 = saved(second.checkpoint_id);
    EXPECT_EQ(saved(p2.parent_id).interrupt_phase, CheckpointPhase::Completed);

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done),
              (std::multiset<std::string>{"first@t1", "first@t2", "ask=yes@t2"}));
}

TEST_P(AbandonedPause, AStateUpdateOfTheAbandonedPauseIsNotInTheChainEither) {
    // update_state saves a checkpoint that keeps the pause's phase and step on
    // top of P1; the second run then starts on top of that one.
    const auto probe  = unique_probe("updated");
    auto       engine = build(entry_fan_out(probe), store());
    ASSERT_EQ(turn(*engine, "t", "t1").status(), RunStatus::Interrupted);
    engine->update_state("t", json{{"turn", "t1-edited"}});
    const auto second = turn(*engine, "t", "t2");
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    const auto p2 = saved(second.checkpoint_id);
    ASSERT_EQ(saved(p2.parent_id).interrupt_phase, CheckpointPhase::Updated);

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"calm@t2", "ask=yes@t2"}));
    EXPECT_EQ(runs(probe, "calm"), 2);
}

TEST_P(AbandonedPause, EitherPauseResumesByItsOwnCheckpointId) {
    const auto probe  = unique_probe("by-id");
    auto       engine = build(entry_fan_out(probe), store());
    const auto first  = turn(*engine, "t", "t1");
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto second = turn(*engine, "t", "t2");
    ASSERT_EQ(second.status(), RunStatus::Interrupted);

    // The later pause: it takes the rows of its own step.
    RunConfig config;
    config.thread_id = "t";
    const auto later = engine->resume_from(config, second.checkpoint_id, json("yes"));
    ASSERT_EQ(later.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(later), (std::multiset<std::string>{"calm@t2", "ask=yes@t2"}));

    // The abandoned pause still resumes, with the state and the rows of its
    // own turn.
    const auto earlier = engine->resume_from(config, first.checkpoint_id, json("no"));
    ASSERT_EQ(earlier.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(earlier), (std::multiset<std::string>{"calm@t1", "ask=no@t1"}));
}

TEST_P(AbandonedPause, TheAbandonedPauseMayBeResumedBeforeTheLaterOne) {
    // Finishing the step of P1 clears the rows recorded under P1, which is
    // also where the second turn's sibling rows are. The later pause then
    // resumes with nothing to replay: calm runs again, and the result is
    // still the second turn's.
    const auto probe  = unique_probe("earlier-first");
    auto       engine = build(entry_fan_out(probe), store());
    const auto first  = turn(*engine, "t", "t1");
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto second = turn(*engine, "t", "t2");
    ASSERT_EQ(second.status(), RunStatus::Interrupted);

    RunConfig config;
    config.thread_id = "t";
    const auto earlier = engine->resume_from(config, first.checkpoint_id, json("no"));
    ASSERT_EQ(earlier.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(earlier), (std::multiset<std::string>{"calm@t1", "ask=no@t1"}));

    const auto later = engine->resume_from(config, second.checkpoint_id, json("yes"));
    ASSERT_EQ(later.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(later), (std::multiset<std::string>{"calm@t2", "ask=yes@t2"}));
}

TEST_P(AbandonedPause, AForkOfTheLaterPauseCarriesItsRows) {
    // fork() copies the rows a resume of the source would replay, with the
    // same walk (#390); it used to fail on the same chain.
    const auto probe  = unique_probe("fork");
    auto       engine = build(entry_fan_out(probe), store());
    ASSERT_EQ(turn(*engine, "t", "t1").status(), RunStatus::Interrupted);
    ASSERT_EQ(turn(*engine, "t", "t2").status(), RunStatus::Interrupted);
    ASSERT_EQ(runs(probe, "calm"), 2);

    const auto forked = engine->fork("t", "t-fork");
    EXPECT_FALSE(forked.empty());
    const auto copied = store()->get_writes("t-fork", forked);
    ASSERT_EQ(copied.size(), 1U);
    EXPECT_EQ(copied[0].task_id, "s1:calm");

    const auto done = answer(*engine, "t-fork", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"calm@t2", "ask=yes@t2"}));
    EXPECT_EQ(runs(probe, "calm"), 2) << "the fork replays calm instead of running it again";

    // The source is untouched and resumes too.
    const auto source = answer(*engine, "t", "yes");
    ASSERT_EQ(source.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(source), (std::multiset<std::string>{"calm@t2", "ask=yes@t2"}));
}

TEST_P(AbandonedPause, AnAbandonedPauseOfAPersistentSubgraphDoesNotBlockItsParent) {
    // The child of a PerThread subgraph runs with resume_if_exists, so a
    // parent run that is not a resume starts the child after the pause that
    // was abandoned in the child's own thread.
    const auto probe = unique_probe("subgraph");
    register_types();
    const std::shared_ptr<GraphEngine> child(
        GraphEngine::build(single(probe), config_with(nullptr)).release());
    const std::string type = "abandoned_pause.subgraph." + probe;
    NodeFactory::instance().register_type(
        type,
        [child](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                name, child, std::map<std::string, std::string>{{"turn", "turn"}},
                std::map<std::string, std::string>{{"results", "results"}},
                SubgraphPersistence::PerThread);
        },
        json{{"type", "object"}});
    auto parent = build(define("abandoned_pause_parent", json{{"inner", json{{"type", type}}}},
                               json::array({edge("__start__", "inner"), edge("inner", "__end__")})),
                        store());
    auto run = [&](const char* name) {
        RunConfig config;
        config.thread_id = "t";
        config.input     = json{{"turn", name}};
        return parent->run(config);
    };
    ASSERT_EQ(run("t1").status(), RunStatus::Interrupted);
    ASSERT_EQ(run("t2").status(), RunStatus::Interrupted);

    const auto done = answer(*parent, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"ask=yes@t2"}));
}

// The walk still refuses a chain that cannot be right. The cases below damage
// the later pause's ancestry in the store and expect the resume to fail before
// any node runs, as for a chain inside one step.
class AbandonedPauseAncestry : public ::testing::Test {
protected:
    // P1 abandoned, P2 on top of it; returns P2 as stored.
    Checkpoint two_pauses(const std::string& probe) {
        engine_ = build(entry_fan_out(probe), store_);
        EXPECT_EQ(turn(*engine_, "t", "t1").status(), RunStatus::Interrupted);
        EXPECT_EQ(turn(*engine_, "t", "t2").status(), RunStatus::Interrupted);
        auto p2 = store_->load_latest("t");
        EXPECT_TRUE(p2);
        auto p1 = store_->load_by_id(p2->parent_id);
        EXPECT_TRUE(p1);
        return *p2;
    }
    // Both walks refuse P2 with `message`: the blocking one and the one that
    // an exact resume of P2 makes. P1 is saved again by these cases, which
    // makes it the thread's latest checkpoint, so P2 is resumed by its id.
    void expect_refused(const std::string& probe, const Checkpoint& p2, const std::string& message) {
        const int before = runs(probe, "ask") + runs(probe, "calm");
        try {
            (void)resume_pending_writes(*store_, p2);
            ADD_FAILURE() << "the blocking walk accepted the chain";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find(message), std::string::npos) << error.what();
        }
        RunConfig config;
        config.thread_id = "t";
        try {
            (void)engine_->resume_from(config, p2.id, json("yes"));
            ADD_FAILURE() << "the resume accepted the chain";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find(message), std::string::npos) << error.what();
        }
        EXPECT_EQ(runs(probe, "ask") + runs(probe, "calm"), before) << "no node ran";
    }

    std::shared_ptr<InMemoryCheckpointStore> store_ = std::make_shared<InMemoryCheckpointStore>();
    std::unique_ptr<GraphEngine>             engine_;
};

TEST_F(AbandonedPauseAncestry, AParentThatIsNotInTheStoreIsRefused) {
    const auto probe = unique_probe("missing");
    auto       p2    = two_pauses(probe);
    p2.parent_id     = "no-such-checkpoint";
    store_->save(p2);
    expect_refused(probe, p2, "Missing or incompatible parent checkpoint");
}

TEST_F(AbandonedPauseAncestry, AParentOfAnotherThreadIsRefused) {
    const auto probe = unique_probe("foreign");
    auto       p2    = two_pauses(probe);
    auto       p1    = *store_->load_by_id(p2.parent_id);
    p1.thread_id     = "other";
    store_->save(p1);
    expect_refused(probe, p2, "Missing or incompatible parent checkpoint");
}

TEST_F(AbandonedPauseAncestry, AParentOfAnotherSchemaIsRefused) {
    const auto probe = unique_probe("schema");
    auto       p2    = two_pauses(probe);
    auto       p1    = *store_->load_by_id(p2.parent_id);
    p1.schema_version += 1;
    store_->save(p1);
    expect_refused(probe, p2, "Missing or incompatible parent checkpoint");
}

TEST_F(AbandonedPauseAncestry, AParentPauseOfALaterStepIsRefused) {
    // Checkpoints of a thread are saved in step order; a pause whose parent is
    // a pause of a later step is damaged, not an abandoned pause.
    const auto probe = unique_probe("backwards");
    auto       p2    = two_pauses(probe);
    auto       p1    = *store_->load_by_id(p2.parent_id);
    p1.step          = p2.step + 3;
    store_->save(p1);
    expect_refused(probe, p2, "runs backwards across super-steps");
}

TEST_F(AbandonedPauseAncestry, TwoPausesOfOneStepThatNameEachOtherAsParentAreRefused) {
    // A chain of pauses of one step that loops back on itself would never end.
    const auto probe = unique_probe("cycle");
    auto       p2    = two_pauses(probe);
    auto       p1    = *store_->load_by_id(p2.parent_id);
    p1.step          = p2.step;
    p1.parent_id     = p2.id;
    store_->save(p1);
    expect_refused(probe, p2, "Cycle in NodeInterrupt checkpoint ancestry");
}

TEST_F(AbandonedPauseAncestry, TheWalkEndsAtTheAbandonedPause) {
    // The writes recorded under the abandoned pause for P2's step are read and
    // the walk stops there: a row of P2's step under a checkpoint further back
    // is not P2's.
    const auto probe  = unique_probe("ends-at");
    auto       p2     = two_pauses(probe);
    auto       p1     = *store_->load_by_id(p2.parent_id);
    Checkpoint before = p1;
    before.id         = Checkpoint::generate_id();
    before.parent_id  = "";
    before.interrupt_phase = CheckpointPhase::Completed;
    store_->save(before);
    p1.parent_id = before.id;
    store_->save(p1);
    PendingWrite stray;
    stray.task_id   = "s1:stray";
    stray.task_path = "s1:stray";
    stray.node_name = "calm";
    stray.writes    = json::array();
    stray.step      = p2.step;
    stray.timestamp = 0;
    store_->put_writes("t", before.id, stray);

    EXPECT_EQ(task_ids_of(resume_pending_writes(*store_, p2)), (std::set<std::string>{"s1:calm"}));
}

INSTANTIATE_TEST_SUITE_P(Stores, AbandonedPause, ::testing::ValuesIn(test_support::backends()),
                         [](const auto& info) { return test_support::backend_name(info.param); });
