// #604: the checkpoint stores and the resume assume that the history of a
// thread is one line.
//
// A channel value is stored once per (thread, channel, version) and the value
// stored first is kept; the rows of a task's pending write are appended and a
// resume replays one row per task id. Both are right while a thread only grows.
// They are wrong as soon as the history of a thread branches: a new run on a
// thread that already has checkpoints without resume_if_exists, a resume of an
// older checkpoint, or a retry of a step that an earlier attempt left
// unfinished. The versions of the new branch start again from the checkpoint it
// continues, so another branch has used them, and the task ids are the same
// because they are built from the step and the node.
//
// Pending writes: a resume replays the newest row of a task id, which is the
// row of the attempt whose pause it resumes. The cases of the pending-writes
// sections of this file pin that, and run by default.
//
// Channel values: the channel-value half of #604 is open. The cases named
// Defect... state what a store has to do when two checkpoints of a thread use
// the same channel version, and fail on the stores of this tree. They are
// skipped unless NEOGRAPH_TEST_LINEAR_HISTORY is set to 1, so that CI stays
// green while the stores keep one value per (thread, channel, version):
//
//     NEOGRAPH_TEST_LINEAR_HISTORY=1 ./neograph_linear_history_tests
//
// The change that fixes the channel values deletes REQUIRE_LINEAR_HISTORY_FIX()
// and the helper behind it, which turns those cases on. The other cases pass
// today and have to keep passing: they pin the behaviour of a thread that only
// grows and of a fork, which is the supported way to branch.

#include "fixtures/checkpoint_backends.h"
#include "fixtures/unique_temp_dir.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/coordinator.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

bool linear_history_cases_enabled() {
    const char* value = std::getenv("NEOGRAPH_TEST_LINEAR_HISTORY");
    return value != nullptr && std::string(value) == "1";
}

#define REQUIRE_LINEAR_HISTORY_FIX()                                                   \
    do {                                                                               \
        if (!linear_history_cases_enabled())                                           \
            GTEST_SKIP() << "#604: the channel-value half is open (the stores keep "   \
                            "one value per channel version); set "                     \
                            "NEOGRAPH_TEST_LINEAR_HISTORY=1 to run";                   \
    } while (false)

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

// Asks until it is answered. Its result carries the answer and the turn, so a
// value of another branch is recognisable.
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

// Asks in the turn its configuration names, and finishes at once with its name
// and the turn in every other: a step that has a task in one attempt and not in
// another.
class AskOnTurnNode final : public GraphNode {
public:
    AskOnTurnNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        const auto turn = turn_of(in);
        if (turn == config_.value("turn", std::string()) && !in.ctx.resume_value)
            throw NodeInterrupt("approve " + name_, json{{"asks", name_}, {"turn", turn}});
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({name_ + "@" + turn})});
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
            "linear_history.ask",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<AskNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "linear_history.ask_on_turn",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<AskOnTurnNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "linear_history.plain",
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
    return define("linear_history_single", json{{"ask", typed(probe, "linear_history.ask")}},
                  json::array({edge("__start__", "ask"), edge("ask", "__end__")}));
}

// ask and calm in the first super-step: calm finishes, ask pauses. No
// checkpoint precedes the step, so calm's write is recorded under the
// checkpoint the run continues from, and under the pause when the thread was
// empty.
json fan_out(const std::string& probe) {
    return define("linear_history_fan_out",
                  json{{"ask", typed(probe, "linear_history.ask")},
                       {"calm", typed(probe, "linear_history.plain")}},
                  json::array({edge("__start__", "ask"), edge("__start__", "calm"),
                               edge("ask", "__end__"), edge("calm", "__end__")}));
}

// ask, calm and late in the first super-step. ask always pauses and calm
// finishes; late pauses in turn "t2" and finishes in any other.
json fan_out_late(const std::string& probe) {
    json late = typed(probe, "linear_history.ask_on_turn");
    late["turn"] = "t2";
    return define("linear_history_fan_out_late",
                  json{{"ask", typed(probe, "linear_history.ask")},
                       {"calm", typed(probe, "linear_history.plain")},
                       {"late", late}},
                  json::array({edge("__start__", "ask"), edge("__start__", "calm"),
                               edge("__start__", "late"), edge("ask", "__end__"),
                               edge("calm", "__end__"), edge("late", "__end__")}));
}

template <typename Store>
class FailNextSave final : public Store {
public:
    using Store::Store;
    void arm() { armed_.store(true); }
    void save(const Checkpoint& cp) override {
        maybe_fail();
        Store::save(cp);
    }
    asio::awaitable<void> save_async(const Checkpoint& cp) override {
        maybe_fail();
        co_await Store::save_async(cp);
    }

private:
    void maybe_fail() {
        if (armed_.exchange(false)) throw std::runtime_error("injected checkpoint save failure");
    }
    std::atomic<bool> armed_{false};
};

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

