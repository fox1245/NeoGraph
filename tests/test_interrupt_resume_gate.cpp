// #382: only a resume that answers an interrupt skips that interrupt's gate.
//
// Resuming an interrupt_before pause runs the step it held, and resuming a
// NodeInterrupt re-enters a step that already passed its gate; neither asks
// again. Every other resume evaluates interrupt_before for the nodes it is
// about to run: recovery from a step's Completed checkpoint after the process
// stopped before the next step's interrupt was saved, a resume after
// interrupt_after, and a resume after an update made as a node.

#include "fixtures/unique_temp_dir.h"

#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

std::mutex&                 runs_mutex() { static std::mutex m; return m; }
std::map<std::string, int>& runs_map() { static std::map<std::string, int> m; return m; }

int runs(const std::string& probe, const std::string& node) {
    std::lock_guard lock(runs_mutex());
    const auto      found = runs_map().find(probe + "/" + node);
    return found == runs_map().end() ? 0 : found->second;
}

std::string unique_probe(const char* name) {
    static std::atomic<int> next{0};
    return std::string(name) + "-" + std::to_string(++next);
}

// Counts its runs per probe and appends its name to "trace". With "ask" it
// raises NodeInterrupt until it is resumed with a value.
class GateNode final : public GraphNode {
public:
    GateNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        {
            std::lock_guard lock(runs_mutex());
            ++runs_map()[config_.value("probe", std::string()) + "/" + name_];
        }
        if (config_.value("ask", false) && !in.ctx.resume_value)
            throw NodeInterrupt("approve " + name_);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"trace", json::array({name_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

void register_types() {
    static const bool registered = [] {
        NodeFactory::instance().register_type(
            "gate.node",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<GateNode>(name, config);
            },
            json{{"type", "object"}});
        return true;
    }();
    (void)registered;
}

json node(const std::string& probe, json extra = json::object()) {
    json config{{"type", "gate.node"}, {"probe", probe}};
    for (const auto& [key, value] : extra.items()) config[key] = value;
    return config;
}

// A -> ASK_USER, with interrupt_before ASK_USER and the given extras.
json approval_graph(const std::string& probe, json extra = json::object()) {
    json definition{
        {"name", "approval"},
        {"channels", json{{"trace", json{{"reducer", "append"}, {"initial", json::array()}}},
                          {"note", json{{"reducer", "overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{"A", node(probe)}, {"ASK_USER", node(probe)}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "A"}},
                               json{{"from", "A"}, {"to", "ASK_USER"}},
                               json{{"from", "ASK_USER"}, {"to", "__end__"}}})},
        {"interrupt_before", json::array({"ASK_USER"})}};
    for (const auto& [key, value] : extra.items()) definition[key] = value;
    return definition;
}

std::unique_ptr<GraphEngine> build(const json& definition, std::shared_ptr<CheckpointStore> store) {
    register_types();
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.stderr_notices   = false;
    return GraphEngine::build(definition, std::move(config));
}

class TempDir : public neograph::graph::test_support::UniqueTempDir {
public:
    TempDir() : UniqueTempDir("ng-interrupt-gate-") {}
};

// The process stops right before it would save an interrupt_before checkpoint:
// the step that led there has committed, the interrupt has not been saved.
class StopsBeforeTheInterruptSave final : public CheckpointStore {
public:
    explicit StopsBeforeTheInterruptSave(std::shared_ptr<CheckpointStore> inner)
        : inner_(std::move(inner)) {}

    void save(const Checkpoint& checkpoint) override {
        if (checkpoint.interrupt_phase == CheckpointPhase::Before)
            throw std::runtime_error("process lost before the interrupt save");
        inner_->save(checkpoint);
    }
    std::optional<Checkpoint> load_latest(const std::string& thread) override {
        return inner_->load_latest(thread);
    }
    std::optional<Checkpoint> load_by_id(const std::string& id) override { return inner_->load_by_id(id); }
    std::vector<Checkpoint> list(const std::string& thread, int limit) override {
        return inner_->list(thread, limit);
    }
    void delete_thread(const std::string& thread) override { inner_->delete_thread(thread); }
    void put_writes(const std::string& thread, const std::string& parent, const PendingWrite& write) override {
        inner_->put_writes(thread, parent, write);
    }
    std::vector<PendingWrite> get_writes(const std::string& thread, const std::string& parent) override {
        return inner_->get_writes(thread, parent);
    }
    void clear_writes(const std::string& thread, const std::string& parent) override {
        inner_->clear_writes(thread, parent);
    }
    bool requires_managed_budget(const std::string& thread) override {
        return inner_->requires_managed_budget(thread);
    }

private:
    std::shared_ptr<CheckpointStore> inner_;
};

}  // namespace

TEST(InterruptResumeGate, RecoveryFromTheCommitBeforeAnUnsavedInterruptStillAsks) {
    const TempDir dir;
    const auto    probe      = unique_probe("crash");
    const auto    definition = approval_graph(probe);
    const auto    file       = dir.file("checkpoints.sqlite");

    {  // Process 1: A's step commits; the process is gone before ASK_USER's interrupt is saved.
        auto engine = build(definition, std::make_shared<StopsBeforeTheInterruptSave>(
                                            std::make_shared<SqliteCheckpointStore>(file)));
        RunConfig run;
        run.thread_id = "crash";
        EXPECT_THROW((void)engine->run(run), std::runtime_error);
        EXPECT_EQ(runs(probe, "A"), 1);
        EXPECT_EQ(runs(probe, "ASK_USER"), 0);
    }

    auto       store  = std::make_shared<SqliteCheckpointStore>(file);
    const auto latest = store->load_latest("crash");
    ASSERT_TRUE(latest.has_value());
    ASSERT_EQ(latest->interrupt_phase, CheckpointPhase::Completed);
    ASSERT_EQ(latest->next_nodes, std::vector<std::string>{"ASK_USER"});

    // Process 2: recovery resumes A's checkpoint. ASK_USER still waits for approval.
    auto       engine    = build(definition, store);
    const auto recovered = engine->resume("crash");
    ASSERT_EQ(recovered.status(), RunStatus::Interrupted);
    EXPECT_EQ(recovered.interrupt_node, "ASK_USER");
    EXPECT_EQ(runs(probe, "ASK_USER"), 0) << "the gated node ran without approval";
    EXPECT_EQ(runs(probe, "A"), 1);

    // The approval answers that interrupt; ASK_USER runs once.
    const auto approved = engine->resume("crash", json{{"approved", true}});
    EXPECT_EQ(approved.status(), RunStatus::Completed);
    EXPECT_EQ(approved.execution_trace, std::vector<std::string>{"ASK_USER"});
    EXPECT_EQ(runs(probe, "ASK_USER"), 1);
}

TEST(InterruptResumeGate, AnExactResumeOfACompletedCheckpointStillAsks) {
    const auto probe  = unique_probe("exact");
    auto       store  = std::make_shared<InMemoryCheckpointStore>();
    auto       engine = build(approval_graph(probe), store);
    RunConfig  run;
    run.thread_id = "exact";
    ASSERT_EQ(engine->run(run).interrupt_node, "ASK_USER");

    // A's Completed checkpoint, the one before the interrupt.
    std::optional<Checkpoint> committed;
    for (const auto& checkpoint : store->list("exact", 100))
        if (checkpoint.interrupt_phase == CheckpointPhase::Completed &&
            checkpoint.next_nodes == std::vector<std::string>{"ASK_USER"})
            committed = checkpoint;
    ASSERT_TRUE(committed.has_value());

    const auto resumed = engine->resume_from(run, committed->id);
    ASSERT_EQ(resumed.status(), RunStatus::Interrupted);
    EXPECT_EQ(resumed.interrupt_node, "ASK_USER");
    EXPECT_EQ(runs(probe, "ASK_USER"), 0);
}

TEST(InterruptResumeGate, AResumeAfterInterruptAfterAsksBeforeTheNextGatedNode) {
    const auto probe = unique_probe("after");
    auto       engine =
        build(approval_graph(probe, json{{"interrupt_after", json::array({"A"})}}),
              std::make_shared<InMemoryCheckpointStore>());
    RunConfig run;
    run.thread_id = "after";
    const auto reviewed = engine->run(run);
    ASSERT_EQ(reviewed.status(), RunStatus::Interrupted);
    EXPECT_EQ(reviewed.interrupt_node, "A");

    // Answering the review after A is not an approval of ASK_USER.
    const auto gated = engine->resume("after", json("reviewed"));
    ASSERT_EQ(gated.status(), RunStatus::Interrupted);
    EXPECT_EQ(gated.interrupt_node, "ASK_USER");
    EXPECT_EQ(runs(probe, "ASK_USER"), 0);

    const auto approved = engine->resume("after", json("approved"));
    EXPECT_EQ(approved.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "ASK_USER"), 1);
}

TEST(InterruptResumeGate, AResumeAfterAnUpdateAsANodeAsksBeforeTheNextGatedNode) {
    const auto probe = unique_probe("update");
    auto       engine =
        build(approval_graph(probe, json{{"interrupt_after", json::array({"A"})}}),
              std::make_shared<InMemoryCheckpointStore>());
    RunConfig run;
    run.thread_id = "update";
    ASSERT_EQ(engine->run(run).interrupt_node, "A");

    engine->update_state("update", json{{"note", "edited"}}, "A");
    const auto gated = engine->resume("update");
    ASSERT_EQ(gated.status(), RunStatus::Interrupted);
    EXPECT_EQ(gated.interrupt_node, "ASK_USER");
    EXPECT_EQ(runs(probe, "ASK_USER"), 0);
}

TEST(InterruptResumeGate, AnAnsweredInterruptIsNotAskedAgain) {
    // ASK_USER is gated and also asks from inside. Approving the gate runs it,
    // and answering its own question finishes it: the gate is never asked
    // twice for the same step.
    const auto probe      = unique_probe("answered");
    auto       definition = approval_graph(probe);
    definition["nodes"]["ASK_USER"] = node(probe, json{{"ask", true}});
    auto      engine = build(definition, std::make_shared<InMemoryCheckpointStore>());
    RunConfig run;
    run.thread_id = "answered";
    const auto gated = engine->run(run);
    ASSERT_EQ(gated.status(), RunStatus::Interrupted);
    EXPECT_EQ(gated.interrupt_value["message"], json("Interrupt before node: ASK_USER"));
    EXPECT_EQ(runs(probe, "ASK_USER"), 0);

    // The approved gate runs ASK_USER, which asks its own question.
    const auto asked = engine->resume("answered");
    ASSERT_EQ(asked.status(), RunStatus::Interrupted);
    EXPECT_EQ(asked.interrupt_value["type"], json("NodeInterrupt"));
    EXPECT_EQ(runs(probe, "ASK_USER"), 1);

    // Answering it finishes ASK_USER without asking the gate again.
    const auto done = engine->resume("answered", json("yes"));
    EXPECT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "ASK_USER"), 2);
}
