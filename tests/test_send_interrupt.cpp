// #385: a NodeInterrupt raised by the target of a Send pauses the run like any
// other node.
//
// Send targets run after the step's ready nodes, outside the catch that turns
// a NodeInterrupt into a pause, and neither Send path saved a checkpoint: the
// interrupt escaped the run as an exception and there was nothing to resume.
//
// The contract is the one of a ready node that interrupts in a fan-out step:
// the run stops Interrupted with a NodeInterrupt checkpoint that re-enters the
// whole step (the state from the start of the step, the step's ready nodes as
// next nodes). Resume runs the step again: every node and every Send that
// finished replays its recorded result, without running again, and the Send
// that asked runs with the answer. With no checkpoint before the step (a map
// node at the entry), those results are recorded under the interrupt
// checkpoint itself.

#include "fixtures/unique_temp_dir.h"

#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
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

// Map step: one Send to "worker" per entry of the "items" channel.
class MapNode final : public GraphNode {
public:
    MapNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(config_.value("probe", std::string()) + "/" + name_);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"trace", json::array({name_})});
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

// Send target. The item named by the "ask" channel interrupts until it is
// answered; the answer "again" makes it ask once more. Every other item, and
// an answered one, appends a result.
class WorkerNode final : public GraphNode {
public:
    WorkerNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto item = in.state.get("item").get<std::string>();
        count_run(config_.value("probe", std::string()) + "/" + name_ + ":" + item);
        NodeOutput out;
        if (in.state.get("ask") == json(item)) {
            const auto& answer = in.ctx.resume_value;
            if (!answer || *answer == json("again"))
                throw NodeInterrupt("approve " + item, json{{"item", item}});
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

// A ready node with no Sends of its own.
class PlainNode final : public GraphNode {
public:
    PlainNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        count_run(config_.value("probe", std::string()) + "/" + name_);
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
        auto& factory = NodeFactory::instance();
        factory.register_type(
            "send_interrupt.map",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<MapNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "send_interrupt.worker",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<WorkerNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "send_interrupt.plain",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<PlainNode>(name, config);
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
                {"results", json{{"reducer", "append"}, {"initial", json::array()}}},
                {"trace", json{{"reducer", "append"}, {"initial", json::array()}}}};
}

json typed(const std::string& probe, const char* type) {
    return json{{"type", type}, {"probe", probe}};
}

// The map node is the entry: its step has no checkpoint before it.
json entry_map_graph(const std::string& probe) {
    return json{{"name", "send_interrupt_entry"},
                {"channels", channels()},
                {"nodes", json{{"map", typed(probe, "send_interrupt.map")},
                               {"worker", typed(probe, "send_interrupt.worker")}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "map"}},
                                       json{{"from", "map"}, {"to", "__end__"}},
                                       json{{"from", "worker"}, {"to", "__end__"}}})}};
}

// anchor, then map and side in one fan-out step: the step has a checkpoint
// before it, and a ready sibling that finishes.
json anchored_map_graph(const std::string& probe) {
    return json{{"name", "send_interrupt_anchored"},
                {"channels", channels()},
                {"nodes", json{{"anchor", typed(probe, "send_interrupt.plain")},
                               {"map", typed(probe, "send_interrupt.map")},
                               {"side", typed(probe, "send_interrupt.plain")},
                               {"worker", typed(probe, "send_interrupt.worker")}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "anchor"}},
                                       json{{"from", "anchor"}, {"to", "map"}},
                                       json{{"from", "anchor"}, {"to", "side"}},
                                       json{{"from", "map"}, {"to", "__end__"}},
                                       json{{"from", "side"}, {"to", "__end__"}},
                                       json{{"from", "worker"}, {"to", "__end__"}}})}};
}

std::unique_ptr<GraphEngine> build(const json& definition, std::shared_ptr<CheckpointStore> store,
                                   int workers = 1) {
    register_types();
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.stderr_notices   = false;
    config.worker_count     = workers;
    return GraphEngine::build(definition, std::move(config));
}

RunConfig run_config(const std::string& thread, json items, const std::string& ask) {
    RunConfig config;
    config.thread_id = thread;
    config.input     = json{{"items", std::move(items)}, {"ask", ask}};
    return config;
}

json channel(const RunResult& result, const char* name) {
    return result.output["channels"][name]["value"];
}

template <typename T>
T drive(asio::awaitable<T> operation) {
    asio::io_context io;
    auto             future = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    return future.get();
}

class TempDir : public neograph::graph::test_support::UniqueTempDir {
public:
    TempDir() : UniqueTempDir("ng-send-interrupt-") {}
};

}  // namespace