std::multiset<std::string> results_in(const json& channel_values) {
    std::multiset<std::string> out;
    const auto                 values = channel_values["channels"]["results"]["value"];
    for (std::size_t index = 0; index < values.size(); ++index)
        out.insert(values[index].get<std::string>());
    return out;
}

std::multiset<std::string> results_of(const RunResult& result) { return results_in(result.output); }

// A new run on a thread. With resume_if_exists it is the next turn of a
// conversation: the state is seeded from the thread's latest checkpoint. Without
// it the run starts from nothing, as a host that reuses a thread id does.
RunResult run_turn(GraphEngine& engine, const std::string& thread, const std::string& name,
                   bool resume_if_exists) {
    RunConfig config;
    config.thread_id        = thread;
    config.resume_if_exists = resume_if_exists;
    config.input            = json{{"turn", name}};
    return engine.run(config);
}

// A run whose input writes channels in the order of the object, each write
// counting once: "pad" before "turn" puts "turn" one version later.
RunResult run_input(GraphEngine& engine, const std::string& thread, const json& input,
                    bool resume_if_exists) {
    RunConfig config;
    config.thread_id        = thread;
    config.resume_if_exists = resume_if_exists;
    config.input            = input;
    return engine.run(config);
}

RunResult answer(GraphEngine& engine, const std::string& thread, const std::string& value) {
    RunConfig config;
    config.thread_id = thread;
    return engine.resume(config, json(value));
}

RunResult answer_from(GraphEngine& engine, const std::string& thread,
                      const std::string& checkpoint_id, const std::string& value) {
    RunConfig config;
    config.thread_id = thread;
    return engine.resume_from(config, checkpoint_id, json(value));
}

// A checkpoint with one channel at a given version, as the engine's own
// serialisation shapes it.
Checkpoint cp_with(const std::string& thread, const std::string& id, const std::string& parent,
                   const std::string& value, std::uint64_t version) {
    Checkpoint cp;
    cp.id        = id;
    cp.thread_id = thread;
    cp.parent_id = parent;
    cp.next_nodes = {"__end__"};
    cp.interrupt_phase = CheckpointPhase::Completed;
    cp.step      = 0;
    cp.timestamp = 1;
    cp.channel_values = json{{"channels", json{{"x", json{{"value", value}, {"version", version}}}}},
                             {"global_version", version}};
    return cp;
}

std::string x_of(const Checkpoint& cp) {
    return cp.channel_values["channels"]["x"]["value"].get<std::string>();
}

// The number of channel values a store keeps, where the store can say.
std::optional<std::size_t> blob_count_of(CheckpointStore& store) {
    if (auto* memory = dynamic_cast<InMemoryCheckpointStore*>(&store)) return memory->blob_count();
    if (auto* sqlite = dynamic_cast<SqliteCheckpointStore*>(&store)) return sqlite->blob_count();
    return std::nullopt;
}

// A row of the progress log for `task_id`. `tag` tells a test which attempt
// wrote it.
PendingWrite row_of(const std::string& task_id, const std::string& tag, std::int64_t step) {
    PendingWrite write;
    write.task_id   = task_id;
    write.task_path = task_id;
    write.node_name = task_id.substr(task_id.find(':') + 1);
    write.writes    = json::array({json{{"channel", "results"}, {"value", tag}}});
    write.command   = json();
    write.sends     = json::array();
    write.step      = step;
    write.timestamp = 1;
    return write;
}

std::map<std::string, std::string> tags_of(const std::vector<PendingWrite>& rows) {
    std::map<std::string, std::string> tags;
    for (const auto& row : rows) {
        EXPECT_TRUE(tags.emplace(row.task_id, row.writes[0]["value"].get<std::string>()).second)
            << "two rows for " << row.task_id;
    }
    return tags;
}

std::map<std::string, std::string> tags_of(const ResumeContext& context) {
    std::map<std::string, std::string> tags;
    for (const auto& [task_id, result] : context.replay_results)
        tags.emplace(task_id, result.writes.at(0).value.get<std::string>());
    return tags;
}

// A pause of super-step 1 whose step started from the Completed checkpoint
// "root" of thread "t". The rows of the step are recorded under "root".
struct PausedStep {
    Checkpoint root;
    Checkpoint pause;
};

PausedStep paused_step() {
    PausedStep step;
    step.root            = cp_with("t", "root", "", "root", 1);
    step.root.next_nodes = {"ask", "calm", "late", "other"};
    step.pause           = cp_with("t", "pause", "root", "pause", 2);
    step.pause.interrupt_phase = CheckpointPhase::NodeInterrupt;
    step.pause.step            = 1;
    step.pause.next_nodes      = {"ask", "calm", "late", "other"};
    return step;
}

using test_support::Backend;

class LinearHistory : public ::testing::TestWithParam<Backend> {
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
    std::multiset<std::string> stored_results(const std::string& id) {
        return results_in(saved(id).channel_values);
    }
    std::multiset<std::string> latest_results(const std::string& thread) {
        auto latest = store()->load_latest(thread);
        EXPECT_TRUE(latest) << "no checkpoint for " << thread;
        return latest ? results_in(latest->channel_values) : std::multiset<std::string>{};
    }

private:
    test_support::UniqueTempDir      dir_{"ng-linear-history-"};
    std::shared_ptr<CheckpointStore> store_;
};

}  // namespace

