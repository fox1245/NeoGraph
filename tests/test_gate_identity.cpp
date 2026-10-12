// #396: each interrupt_before gate of a super-step is answered on its own.
//
// When two nodes gated by interrupt_before were ready in the same super-step,
// the run paused before the first, and one resume ran both: the second gate
// was never asked, so its human approval was bypassed.
//
// The contract, the same per-interrupt identity as #388 and the same delivery
// rule as #393: a resume of a gate's Before checkpoint answers that gate, the
// one whose node the checkpoint held, identified by the node's task id in the
// step ("s3:deploy"). Before the step runs, every other gated node of the step
// whose gate has not been answered pauses the run again, with its own Before
// interrupt; the gates the step already had answered are carried with that
// checkpoint. Once every gate of the step is answered, the step runs, and each
// gated node receives the value of the resume that answered its own gate.

#include "fixtures/checkpoint_backends.h"
#include "fixtures/unique_temp_dir.h"

#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
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

int runs(const std::string& probe, const std::string& name) {
    std::lock_guard lock(runs_mutex());
    const auto      found = runs_map().find(probe + "/" + name);
    return found == runs_map().end() ? 0 : found->second;
}

std::string unique_probe(const char* name) {
    static std::atomic<int> next{0};
    return std::string(name) + "-" + std::to_string(++next);
}

std::string text_of(const json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

// Records that it ran, and the resume value it saw ("-" for none).
class RecordNode final : public GraphNode {
public:
    RecordNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        {
            std::lock_guard lock(runs_mutex());
            ++runs_map()[config_.value("probe", std::string()) + "/" + name_];
        }
        const auto seen = in.ctx.resume_value ? text_of(*in.ctx.resume_value) : std::string("-");
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({name_ + "=" + seen})});
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
            "gate_identity.record",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<RecordNode>(name, config);
            },
            json{{"type", "object"}});
        return true;
    }();
    (void)registered;
}

json edge(const std::string& from, const std::string& to) { return json{{"from", from}, {"to", to}}; }

// anchor, then `fan` nodes ready together, then "after"; `gated` lists the
// nodes with interrupt_before.
json definition(const std::string& probe, const std::vector<std::string>& fan,
                const std::vector<std::string>& gated) {
    auto nodes = json::object();
    auto edges = json::array();
    const auto node = [&](const std::string& name) {
        nodes[name] = json{{"type", "gate_identity.record"}, {"probe", probe}};
    };
    node("anchor");
    node("after");
    edges.push_back(edge("__start__", "anchor"));
    for (const auto& name : fan) {
        node(name);
        edges.push_back(edge("anchor", name));
        edges.push_back(edge(name, "after"));
    }
    edges.push_back(edge("after", "__end__"));
    auto gates = json::array();
    for (const auto& name : gated) gates.push_back(name);
    return json{{"name", "gate_identity"},
                {"channels", json{{"results", json{{"reducer", "append"}, {"initial", json::array()}}}}},
                {"nodes", nodes},
                {"edges", edges},
                {"interrupt_before", gates}};
}

std::unique_ptr<GraphEngine> build(const json& graph, std::shared_ptr<CheckpointStore> store) {
    register_types();
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.stderr_notices   = false;
    config.worker_count     = 2;
    return GraphEngine::build(graph, std::move(config));
}

std::multiset<std::string> results_of(const RunResult& result) {
    std::multiset<std::string> out;
    const auto                 values = result.output["channels"]["results"]["value"];
    for (std::size_t index = 0; index < values.size(); ++index)
        out.insert(values[index].get<std::string>());
    return out;
}

using test_support::Backend;

class GateIdentity : public ::testing::TestWithParam<Backend> {
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
        if (!store_) store_ = make_store();
        return store_;
    }
    // A second store over the same file, as a restarted process opens it.
    std::shared_ptr<CheckpointStore> reopen() {
        if (GetParam() == Backend::InMemory) return store();
        store_ = make_store();
        return store_;
    }

private:
    std::shared_ptr<CheckpointStore> make_store() {
        return test_support::open_backend(GetParam(), dir_.file("gates.sqlite"));
    }
    test_support::UniqueTempDir      dir_{"ng-gate-identity-"};
    std::shared_ptr<CheckpointStore> store_;
};

}  // namespace