TEST(SendInterrupt, ASingleSendThatInterruptsPausesTheRun) {
    const auto probe  = unique_probe("single");
    auto       store  = std::make_shared<InMemoryCheckpointStore>();
    auto       engine = build(entry_map_graph(probe), store);

    const auto paused = engine->run(run_config("single", json::array({"a"}), "a"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "worker");
    EXPECT_EQ(paused.interrupt_value["type"], json("NodeInterrupt"));
    EXPECT_EQ(paused.interrupt_value["reason"], json("approve a"));
    EXPECT_EQ(paused.interrupt_value["value"], json({{"item", "a"}}));
    EXPECT_TRUE(paused.execution_trace.empty()) << "the paused step has not completed";
    ASSERT_FALSE(paused.checkpoint_id.empty());

    // The checkpoint re-enters the whole step, from the state it started with.
    const auto saved = store->load_by_id(paused.checkpoint_id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->interrupt_phase, CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(saved->current_node, "worker");
    EXPECT_EQ(saved->next_nodes, std::vector<std::string>{"map"});
    EXPECT_EQ(saved->step, 0);
    EXPECT_EQ(saved->channel_values["channels"]["trace"]["value"], json::array());
    EXPECT_EQ(saved->channel_values["channels"]["item"]["value"], json(""));

    const auto done = engine->resume("single", json("yes"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(channel(done, "results"), json::array({"a=yes"}));
    EXPECT_EQ(channel(done, "trace"), json::array({"map"}));
    EXPECT_EQ(runs(probe, "map"), 1) << "the map node replays; it does not run again";
    EXPECT_EQ(runs(probe, "worker:a"), 2);
}

TEST(SendInterrupt, ParallelSendsReplayTheSiblingsThatFinished) {
    for (const int workers : {1, 4}) {
        SCOPED_TRACE("workers=" + std::to_string(workers));
        const auto probe  = unique_probe("parallel");
        auto       store  = std::make_shared<InMemoryCheckpointStore>();
        auto       engine = build(entry_map_graph(probe), store, workers);

        const auto paused =
            engine->run(run_config("parallel", json::array({"a", "b", "c"}), "b"));
        ASSERT_EQ(paused.status(), RunStatus::Interrupted);
        EXPECT_EQ(paused.interrupt_node, "worker");
        EXPECT_EQ(paused.interrupt_value["value"], json({{"item", "b"}}));
        const auto saved = store->load_by_id(paused.checkpoint_id);
        ASSERT_TRUE(saved.has_value());
        EXPECT_EQ(saved->interrupt_phase, CheckpointPhase::NodeInterrupt);
        EXPECT_EQ(saved->next_nodes, std::vector<std::string>{"map"});
        EXPECT_EQ(saved->channel_values["channels"]["results"]["value"], json::array());
        EXPECT_EQ(runs(probe, "worker:a"), 1);
        EXPECT_EQ(runs(probe, "worker:c"), 1);

        // A fresh engine over the same store resumes the exact checkpoint.
        auto       cold = build(entry_map_graph(probe), store, workers);
        RunConfig  config;
        config.thread_id = "parallel";
        const auto done  = cold->resume_from(config, paused.checkpoint_id, json("yes"));
        ASSERT_EQ(done.status(), RunStatus::Completed);
        EXPECT_EQ(channel(done, "results"), json::array({"a", "b=yes", "c"}));
        EXPECT_EQ(runs(probe, "map"), 1);
        EXPECT_EQ(runs(probe, "worker:a"), 1) << "a finished sibling is replayed, not re-run";
        EXPECT_EQ(runs(probe, "worker:c"), 1) << "a finished sibling is replayed, not re-run";
        EXPECT_EQ(runs(probe, "worker:b"), 2);
    }
}

TEST(SendInterrupt, AFinishedReadySiblingAndFinishedSendsReplayAfterAParentCheckpoint) {
    const auto probe  = unique_probe("anchored");
    auto       store  = std::make_shared<InMemoryCheckpointStore>();
    auto       engine = build(anchored_map_graph(probe), store, 2);

    const auto paused = engine->run(run_config("anchored", json::array({"a", "b"}), "a"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    const auto saved = store->load_by_id(paused.checkpoint_id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->interrupt_phase, CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(std::set<std::string>(saved->next_nodes.begin(), saved->next_nodes.end()),
              (std::set<std::string>{"map", "side"}));
    EXPECT_EQ(saved->channel_values["channels"]["trace"]["value"], json::array({"anchor"}))
        << "the state from the start of the step";
    EXPECT_EQ(paused.execution_trace, std::vector<std::string>{"anchor"});

    const auto done = engine->resume("anchored", json("ok"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(channel(done, "results"), json::array({"a=ok", "b"}));
    EXPECT_EQ(runs(probe, "anchor"), 1);
    EXPECT_EQ(runs(probe, "map"), 1);
    EXPECT_EQ(runs(probe, "side"), 1) << "the finished ready sibling is replayed";
    EXPECT_EQ(runs(probe, "worker:b"), 1) << "the finished Send is replayed";
    EXPECT_EQ(runs(probe, "worker:a"), 2);
}

TEST(SendInterrupt, AnAnsweredSendCanAskAgainAndTheFinishedSiblingsStillReplay) {
    const auto probe  = unique_probe("again");
    auto       store  = std::make_shared<InMemoryCheckpointStore>();
    auto       engine = build(entry_map_graph(probe), store, 4);

    const auto first = engine->run(run_config("again", json::array({"a", "b", "c"}), "c"));
    ASSERT_EQ(first.status(), RunStatus::Interrupted);
    const auto second = engine->resume("again", json("again"));
    ASSERT_EQ(second.status(), RunStatus::Interrupted);
    EXPECT_EQ(second.interrupt_node, "worker");
    ASSERT_NE(second.checkpoint_id, first.checkpoint_id);
    const auto saved = store->load_by_id(second.checkpoint_id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->parent_id, first.checkpoint_id);
    EXPECT_EQ(saved->step, 0);

    const auto done = engine->resume("again", json("yes"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(channel(done, "results"), json::array({"a", "b", "c=yes"}));
    EXPECT_EQ(runs(probe, "map"), 1);
    EXPECT_EQ(runs(probe, "worker:a"), 1);
    EXPECT_EQ(runs(probe, "worker:b"), 1);
    EXPECT_EQ(runs(probe, "worker:c"), 3);
}

TEST(SendInterrupt, EveryEntryPointPausesAndResumesTheSameWay) {
    using Pause  = std::function<RunResult(GraphEngine&, RunConfig)>;
    using Resume = std::function<RunResult(GraphEngine&, const RunConfig&, const RunResult&)>;
    const GraphStreamCallback ignore = [](const GraphEvent&) {};
    const std::vector<std::pair<std::string, Pause>> pauses{
        {"run", [](GraphEngine& engine, RunConfig config) { return engine.run(config); }},
        {"run_stream",
         [&](GraphEngine& engine, RunConfig config) { return engine.run_stream(config, ignore); }},
        {"run_async",
         [](GraphEngine& engine, RunConfig config) {
             return drive(engine.run_async(std::move(config)));
         }},
        {"run_stream_async", [&](GraphEngine& engine, RunConfig config) {
             return drive(engine.run_stream_async(std::move(config), ignore));
         }}};
    const std::vector<std::pair<std::string, Resume>> resumes{
        {"resume",
         [](GraphEngine& engine, const RunConfig& config, const RunResult&) {
             return engine.resume(config.thread_id, json("yes"));
         }},
        {"resume_async",
         [](GraphEngine& engine, const RunConfig& config, const RunResult&) {
             return drive(engine.resume_async(config.thread_id, json("yes")));
         }},
        {"resume_from",
         [](GraphEngine& engine, const RunConfig& config, const RunResult& paused) {
             return engine.resume_from(config, paused.checkpoint_id, json("yes"));
         }},
        {"resume_from_async", [](GraphEngine& engine, const RunConfig& config,
                                 const RunResult& paused) {
             return drive(engine.resume_from_async(config, paused.checkpoint_id, json("yes")));
         }}};
    for (std::size_t index = 0; index < pauses.size(); ++index) {
        const auto& [pause_name, pause] = pauses[index];
        const auto& [resume_name, resume] = resumes[index];
        SCOPED_TRACE(pause_name + " then " + resume_name);
        const auto probe  = unique_probe("entry");
        auto       store  = std::make_shared<InMemoryCheckpointStore>();
        auto       engine = build(entry_map_graph(probe), store, 2);
        const auto config = run_config("entry-" + pause_name, json::array({"a", "b"}), "b");

        const auto paused = pause(*engine, config);
        ASSERT_EQ(paused.status(), RunStatus::Interrupted);
        EXPECT_EQ(paused.interrupt_node, "worker");
        ASSERT_FALSE(paused.checkpoint_id.empty());

        const auto done = resume(*engine, config, paused);
        ASSERT_EQ(done.status(), RunStatus::Completed);
        EXPECT_EQ(channel(done, "results"), json::array({"a", "b=yes"}));
        EXPECT_EQ(runs(probe, "map"), 1);
        EXPECT_EQ(runs(probe, "worker:a"), 1);
        EXPECT_EQ(runs(probe, "worker:b"), 2);
    }
}

TEST(SendInterrupt, WithoutACheckpointStoreTheRunStillPauses) {
    const auto probe  = unique_probe("storeless");
    auto       engine = build(entry_map_graph(probe), nullptr, 2);
    const auto paused = engine->run(run_config("storeless", json::array({"a", "b"}), "a"));
    EXPECT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "worker");
    EXPECT_TRUE(paused.checkpoint_id.empty());
}

TEST(SendInterrupt, SqliteCrashAndResumeReplaysTheFinishedSends) {
    const TempDir dir;
    const auto    probe = unique_probe("sqlite");
    const auto    file  = dir.file("checkpoints.sqlite");
    std::string   checkpoint_id;
    {  // Process 1 pauses, then is gone.
        auto engine = build(anchored_map_graph(probe), std::make_shared<SqliteCheckpointStore>(file), 4);
        const auto paused =
            engine->run(run_config("sqlite", json::array({"a", "b", "c"}), "b"));
        ASSERT_EQ(paused.status(), RunStatus::Interrupted);
        checkpoint_id = paused.checkpoint_id;
        EXPECT_EQ(runs(probe, "worker:a"), 1);
        EXPECT_EQ(runs(probe, "worker:c"), 1);
    }

    // Process 2 resumes the exact checkpoint from the file.
    auto      engine = build(anchored_map_graph(probe), std::make_shared<SqliteCheckpointStore>(file), 4);
    RunConfig config;
    config.thread_id = "sqlite";
    const auto done  = engine->resume_from(config, checkpoint_id, json("yes"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(channel(done, "results"), json::array({"a", "b=yes", "c"}));
    EXPECT_EQ(runs(probe, "anchor"), 1);
    EXPECT_EQ(runs(probe, "map"), 1);
    EXPECT_EQ(runs(probe, "side"), 1);
    EXPECT_EQ(runs(probe, "worker:a"), 1);
    EXPECT_EQ(runs(probe, "worker:c"), 1);
    EXPECT_EQ(runs(probe, "worker:b"), 2);
}

TEST(SendInterrupt, ASqliteEntryStepRecordsItsFinishedWorkUnderTheInterrupt) {
    // No checkpoint precedes the entry step, so the finished map node and Sends
    // are recorded under the interrupt checkpoint itself.
    const TempDir dir;
    const auto    probe = unique_probe("sqlite-entry");
    const auto    file  = dir.file("checkpoints.sqlite");
    std::string   checkpoint_id;
    {
        auto engine = build(entry_map_graph(probe), std::make_shared<SqliteCheckpointStore>(file), 2);
        const auto paused = engine->run(run_config("entry", json::array({"a", "b"}), "a"));
        ASSERT_EQ(paused.status(), RunStatus::Interrupted);
        checkpoint_id = paused.checkpoint_id;
    }
    auto store = std::make_shared<SqliteCheckpointStore>(file);
    const auto saved = store->load_by_id(checkpoint_id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_TRUE(saved->parent_id.empty());
    std::set<std::string> recorded;
    for (const auto& write : store->get_writes("entry", checkpoint_id)) recorded.insert(write.node_name);
    EXPECT_EQ(recorded, (std::set<std::string>{"map", "worker"}));

    auto      engine = build(entry_map_graph(probe), store, 2);
    RunConfig config;
    config.thread_id = "entry";
    const auto done  = engine->resume_from(config, checkpoint_id, json("yes"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(channel(done, "results"), json::array({"a=yes", "b"}));
    EXPECT_EQ(runs(probe, "map"), 1);
    EXPECT_EQ(runs(probe, "worker:b"), 1);
    EXPECT_EQ(runs(probe, "worker:a"), 2);
}

TEST(SendInterrupt, AReadyNodeThatInterruptsAtTheEntryReplaysItsFinishedSibling) {
    // The ready-node path keeps the same contract with no checkpoint before
    // the step: the sibling that finished is recorded under the interrupt.
    const auto probe = unique_probe("entry-ready");
    json       definition{
        {"name", "send_interrupt_ready_entry"},
        {"channels", channels()},
        {"nodes", json{{"calm", typed(probe, "send_interrupt.plain")},
                       {"worker", typed(probe, "send_interrupt.worker")}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "calm"}},
                               json{{"from", "__start__"}, {"to", "worker"}},
                               json{{"from", "calm"}, {"to", "__end__"}},
                               json{{"from", "worker"}, {"to", "__end__"}}})}};
    auto      store  = std::make_shared<InMemoryCheckpointStore>();
    auto      engine = build(definition, store, 2);
    RunConfig config;
    config.thread_id = "entry-ready";
    config.input     = json{{"item", "x"}, {"ask", "x"}};
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "worker");

    const auto done = engine->resume("entry-ready", json("yes"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(runs(probe, "calm"), 1) << "the finished sibling is replayed";
    EXPECT_EQ(runs(probe, "worker:x"), 2);
}