// ── What holds today and has to keep holding ────────────────────────────────

TEST_P(LinearHistory, AThreadThatOnlyGrowsReadsBackWhatItStored) {
    const auto probe  = unique_probe("linear");
    auto       engine = build(fan_out(probe), store());
    const auto first  = run_turn(*engine, "t", "t1", true);
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto done1 = answer(*engine, "t", "yes");
    ASSERT_EQ(done1.status(), RunStatus::Completed);
    EXPECT_EQ(stored_results(done1.checkpoint_id), results_of(done1));

    const auto second = run_turn(*engine, "t", "t2", true);
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    const auto done2 = answer(*engine, "t", "no");
    ASSERT_EQ(done2.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done2),
              (std::multiset<std::string>{"calm@t1", "ask=yes@t1", "calm@t2", "ask=no@t2"}));
    EXPECT_EQ(stored_results(done2.checkpoint_id), results_of(done2));
    EXPECT_EQ(latest_results("t"), results_of(done2));
}

TEST_P(LinearHistory, AForkIsABranchOfItsOwn) {
    // The supported way to branch: a fork has a thread of its own, and a store
    // keys its values by thread.
    const auto probe  = unique_probe("fork");
    auto       engine = build(fan_out(probe), store());
    ASSERT_EQ(run_turn(*engine, "t", "t1", true).status(), RunStatus::Interrupted);
    const auto forked = engine->fork("t", "t-fork");
    EXPECT_FALSE(forked.empty());

    const auto left  = answer(*engine, "t", "left");
    const auto right = answer(*engine, "t-fork", "right");
    ASSERT_EQ(left.status(), RunStatus::Completed);
    ASSERT_EQ(right.status(), RunStatus::Completed);
    EXPECT_EQ(stored_results(left.checkpoint_id),
              (std::multiset<std::string>{"calm@t1", "ask=left@t1"}));
    EXPECT_EQ(stored_results(right.checkpoint_id),
              (std::multiset<std::string>{"calm@t1", "ask=right@t1"}));
    EXPECT_EQ(latest_results("t"), stored_results(left.checkpoint_id));
    EXPECT_EQ(latest_results("t-fork"), stored_results(right.checkpoint_id));
}

TEST_P(LinearHistory, AValueThatDoesNotChangeIsStoredOnce) {
    // Dedup is what a fix must not give up: a thread that only grows keeps one
    // value per (channel, version), however many checkpoints hold it.
    auto  backend = store();
    const auto first = cp_with("t", "c1", "", "same", 7);
    auto second      = cp_with("t", "c2", "c1", "same", 7);
    second.step      = 1;
    backend->save(first);
    backend->save(second);
    EXPECT_EQ(x_of(saved("c1")), "same");
    EXPECT_EQ(x_of(saved("c2")), "same");
    if (const auto blobs = blob_count_of(*backend)) EXPECT_EQ(*blobs, 1U);
}

TEST_P(LinearHistory, PendingWritesAreKeptInOrderPerParent) {
    // The store keeps every row, oldest first; which row of a task id a resume
    // uses is the engine's choice, not the store's (it replays the newest).
    auto backend = store();
    PendingWrite write;
    write.task_id   = "s1:calm";
    write.task_path = "s1:calm";
    write.node_name = "calm";
    write.writes    = json::array();
    write.step      = 1;
    write.timestamp = 1;
    write.command   = json();
    write.sends     = json::array();
    for (const char* tag : {"old", "new"}) {
        write.writes = json::array({json{{"channel", "results"}, {"value", tag}}});
        backend->put_writes("t", "parent", write);
    }
    const auto rows = backend->get_writes("t", "parent");
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_NE(rows[0].writes.dump().find("old"), std::string::npos);
    EXPECT_NE(rows[1].writes.dump().find("new"), std::string::npos);
    EXPECT_TRUE(backend->get_writes("t", "other-parent").empty());
    EXPECT_TRUE(backend->get_writes("other-thread", "parent").empty());
}

// ── Pending writes: which row of a task id a resume replays ─────────────────
//
// A step that was started again from the same checkpoint (an attempt failed or
// was cancelled before it saved its pause, and the host ran the turn again)
// leaves the rows of every attempt under that checkpoint, in the order they
// were recorded. A resume replays the newest row of each task id, the one of
// the attempt whose pause it resumes, and the nearest checkpoint of the walk
// still decides first.