TEST_P(GateIdentity, TwoGatesOfOneStepAreAnsweredOneAtATime) {
    const auto probe  = unique_probe("two");
    auto       engine = build(definition(probe, {"a", "b"}, {"a", "b"}), store());
    RunConfig  config;
    config.thread_id  = "two";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "a");

    // The resume answers a's gate. b's gate has not been answered: the run
    // pauses before b with its own interrupt, and neither node runs.
    const auto second = engine->resume("two", json("A"));
    ASSERT_EQ(second.status(), RunStatus::Interrupted) << "one resume passed both gates";
    EXPECT_EQ(second.interrupt_node, "b");
    EXPECT_EQ(second.interrupt_value["message"], json("Interrupt before node: b"));
    EXPECT_NE(second.checkpoint_id, paused.checkpoint_id);
    EXPECT_EQ(runs(probe, "a"), 0);
    EXPECT_EQ(runs(probe, "b"), 0);

    const auto done = engine->resume("two", json("B"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"anchor=-", "a=A", "b=B", "after=-"}))
        << "each gated node receives the answer to its own gate";
    EXPECT_EQ(runs(probe, "a"), 1);
    EXPECT_EQ(runs(probe, "b"), 1);
}

TEST_P(GateIdentity, AnUngatedSiblingWaitsForEveryGateOfItsStep) {
    const auto probe  = unique_probe("mixed");
    auto       engine = build(definition(probe, {"a", "free", "c"}, {"a", "c"}), store());
    RunConfig  config;
    config.thread_id = "mixed";
    ASSERT_EQ(engine->run(config).interrupt_node, "a");
    const auto second = engine->resume("mixed", json("A"));
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    EXPECT_EQ(second.interrupt_node, "c");
    EXPECT_EQ(runs(probe, "free"), 0) << "the step does not run before every gate is answered";
    const auto done = engine->resume("mixed", json("C"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done),
              (std::multiset<std::string>{"anchor=-", "a=A", "free=-", "c=C", "after=-"}));
}

TEST_P(GateIdentity, ThreeGatesAreAnsweredAcrossARestart) {
    const auto  probe = unique_probe("three");
    const auto  graph = definition(probe, {"a", "b", "c"}, {"a", "b", "c"});
    RunConfig   config;
    config.thread_id = "three";
    {
        auto engine = build(graph, store());
        ASSERT_EQ(engine->run(config).interrupt_node, "a");
        ASSERT_EQ(engine->resume("three", json("A")).interrupt_node, "b");
    }
    // A fresh engine on the same store keeps the gates already answered.
    auto       engine = build(graph, reopen());
    const auto third  = engine->resume("three", json("B"));
    ASSERT_EQ(third.status(), RunStatus::Interrupted);
    EXPECT_EQ(third.interrupt_node, "c");
    const auto done = engine->resume("three", json("C"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done),
              (std::multiset<std::string>{"anchor=-", "a=A", "b=B", "c=C", "after=-"}));
}

TEST_P(GateIdentity, ASingleGateKeepsItsCheckpointBytes) {
    const auto probe  = unique_probe("single");
    auto       engine = build(definition(probe, {"a", "free"}, {"a"}), store());
    RunConfig  config;
    config.thread_id  = "single";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.interrupt_node, "a");
    const auto saved = store()->load_by_id(paused.checkpoint_id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->metadata.dump().find("answered_gates"), std::string::npos);
    const auto done = engine->resume("single", json("A"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"anchor=-", "a=A", "free=-", "after=-"}));
}

TEST_P(GateIdentity, AGateOfALaterStepIsAskedAgain) {
    // b's gate is in the step after a's; one resume answers a only.
    const auto probe = unique_probe("later");
    auto graph = definition(probe, {"a"}, {"a", "after"});
    auto engine = build(graph, store());
    RunConfig config;
    config.thread_id = "later";
    ASSERT_EQ(engine->run(config).interrupt_node, "a");
    const auto second = engine->resume("later", json("A"));
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    EXPECT_EQ(second.interrupt_node, "after");
    const auto done = engine->resume("later", json("Z"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"anchor=-", "a=A", "after=Z"}));
}

INSTANTIATE_TEST_SUITE_P(Stores, GateIdentity, ::testing::ValuesIn(test_support::backends()),
                         [](const auto& info) { return test_support::backend_name(info.param); });
