// #390: GraphEngine::fork of a NodeInterrupt checkpoint carries the
// checkpoint's pending writes.
//
// A NodeInterrupt checkpoint re-enters its super-step on resume, and the
// pending writes recorded for that step (under the step's parent checkpoint,
// or under the interrupt checkpoint itself when no checkpoint precedes the
// step) let resume replay every node and Send that finished instead of running
// it again. fork() copied the checkpoint but not those writes, so the forked
// run ran the finished siblings again, with their effects and provider calls.
//
// The contract: resuming a fork replays exactly what resuming its source
// replays. The source keeps its own writes; both can be resumed.

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

// Asks until it is answered; "again" makes it ask once more.
class AskNode final : public GraphNode {
public:
    AskNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        const auto& answer = in.ctx.resume_value;
        if (!answer || *answer == json("again"))
            throw NodeInterrupt("approve " + name_, json{{"node", name_}});
        NodeOutput out;
        out.writes.push_back(
            ChannelWrite{"results", json::array({name_ + "=" + answer->get<std::string>()})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Finishes at once. Its run count is what a fork must not raise.
class PlainNode final : public GraphNode {
public:
    PlainNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        count_run(probe_of(config_) + "/" + name_);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({name_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Map step: one Send to "worker" per entry of "items".
class MapNode final : public GraphNode {
public:
    MapNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        NodeOutput out;
        const auto items = in.state.get("items");
        if (items.is_array())
            for (std::size_t index = 0; index < items.size(); ++index)
                out.sends.push_back(Send{"worker", json{{"item", items[index]}}});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Send target: the item named by "ask" asks until answered.
class WorkerNode final : public GraphNode {
public:
    WorkerNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto item = in.state.get("item").get<std::string>();
        count_run(probe_of(config_) + "/" + name_ + ":" + item);
        NodeOutput out;
        if (in.state.get("ask") == json(item)) {
            const auto& answer = in.ctx.resume_value;
            if (!answer) throw NodeInterrupt("approve " + item, json{{"item", item}});
            out.writes.push_back(
                ChannelWrite{"results", json::array({item + "=" + answer->get<std::string>()})});
        } else {
            out.writes.push_back(ChannelWrite{"results", json::array({item})});
        }
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
            "fork_writes.ask",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<AskNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "fork_writes.plain",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<PlainNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "fork_writes.map",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<MapNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "fork_writes.worker",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<WorkerNode>(name, config);
            },
            json{{"type", "object"}});
        return true;
    }();
    (void)registered;
}

json channels() {
    return json{{"items", json{{"reducer", "overwrite"}, {"initial", json::array()}}},
                {"ask", json{{"reducer", "overwrite"}, {"initial", ""}}},
                {"item", json{{"reducer", "overwrite"}, {"initial", ""}}},
                {"results", json{{"reducer", "append"}, {"initial", json::array()}}}};
}

json typed(const std::string& probe, const char* type) {
    return json{{"type", type}, {"probe", probe}};
}

// anchor, then risky and calm in one fan-out step. The step has a parent
// checkpoint, under which calm's write is recorded.
json anchored_fan_out(const std::string& probe) {
    return json{{"name", "fork_writes_anchored"},
                {"channels", channels()},
                {"nodes", json{{"anchor", typed(probe, "fork_writes.plain")},
                               {"risky", typed(probe, "fork_writes.ask")},
                               {"calm", typed(probe, "fork_writes.plain")}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "anchor"}},
                                       json{{"from", "anchor"}, {"to", "risky"}},
                                       json{{"from", "anchor"}, {"to", "calm"}},
                                       json{{"from", "risky"}, {"to", "__end__"}},
                                       json{{"from", "calm"}, {"to", "__end__"}}})}};
}

// risky and calm at the entry: no checkpoint precedes the step, so calm's
// write is recorded under the interrupt checkpoint itself.
json entry_fan_out(const std::string& probe) {
    return json{{"name", "fork_writes_entry"},
                {"channels", channels()},
                {"nodes", json{{"risky", typed(probe, "fork_writes.ask")},
                               {"calm", typed(probe, "fork_writes.plain")}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "risky"}},
                                       json{{"from", "__start__"}, {"to", "calm"}},
                                       json{{"from", "risky"}, {"to", "__end__"}},
                                       json{{"from", "calm"}, {"to", "__end__"}}})}};
}

// anchor, then a map node whose Sends go to worker.
json anchored_map(const std::string& probe) {
    return json{{"name", "fork_writes_map"},
                {"channels", channels()},
                {"nodes", json{{"anchor", typed(probe, "fork_writes.plain")},
                               {"map", typed(probe, "fork_writes.map")},
                               {"worker", typed(probe, "fork_writes.worker")}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "anchor"}},
                                       json{{"from", "anchor"}, {"to", "map"}},
                                       json{{"from", "map"}, {"to", "__end__"}},
                                       json{{"from", "worker"}, {"to", "__end__"}}})}};
}

std::unique_ptr<GraphEngine> build(const json& definition, std::shared_ptr<CheckpointStore> store,
                                   int workers = 2) {
    register_types();
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.stderr_notices   = false;
    config.worker_count     = workers;
    return GraphEngine::build(definition, std::move(config));
}

json results_of(const RunResult& result) {
    return result.output["channels"]["results"]["value"];
}

std::multiset<std::string> sorted(const json& values) {
    std::multiset<std::string> out;
    for (std::size_t index = 0; index < values.size(); ++index)
        out.insert(values[index].get<std::string>());
    return out;
}

class TempDir : public neograph::graph::test_support::UniqueTempDir {
public:
    TempDir() : UniqueTempDir("ng-fork-writes-") {}
};