TEST_P(LinearHistory, EveryReadOfAResumeReplaysTheNewestRowOfATaskId) {
    auto backend = store();
    auto step    = paused_step();
    backend->save(step.root);
    backend->save(step.pause);
    // Two attempts of step 1 from "root". calm and late finished in the first;
    // calm and other in the second, in which late did not finish.
    for (const auto& [task, tag] : std::vector<std::pair<const char*, const char*>>{
             {"s1:calm", "attempt-1"}, {"s1:late", "attempt-1"},
             {"s1:calm", "attempt-2"}, {"s1:other", "attempt-2"}})
        backend->put_writes("t", "root", row_of(task, tag, 1));
    ASSERT_EQ(backend->get_writes("t", "root").size(), 4U);

    const std::map<std::string, std::string> expected{
        {"s1:calm", "attempt-2"}, {"s1:late", "attempt-1"}, {"s1:other", "attempt-2"}};

    EXPECT_EQ(tags_of(resume_pending_writes(*backend, step.pause)), expected)
        << "the walk of fork";
    CheckpointCoordinator coordinator(backend, "t");
    const auto            latest = coordinator.load_for_resume();
    ASSERT_EQ(latest.checkpoint_id, "pause");
    EXPECT_EQ(tags_of(latest), expected) << "the blocking resume";
    EXPECT_EQ(tags_of(neograph::async::run_sync(coordinator.load_for_resume_async())), expected)
        << "the awaitable resume of the latest checkpoint";
    EXPECT_EQ(tags_of(neograph::async::run_sync(coordinator.load_for_resume_by_id_async("pause"))),
              expected)
        << "the awaitable resume by checkpoint id";

    // What fork carries over to the copy is the same selection, one row per task id.
    Checkpoint copy = step.pause;
    copy.id         = "pause-copy";
    copy.thread_id  = "t-copy";
    copy.parent_id  = "";
    backend->save(copy);
    copy_resume_pending_writes(*backend, step.pause, copy);
    EXPECT_EQ(tags_of(backend->get_writes("t-copy", "pause-copy")), expected);
}

TEST_P(LinearHistory, ANearerCheckpointKeepsItsRowOverANewerRowOfAnAncestor) {
    // The walk goes from the resumed checkpoint to its ancestors of the same
    // step, nearest first. A row under the pause itself decides before any row
    // under the checkpoint the step started from, whichever was recorded last.
    auto backend = store();
    auto step    = paused_step();
    backend->save(step.root);
    backend->save(step.pause);
    backend->put_writes("t", "pause", row_of("s1:calm", "under-the-pause", 1));
    backend->put_writes("t", "root", row_of("s1:calm", "under-the-root", 1));
    backend->put_writes("t", "root", row_of("s1:late", "under-the-root", 1));

    const std::map<std::string, std::string> expected{{"s1:calm", "under-the-pause"},
                                                      {"s1:late", "under-the-root"}};
    EXPECT_EQ(tags_of(resume_pending_writes(*backend, step.pause)), expected);
    CheckpointCoordinator coordinator(backend, "t");
    EXPECT_EQ(tags_of(neograph::async::run_sync(coordinator.load_for_resume_by_id_async("pause"))),
              expected);
}

TEST_P(LinearHistory, AResumeOfACompletedCheckpointReplaysTheNewestRowOfATaskId) {
    // A checkpoint that is not a pause is resumed with every row under its id,
    // and the newest row of a task id is the one it replays. This is also what
    // resuming the checkpoint that two attempts of a step both started from
    // does: the attempt that finished last.
    auto backend = store();
    auto step    = paused_step();
    backend->save(step.root);
    for (const auto& [task, tag] : std::vector<std::pair<const char*, const char*>>{
             {"s1:calm", "attempt-1"}, {"s1:other", "attempt-1"}, {"s1:calm", "attempt-2"}})
        backend->put_writes("t", "root", row_of(task, tag, 1));

    const std::map<std::string, std::string> expected{{"s1:calm", "attempt-2"},
                                                      {"s1:other", "attempt-1"}};
    EXPECT_EQ(tags_of(resume_pending_writes(*backend, step.root)), expected);
    CheckpointCoordinator coordinator(backend, "t");
    EXPECT_EQ(tags_of(coordinator.load_for_resume()), expected);
    EXPECT_EQ(tags_of(neograph::async::run_sync(coordinator.load_for_resume_by_id_async("root"))),
              expected);
}

// ── Channel values: a version that two branches of a thread both use ────────

TEST_P(LinearHistory, DefectAReusedVersionKeepsTheValueOfEachCheckpoint) {
    REQUIRE_LINEAR_HISTORY_FIX();
    // The smallest form of the defect, with no engine: two checkpoints of one
    // thread that are not on one line both hold channel x at version 7.
    auto backend = store();
    backend->save(cp_with("t", "branch-a", "root", "from a", 7));
    backend->save(cp_with("t", "branch-b", "root", "from b", 7));
    EXPECT_EQ(x_of(saved("branch-a")), "from a");
    EXPECT_EQ(x_of(saved("branch-b")), "from b");
    EXPECT_EQ(x_of(*backend->load_latest("t")), "from b");
    const auto listed = backend->list("t");
    ASSERT_EQ(listed.size(), 2U);
    EXPECT_EQ(x_of(listed[0]), "from b");
    EXPECT_EQ(x_of(listed[1]), "from a");
}

TEST_P(LinearHistory, DefectASavedAgainCheckpointReadsBackItsNewValue) {
    REQUIRE_LINEAR_HISTORY_FIX();
    // CheckpointStore::save: saving an ID the thread already holds replaces that
    // checkpoint. Its channel keeps its version when only the value changes.
    auto backend = store();
    backend->save(cp_with("t", "c1", "", "before", 3));
    backend->save(cp_with("t", "c1", "", "after", 3));
    EXPECT_EQ(x_of(saved("c1")), "after");
}

TEST_P(LinearHistory, DeletingAThreadKeepsTheValuesOfTheOtherThreads) {
    // delete_thread removes the values of its own thread only, whatever number
    // of values a version holds. A fix that keeps more than one value per
    // version has to remove all of them here and none of another thread's.
    auto backend = store();
    backend->save(cp_with("t", "branch-a", "root", "from a", 7));
    backend->save(cp_with("t", "branch-b", "root", "from b", 7));
    backend->save(cp_with("other", "elsewhere", "", "elsewhere", 7));
    backend->delete_thread("t");
    EXPECT_FALSE(backend->load_by_id("branch-a"));
    EXPECT_FALSE(backend->load_by_id("branch-b"));
    ASSERT_TRUE(backend->load_by_id("elsewhere"));
    EXPECT_EQ(x_of(saved("elsewhere")), "elsewhere");
    if (const auto blobs = blob_count_of(*backend)) EXPECT_EQ(*blobs, 1U);
}

TEST_P(LinearHistory, DefectARunWithoutResumeIfExistsKeepsItsOwnValues) {
    REQUIRE_LINEAR_HISTORY_FIX();
    // The steps of #604: a run pauses; another run starts on the same thread
    // without resume_if_exists and pauses; the second pause is resumed. The
    // second run counts its channel versions from 0 again, so its "turn" is at
    // the version the first run used for its own.
    const auto probe  = unique_probe("restart");
    auto       engine = build(single(probe), store());
    const auto first  = run_turn(*engine, "t", "t1", false);
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto second = run_turn(*engine, "t", "t2", false);
    ASSERT_EQ(second.status(), RunStatus::Interrupted);

    const auto p1 = saved(first.checkpoint_id);
    const auto p2 = saved(second.checkpoint_id);
    EXPECT_EQ(p1.channel_values["channels"]["turn"]["value"], json("t1"));
    EXPECT_EQ(p2.channel_values["channels"]["turn"]["value"], json("t2"))
        << "the stored pause of the second run holds the first run's turn";

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"ask=yes@t2"}));
    EXPECT_EQ(stored_results(done.checkpoint_id), results_of(done));
}

TEST_P(LinearHistory, DefectTwoAnswersToOnePauseKeepTheirOwnResults) {
    REQUIRE_LINEAR_HISTORY_FIX();
    // Time travel: the same pause is resumed by its ID with two answers. Both
    // branches continue from the one checkpoint, so they write "results" at the
    // same version.
    const auto probe  = unique_probe("two-answers");
    auto       engine = build(single(probe), store());
    const auto paused = run_turn(*engine, "t", "t1", false);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);

    const auto left = answer_from(*engine, "t", paused.checkpoint_id, "left");
    ASSERT_EQ(left.status(), RunStatus::Completed);
    const auto right = answer_from(*engine, "t", paused.checkpoint_id, "right");
    ASSERT_EQ(right.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(left), (std::multiset<std::string>{"ask=left@t1"}));
    EXPECT_EQ(results_of(right), (std::multiset<std::string>{"ask=right@t1"}));

    EXPECT_EQ(stored_results(left.checkpoint_id), (std::multiset<std::string>{"ask=left@t1"}));
    EXPECT_EQ(stored_results(right.checkpoint_id), (std::multiset<std::string>{"ask=right@t1"}))
        << "the stored result of the second branch holds the first branch's";
    EXPECT_EQ(latest_results("t"), (std::multiset<std::string>{"ask=right@t1"}));
}

TEST_P(LinearHistory, AnOlderPauseResumedByIdAfterAContinuedTurnKeepsItsOwnValues) {
    // Turns that continue the thread chain their checkpoints, so the second
    // turn starts at the counter the first one reached and the two pauses
    // share no version. Resuming the older one by its ID is not a collision.
    const auto probe  = unique_probe("continued");
    auto       engine = build(fan_out(probe), store());
    const auto p1     = run_turn(*engine, "t", "t1", true);
    ASSERT_EQ(p1.status(), RunStatus::Interrupted);
    const auto p2 = run_turn(*engine, "t", "t2", true);
    ASSERT_EQ(p2.status(), RunStatus::Interrupted);

    const auto later = answer_from(*engine, "t", p2.checkpoint_id, "later");
    ASSERT_EQ(later.status(), RunStatus::Completed);
    const auto earlier = answer_from(*engine, "t", p1.checkpoint_id, "earlier");
    ASSERT_EQ(earlier.status(), RunStatus::Completed);
    EXPECT_EQ(stored_results(later.checkpoint_id), results_of(later));
    EXPECT_EQ(stored_results(earlier.checkpoint_id), results_of(earlier));
}