// In memory, and on SQLite where the fork must be durable.
class ForkPendingWrites : public ::testing::TestWithParam<bool> {
protected:
    std::shared_ptr<CheckpointStore> store() {
        if (!GetParam()) return std::make_shared<InMemoryCheckpointStore>();
        return std::make_shared<SqliteCheckpointStore>(dir_.file("checkpoints.sqlite"));
    }

private:
    TempDir dir_;
};

}  // namespace

TEST_P(ForkPendingWrites, AForkedFanOutReplaysTheSiblingThatFinished) {
    const auto probe  = unique_probe("anchored");
    auto       engine = build(anchored_fan_out(probe), store());
    RunConfig  config;
    config.thread_id  = "source";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(runs(probe, "calm"), 1);
    EXPECT_EQ(runs(probe, "risky"), 1);

    const auto forked_id = engine->fork("source", "forked", paused.checkpoint_id);
    ASSERT_FALSE(forked_id.empty());

    const auto forked = engine->resume("forked", json("yes"));
    ASSERT_EQ(forked.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "calm"), 1) << "the finished sibling replays in the fork";
    EXPECT_EQ(runs(probe, "risky"), 2);
    EXPECT_EQ(sorted(results_of(forked)),
              (std::multiset<std::string>{"anchor", "calm", "risky=yes"}));

    // The source keeps its own writes and resumes the same way.
    const auto source = engine->resume("source", json("yes"));
    ASSERT_EQ(source.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "calm"), 1) << "the source still replays its sibling";
    EXPECT_EQ(runs(probe, "risky"), 3);
    EXPECT_EQ(results_of(source), results_of(forked));
}

TEST_P(ForkPendingWrites, AForkOfTheLatestCheckpointCarriesItsWrites) {
    const auto probe  = unique_probe("latest");
    auto       engine = build(anchored_fan_out(probe), store());
    RunConfig  config;
    config.thread_id = "source";
    ASSERT_EQ(engine->run(config).status(), RunStatus::Interrupted);

    (void)engine->fork("source", "forked");
    const auto forked = engine->resume("forked", json("yes"));
    ASSERT_EQ(forked.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "calm"), 1);
    EXPECT_EQ(runs(probe, "anchor"), 1);
}

TEST_P(ForkPendingWrites, AForkedEntryStepReplaysWhatItsInterruptCheckpointRecorded) {
    const auto probe  = unique_probe("entry");
    auto       engine = build(entry_fan_out(probe), store());
    RunConfig  config;
    config.thread_id  = "source";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);

    (void)engine->fork("source", "forked", paused.checkpoint_id);
    const auto forked = engine->resume("forked", json("yes"));
    ASSERT_EQ(forked.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "calm"), 1);
    EXPECT_EQ(sorted(results_of(forked)), (std::multiset<std::string>{"calm", "risky=yes"}));
}

TEST_P(ForkPendingWrites, AForkedSendStepReplaysTheSendsThatFinished) {
    const auto probe  = unique_probe("sends");
    auto       engine = build(anchored_map(probe), store(), 4);
    RunConfig  config;
    config.thread_id  = "source";
    config.input      = json{{"items", json::array({"a", "b", "c"})}, {"ask", "b"}};
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(runs(probe, "worker:a"), 1);
    EXPECT_EQ(runs(probe, "worker:c"), 1);

    (void)engine->fork("source", "forked", paused.checkpoint_id);
    const auto forked = engine->resume("forked", json("ok"));
    ASSERT_EQ(forked.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "map"), 1) << "the node that emitted the Sends replays";
    EXPECT_EQ(runs(probe, "worker:a"), 1) << "a finished Send replays in the fork";
    EXPECT_EQ(runs(probe, "worker:c"), 1) << "a finished Send replays in the fork";
    EXPECT_EQ(runs(probe, "worker:b"), 2);
    EXPECT_EQ(sorted(results_of(forked)),
              (std::multiset<std::string>{"anchor", "a", "b=ok", "c"}));
}

TEST_P(ForkPendingWrites, AForkOfARepeatedInterruptCarriesTheWholeStep) {
    // The second NodeInterrupt checkpoint of a step is a child of the first;
    // the writes a resume replays are recorded under both. The fork carries
    // all of them.
    const auto probe  = unique_probe("again");
    auto       engine = build(anchored_fan_out(probe), store());
    RunConfig  config;
    config.thread_id = "source";
    ASSERT_EQ(engine->run(config).status(), RunStatus::Interrupted);
    const auto again = engine->resume("source", json("again"));
    ASSERT_EQ(again.status(), RunStatus::Interrupted);
    EXPECT_EQ(runs(probe, "calm"), 1);

    (void)engine->fork("source", "forked", again.checkpoint_id);
    const auto forked = engine->resume("forked", json("yes"));
    ASSERT_EQ(forked.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "calm"), 1);
    EXPECT_EQ(runs(probe, "anchor"), 1);
    EXPECT_EQ(runs(probe, "risky"), 3);
    EXPECT_EQ(sorted(results_of(forked)),
              (std::multiset<std::string>{"anchor", "calm", "risky=yes"}));
}

TEST_P(ForkPendingWrites, TheForkKeepsTheWritesAfterTheSourceThreadIsDeleted) {
    const auto probe  = unique_probe("deleted");
    auto       store_ = store();
    auto       engine = build(anchored_fan_out(probe), store_);
    RunConfig  config;
    config.thread_id  = "source";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);

    (void)engine->fork("source", "forked", paused.checkpoint_id);
    store_->delete_thread("source");
    const auto forked = engine->resume("forked", json("yes"));
    ASSERT_EQ(forked.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "calm"), 1) << "the fork owns its copy of the writes";
}

INSTANTIATE_TEST_SUITE_P(Stores, ForkPendingWrites, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Sqlite" : "InMemory"; });