TEST_P(LinearHistory, DefectANewTurnThenAnOlderPauseResumedByIdKeepsEachResult) {
    REQUIRE_LINEAR_HISTORY_FIX();
    // The second case of #604: a new turn is run on the thread (without
    // resume_if_exists, so it counts from 0 again) and an older pause is then
    // resumed by its ID. Each resumed run returns the right result; the
    // checkpoint it stores holds the value of the other branch.
    //
    // The two turns write "pad" and "turn" in the other order, so that "turn"
    // is at a different version in each pause and the pauses themselves read
    // back right. Only the "results" both branches write next, at version 3,
    // are shared.
    const auto probe  = unique_probe("new-turn");
    auto       engine = build(single(probe), store());
    const auto p1 = run_input(*engine, "t", json{{"pad", "p"}, {"turn", "t1"}}, false);
    ASSERT_EQ(p1.status(), RunStatus::Interrupted);
    const auto p2 = run_input(*engine, "t", json{{"turn", "t2"}, {"pad", "p"}}, false);
    ASSERT_EQ(p2.status(), RunStatus::Interrupted);
    ASSERT_EQ(saved(p1.checkpoint_id).channel_values["channels"]["turn"]["value"], json("t1"));
    ASSERT_EQ(saved(p2.checkpoint_id).channel_values["channels"]["turn"]["value"], json("t2"));

    const auto first = answer_from(*engine, "t", p1.checkpoint_id, "no");
    ASSERT_EQ(first.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(first), (std::multiset<std::string>{"ask=no@t1"}));
    const auto second = answer_from(*engine, "t", p2.checkpoint_id, "yes");
    ASSERT_EQ(second.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(second), (std::multiset<std::string>{"ask=yes@t2"}))
        << "the result of the run itself is right";

    EXPECT_EQ(stored_results(first.checkpoint_id), (std::multiset<std::string>{"ask=no@t1"}));
    EXPECT_EQ(stored_results(second.checkpoint_id), (std::multiset<std::string>{"ask=yes@t2"}))
        << "the stored result of the second branch holds the first branch's";
}

TEST_P(LinearHistory, DefectTheNextTurnContinuesFromTheBranchThatWasResumedLast) {
    REQUIRE_LINEAR_HISTORY_FIX();
    // The thread goes on after a time-travel resume: the next turn seeds its
    // state from the latest checkpoint, the branch that was resumed last.
    const auto probe  = unique_probe("goes-on");
    auto       engine = build(single(probe), store());
    const auto paused = run_turn(*engine, "t", "t1", false);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    ASSERT_EQ(answer_from(*engine, "t", paused.checkpoint_id, "left").status(), RunStatus::Completed);
    const auto right = answer_from(*engine, "t", paused.checkpoint_id, "right");
    ASSERT_EQ(right.status(), RunStatus::Completed);

    const auto next = run_turn(*engine, "t", "t2", true);
    ASSERT_EQ(next.status(), RunStatus::Interrupted);
    const auto done = answer(*engine, "t", "last");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"ask=right@t1", "ask=last@t2"}));
    EXPECT_EQ(stored_results(done.checkpoint_id), results_of(done));
}

// A subgraph's thread belongs to its parent run. Legacy gives it a name built
// from the parent's thread, step and task, which a second run of the parent
// builds again; PerInvocation adds an identity that is new for each run.
static std::unique_ptr<GraphEngine> build_with_subgraph(const std::string& probe,
                                                 SubgraphPersistence persistence,
                                                 std::shared_ptr<CheckpointStore> store) {
    register_types();
    const std::shared_ptr<GraphEngine> child(
        GraphEngine::build(single(probe), config_with(nullptr)).release());
    const std::string type = "linear_history.subgraph." + probe;
    NodeFactory::instance().register_type(
        type,
        [child, persistence](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                name, child, std::map<std::string, std::string>{{"turn", "turn"}},
                std::map<std::string, std::string>{{"results", "results"}}, persistence);
        },
        json{{"type", "object"}});
    return build(define("linear_history_parent", json{{"inner", json{{"type", type}}}},
                        json::array({edge("__start__", "inner"), edge("inner", "__end__")})),
                 std::move(store));
}

TEST_P(LinearHistory, APerInvocationSubgraphKeepsTheValuesOfASecondParentRun) {
    const auto probe  = unique_probe("sub-invocation");
    auto       engine = build_with_subgraph(probe, SubgraphPersistence::PerInvocation, store());
    ASSERT_EQ(run_turn(*engine, "t", "t1", false).status(), RunStatus::Interrupted);
    ASSERT_EQ(run_turn(*engine, "t", "t2", false).status(), RunStatus::Interrupted);
    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"ask=yes@t2"}));
}

TEST_P(LinearHistory, DefectALegacySubgraphKeepsTheValuesOfASecondParentRun) {
    REQUIRE_LINEAR_HISTORY_FIX();
    // The second parent run starts the child again on the child's thread of the
    // first run, which holds checkpoints, so the collision is in the child.
    const auto probe  = unique_probe("sub-legacy");
    auto       engine = build_with_subgraph(probe, SubgraphPersistence::Legacy, store());
    ASSERT_EQ(run_turn(*engine, "t", "t1", false).status(), RunStatus::Interrupted);
    ASSERT_EQ(run_turn(*engine, "t", "t2", false).status(), RunStatus::Interrupted);
    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"ask=yes@t2"}));
}

// ── Pending writes: the row of another attempt at the same step ─────────────

namespace {

// How the pause of the retried step is answered. resume and resume_from run
// the awaitable walk; fork copies the rows with the blocking one.
enum class Answer { Resume, ResumeFrom, ForkThenResume };

const char* answer_name(Answer how) {
    switch (how) {
    case Answer::Resume: return "resume";
    case Answer::ResumeFrom: return "resume_from";
    case Answer::ForkThenResume: return "fork then resume";
    }
    return "";
}

// A thread that fails to save the pause of a step, and the retry of that step.
struct RetriedStep {
    std::string parent;  // the checkpoint both attempts continued from
    std::string pause;   // the pause of the retry
};

}  // namespace

class LinearHistoryAttempts : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() override {
        if (GetParam() != Backend::InMemory && GetParam() != Backend::Sqlite)
            GTEST_SKIP() << "the failing store wraps the in-memory and the SQLite store";
    }
    // Opens the failing store of the backend.
    std::shared_ptr<CheckpointStore> open(std::function<void()>& arm) {
        if (GetParam() == Backend::InMemory) {
            auto store = std::make_shared<FailNextSave<InMemoryCheckpointStore>>();
            arm        = [store] { store->arm(); };
            return store;
        }
        auto store = std::make_shared<FailNextSave<SqliteCheckpointStore>>(dir_.file("attempts.sqlite"));
        arm        = [store] { store->arm(); };
        return store;
    }

    // The thread is at a Completed checkpoint. A run starts on it (the next
    // turn), finishes calm and fails to save its pause: its row for task
    // "s<n>:calm" stays under the checkpoint it continued from, with the first
    // attempt's turn in it. The host starts the turn again. Its calm runs and
    // is recorded under the same parent with the same task id; its ask pauses.
    void retry_after_a_failed_attempt(GraphEngine& engine, CheckpointStore& backend,
                                      const std::function<void()>& arm, const std::string& thread,
                                      RetriedStep& out) {
        ASSERT_EQ(run_turn(engine, thread, "t1", true).status(), RunStatus::Interrupted);
        ASSERT_EQ(answer(engine, thread, "yes").status(), RunStatus::Completed);
        const auto parent = backend.load_latest(thread);
        ASSERT_TRUE(parent);
        ASSERT_EQ(parent->interrupt_phase, CheckpointPhase::Completed);

        arm();
        EXPECT_ANY_THROW((void)run_turn(engine, thread, "t2", true)) << "the pause could not be saved";
        ASSERT_EQ(backend.load_latest(thread)->id, parent->id) << "the failed attempt saved nothing";
        const auto left_behind = backend.get_writes(thread, parent->id);
        ASSERT_EQ(left_behind.size(), 1U) << "the failed attempt left its row for calm";
        EXPECT_NE(left_behind[0].writes.dump().find("calm@t2"), std::string::npos);

        const auto retried = run_turn(engine, thread, "t3", true);
        ASSERT_EQ(retried.status(), RunStatus::Interrupted);
        ASSERT_EQ(backend.load_by_id(retried.checkpoint_id)->parent_id, parent->id);
        out.parent = parent->id;
        out.pause  = retried.checkpoint_id;
    }

    // Answers the retry's pause the way `how` says. The fork is a thread of its own.
    RunResult answer_retry(GraphEngine& engine, const std::string& thread, const RetriedStep& retried,
                           Answer how) {
        switch (how) {
        case Answer::Resume: return answer(engine, thread, "yes");
        case Answer::ResumeFrom: return answer_from(engine, thread, retried.pause, "yes");
        case Answer::ForkThenResume: {
            const auto forked = thread + "-fork";
            EXPECT_FALSE(engine.fork(thread, forked, retried.pause).empty());
            return answer(engine, forked, "yes");
        }
        }
        return {};
    }

private:
    test_support::UniqueTempDir dir_{"ng-linear-history-attempts-"};
};

TEST_P(LinearHistoryAttempts, ARetriedStepDoesNotReplayTheRowOfTheAttemptThatFailed) {
    // The resume has to replay the row of the attempt that paused, not the
    // older one.
    const auto probe = unique_probe("attempts");
    std::function<void()> arm;
    auto                  backend = open(arm);
    auto                  engine  = build(fan_out(probe), backend);
    RetriedStep           retried;
    ASSERT_NO_FATAL_FAILURE(retry_after_a_failed_attempt(*engine, *backend, arm, "t", retried));
    ASSERT_EQ(backend->get_writes("t", retried.parent).size(), 2U)
        << "the rows of both attempts are under the parent";

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    const auto results = results_of(done);
    EXPECT_EQ(results.count("calm@t3"), 1U) << "the retry's calm is in the result";
    EXPECT_EQ(results.count("calm@t2"), 0U) << "the failed attempt's calm was replayed";
    EXPECT_EQ(results.count("ask=yes@t3"), 1U);
    EXPECT_EQ(runs(probe, "calm"), 3) << "the retry's calm is replayed, not run again";
}

TEST_P(LinearHistoryAttempts, EveryWayToAnswerTheRetriedPauseReplaysTheRowOfTheRetry) {
    // resume and resume_from read the rows through the awaitable walk; fork
    // copies them through the blocking one, and the copy is then resumed.
    for (const auto how : {Answer::Resume, Answer::ResumeFrom, Answer::ForkThenResume}) {
        SCOPED_TRACE(answer_name(how));
        const auto probe = unique_probe("attempts-ways");
        const auto thread = std::string("t-") + std::to_string(static_cast<int>(how));
        std::function<void()> arm;
        auto                  backend = open(arm);
        auto                  engine  = build(fan_out(probe), backend);
        RetriedStep           retried;
        ASSERT_NO_FATAL_FAILURE(retry_after_a_failed_attempt(*engine, *backend, arm, thread, retried));

        const auto done = answer_retry(*engine, thread, retried, how);
        ASSERT_EQ(done.status(), RunStatus::Completed);
        const auto results = results_of(done);
        EXPECT_EQ(results.count("calm@t3"), 1U) << "the retry's calm is in the result";
        EXPECT_EQ(results.count("calm@t2"), 0U) << "the failed attempt's calm was replayed";
        EXPECT_EQ(runs(probe, "calm"), 3) << "the retry's calm is replayed, not run again";
    }
}

TEST_P(LinearHistoryAttempts, ARetriedStepReplaysTheNewestRowOfEachTaskIdItHas) {
    // late pauses in the failed attempt and finishes in the retry, so under the
    // parent the stale row of calm comes first, followed by the retry's rows of
    // calm and of a task id the failed attempt has no row for.
    const auto probe = unique_probe("attempts-late");
    std::function<void()> arm;
    auto                  backend = open(arm);
    auto                  engine  = build(fan_out_late(probe), backend);
    RetriedStep           retried;
    ASSERT_NO_FATAL_FAILURE(retry_after_a_failed_attempt(*engine, *backend, arm, "t", retried));
    const auto rows = backend->get_writes("t", retried.parent);
    ASSERT_EQ(rows.size(), 3U) << "calm of the failed attempt, then calm and late of the retry";
    EXPECT_NE(rows[0].writes.dump().find("calm@t2"), std::string::npos);
    std::set<std::string> task_ids;
    for (const auto& row : rows) task_ids.insert(row.task_id);
    EXPECT_EQ(task_ids.size(), 2U);
    ASSERT_EQ(runs(probe, "calm"), 3);
    ASSERT_EQ(runs(probe, "late"), 3);

    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    const auto results = results_of(done);
    EXPECT_EQ(results.count("calm@t3"), 1U);
    EXPECT_EQ(results.count("late@t3"), 1U);
    EXPECT_EQ(results.count("calm@t2"), 0U) << "the failed attempt's calm was replayed";
    EXPECT_EQ(results.count("ask=yes@t3"), 1U);
    EXPECT_EQ(runs(probe, "calm"), 3) << "calm of the retry is replayed";
    EXPECT_EQ(runs(probe, "late"), 3) << "late of the retry is replayed";
}

TEST_P(LinearHistoryAttempts, ARetriedStepStillReplaysItsOwnFinishedSibling) {
    // What the rows are for, and what a fix has to keep: the attempt that
    // paused is resumed, and calm, which it finished, does not run again.
    const auto probe = unique_probe("replay");
    std::function<void()> arm;
    auto                  backend = open(arm);
    auto                  engine  = build(fan_out(probe), backend);
    ASSERT_EQ(run_turn(*engine, "t", "t1", true).status(), RunStatus::Interrupted);
    ASSERT_EQ(answer(*engine, "t", "yes").status(), RunStatus::Completed);
    ASSERT_EQ(runs(probe, "calm"), 1);

    const auto paused = run_turn(*engine, "t", "t2", true);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    ASSERT_EQ(runs(probe, "calm"), 2);
    const auto done = answer(*engine, "t", "yes");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "calm"), 2) << "the finished sibling is replayed, not run again";
    EXPECT_EQ(results_of(done).count("calm@t2"), 1U);
}

INSTANTIATE_TEST_SUITE_P(Stores, LinearHistory, ::testing::ValuesIn(test_support::backends()),
                         [](const auto& info) { return test_support::backend_name(info.param); });

INSTANTIATE_TEST_SUITE_P(Stores, LinearHistoryAttempts,
                         ::testing::Values(Backend::InMemory, Backend::Sqlite),
                         [](const auto& info) { return test_support::backend_name(info.param); });
