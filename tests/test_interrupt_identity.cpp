// #388: several NodeInterrupts in one super-step are answered one at a time.
//
// When two ready nodes, or two Sends, of one super-step raised NodeInterrupt,
// the run paused on the first and one resume(value) handed the same value to
// both: the second interrupt was answered with the first one's answer, and
// its own question was never asked. The JavaScript joins (#380) and the
// plan-level parallel and quorum (#384, #387) answer one paused member per
// resume, and the others ask again.
//
// The contract, for Core static nodes, fan-out branches and Sends: each
// interrupt has an identity, the paused invocation's task id. The paused
// result lists every interrupt of the step (RunResult::interrupts) and
// reports the first. A resume answers the interrupt that
// RunMetadata::resume_interrupt_id names, or the first pending one; every
// other invocation that paused runs again without the value and asks again
// with its own payload. A checkpoint that paused on several lists them in its
// metadata; one that paused on one keeps its bytes.

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

std::string probe_of(const json& config) { return config.value("probe", std::string()); }

// Asks its own question until it is answered, and records the answer it got.
class AskNode final : public GraphNode {
public:
    AskNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        const auto& answer = in.ctx.resume_value;
        if (!answer) throw NodeInterrupt("approve " + name_, json{{"asks", name_}});
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

// One Send to "worker" per entry of "items".
class MapNode final : public GraphNode {
public:
    MapNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        NodeOutput out;
        const auto items = in.state.get("items");
        for (std::size_t index = 0; index < items.size(); ++index)
            out.sends.push_back(Send{"worker", json{{"item", items[index]}}});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Send target: items listed in "ask" ask until answered.
class WorkerNode final : public GraphNode {
public:
    WorkerNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto item = in.state.get("item").get<std::string>();
        count_run(probe_of(config_) + "/" + name_ + ":" + item);
        NodeOutput out;
        const auto asking = in.state.get("ask");
        bool       asks   = false;
        for (std::size_t index = 0; index < asking.size(); ++index)
            if (asking[index] == json(item)) asks = true;
        if (asks) {
            const auto& answer = in.ctx.resume_value;
            if (!answer) throw NodeInterrupt("approve " + item, json{{"asks", item}});
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
            "interrupt_identity.ask",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<AskNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "interrupt_identity.plain",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<PlainNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "interrupt_identity.map",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<MapNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "interrupt_identity.worker",
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
                {"ask", json{{"reducer", "overwrite"}, {"initial", json::array()}}},
                {"item", json{{"reducer", "overwrite"}, {"initial", ""}}},
                {"results", json{{"reducer", "append"}, {"initial", json::array()}}}};
}

json typed(const std::string& probe, const char* type) {
    return json{{"type", type}, {"probe", probe}};
}

// anchor, then left, right and calm in one fan-out step: left and right ask.
json two_askers(const std::string& probe) {
    return json{{"name", "interrupt_identity_fan_out"},
                {"channels", channels()},
                {"nodes", json{{"anchor", typed(probe, "interrupt_identity.plain")},
                               {"left", typed(probe, "interrupt_identity.ask")},
                               {"right", typed(probe, "interrupt_identity.ask")},
                               {"calm", typed(probe, "interrupt_identity.plain")}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "anchor"}},
                                       json{{"from", "anchor"}, {"to", "left"}},
                                       json{{"from", "anchor"}, {"to", "right"}},
                                       json{{"from", "anchor"}, {"to", "calm"}},
                                       json{{"from", "left"}, {"to", "__end__"}},
                                       json{{"from", "right"}, {"to", "__end__"}},
                                       json{{"from", "calm"}, {"to", "__end__"}}})}};
}

// The same at the entry: no checkpoint precedes the step.
json two_askers_at_entry(const std::string& probe) {
    return json{{"name", "interrupt_identity_entry"},
                {"channels", channels()},
                {"nodes", json{{"left", typed(probe, "interrupt_identity.ask")},
                               {"right", typed(probe, "interrupt_identity.ask")}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "left"}},
                                       json{{"from", "__start__"}, {"to", "right"}},
                                       json{{"from", "left"}, {"to", "__end__"}},
                                       json{{"from", "right"}, {"to", "__end__"}}})}};
}

// A single asking node.
json one_asker(const std::string& probe) {
    return json{{"name", "interrupt_identity_single"},
                {"channels", channels()},
                {"nodes", json{{"left", typed(probe, "interrupt_identity.ask")}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "left"}},
                                       json{{"from", "left"}, {"to", "__end__"}}})}};
}

// anchor, then a map node whose Sends go to worker.
json sends(const std::string& probe) {
    return json{{"name", "interrupt_identity_sends"},
                {"channels", channels()},
                {"nodes", json{{"anchor", typed(probe, "interrupt_identity.plain")},
                               {"map", typed(probe, "interrupt_identity.map")},
                               {"worker", typed(probe, "interrupt_identity.worker")}}},
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

std::multiset<std::string> results_of(const RunResult& result) {
    std::multiset<std::string> out;
    const auto                 values = result.output["channels"]["results"]["value"];
    for (std::size_t index = 0; index < values.size(); ++index)
        out.insert(values[index].get<std::string>());
    return out;
}

std::vector<std::string> ids_of(const RunResult& result) {
    std::vector<std::string> ids;
    for (const auto& pending : result.interrupts) ids.push_back(pending.id);
    return ids;
}

RunResult resume(GraphEngine& engine, const std::string& thread, const std::string& value,
                 const std::string& interrupt_id = {}) {
    RunConfig config;
    config.thread_id = thread;
    RunMetadata metadata;
    metadata.resume_interrupt_id = interrupt_id;
    return engine.resume(config, json(value), nullptr, metadata);
}

json recorded_interrupts(CheckpointStore& store, const std::string& checkpoint_id) {
    const auto saved = store.load_by_id(checkpoint_id);
    if (!saved || !saved->metadata.is_object() || !saved->metadata.contains("_neograph"))
        return json();
    const auto reserved = saved->metadata["_neograph"];
    return reserved.is_object() && reserved.contains("interrupts") ? reserved["interrupts"] : json();
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> next{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ng-interrupt-identity-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++next));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string file(const char* name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

class InterruptIdentity : public ::testing::TestWithParam<bool> {
protected:
    std::shared_ptr<CheckpointStore> store() {
        if (!store_)
            store_ = GetParam() ? std::shared_ptr<CheckpointStore>(std::make_shared<SqliteCheckpointStore>(
                                      dir_.file("checkpoints.sqlite")))
                                : std::make_shared<InMemoryCheckpointStore>();
        return store_;
    }

private:
    TempDir                          dir_;
    std::shared_ptr<CheckpointStore> store_;
};

}  // namespace

TEST_P(InterruptIdentity, TwoNodesThatAskAreAnsweredOneAtATime) {
    const auto probe  = unique_probe("fan-out");
    auto       engine = build(two_askers(probe), store());
    RunConfig  config;
    config.thread_id  = "two";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "left");
    EXPECT_EQ(ids_of(paused), (std::vector<std::string>{"s1:left", "s1:right"}));
    EXPECT_EQ(paused.interrupt_id, "s1:left") << "the reported interrupt is the first";
    ASSERT_EQ(paused.interrupts.size(), 2U);
    EXPECT_EQ(paused.interrupts[1].node, "right");
    EXPECT_EQ(paused.interrupts[1].value["value"], json({{"asks", "right"}}));
    EXPECT_EQ(paused.interrupts[0].value, paused.interrupt_value);
    const auto listed = recorded_interrupts(*store(), paused.checkpoint_id);
    ASSERT_TRUE(listed.is_array()) << "a checkpoint of several pending interrupts lists them";
    ASSERT_EQ(listed.size(), 2U);
    EXPECT_EQ(listed[1]["id"], json("s1:right"));

    // The resume answers left only. right runs without the value and asks its
    // own question again.
    const auto again = engine->resume("two", json("yes"));
    ASSERT_EQ(again.status(), RunStatus::Interrupted);
    EXPECT_EQ(again.interrupt_node, "right");
    EXPECT_EQ(again.interrupt_id, "s1:right");
    EXPECT_EQ(again.interrupt_value["value"], json({{"asks", "right"}}));
    EXPECT_EQ(ids_of(again), (std::vector<std::string>{"s1:right"}));
    EXPECT_FALSE(recorded_interrupts(*store(), again.checkpoint_id).is_array())
        << "a checkpoint of one interrupt keeps its bytes";

    const auto done = engine->resume("two", json("no"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_TRUE(done.interrupts.empty());
    EXPECT_EQ(results_of(done),
              (std::multiset<std::string>{"anchor", "calm", "left=yes", "right=no"}));
    EXPECT_EQ(runs(probe, "anchor"), 1);
    EXPECT_EQ(runs(probe, "calm"), 1) << "the finished sibling replays";
    EXPECT_EQ(runs(probe, "left"), 2);
    EXPECT_EQ(runs(probe, "right"), 3) << "right asked twice, then got its own answer";
}

TEST_P(InterruptIdentity, AResumeAnswersTheInterruptItNames) {
    const auto probe  = unique_probe("named");
    auto       engine = build(two_askers(probe), store());
    RunConfig  config;
    config.thread_id = "named";
    ASSERT_EQ(engine->run(config).status(), RunStatus::Interrupted);

    const auto again = resume(*engine, "named", "R", "s1:right");
    ASSERT_EQ(again.status(), RunStatus::Interrupted);
    EXPECT_EQ(again.interrupt_id, "s1:left") << "left asks again";

    const auto done = resume(*engine, "named", "L", "s1:left");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"anchor", "calm", "left=L", "right=R"}));
}

TEST_P(InterruptIdentity, AnInterruptThatIsNotPendingIsRejectedBeforeAnythingRuns) {
    const auto probe  = unique_probe("unknown");
    auto       engine = build(two_askers(probe), store());
    RunConfig  config;
    config.thread_id = "unknown";
    ASSERT_EQ(engine->run(config).status(), RunStatus::Interrupted);
    EXPECT_THROW((void)resume(*engine, "unknown", "x", "s1:calm"), std::invalid_argument)
        << "calm finished; it is not pending";
    EXPECT_THROW((void)resume(*engine, "unknown", "x", "s0:anchor"), std::invalid_argument);
    EXPECT_EQ(runs(probe, "left"), 1);
    EXPECT_EQ(runs(probe, "right"), 1);
    EXPECT_EQ(runs(probe, "calm"), 1);

    const auto again = resume(*engine, "unknown", "x", "s1:left");
    ASSERT_EQ(again.status(), RunStatus::Interrupted) << "the run is still resumable";
}

TEST_P(InterruptIdentity, TwoNodesThatAskAtTheEntryAreAnsweredOneAtATime) {
    // No checkpoint precedes the step: the list and the derived ids agree.
    const auto probe  = unique_probe("entry");
    auto       engine = build(two_askers_at_entry(probe), store());
    RunConfig  config;
    config.thread_id  = "entry";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(ids_of(paused), (std::vector<std::string>{"s0:left", "s0:right"}));
    const auto again = resume(*engine, "entry", "R", "s0:right");
    ASSERT_EQ(again.status(), RunStatus::Interrupted);
    EXPECT_EQ(again.interrupt_id, "s0:left");
    const auto done = engine->resume("entry", json("L"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"left=L", "right=R"}));
}

TEST_P(InterruptIdentity, TwoSendsThatAskAreAnsweredOneAtATime) {
    for (const int workers : {1, 4}) {
        SCOPED_TRACE("workers=" + std::to_string(workers));
        const auto probe  = unique_probe("sends");
        auto       engine = build(sends(probe), store(), workers);
        RunConfig  config;
        config.thread_id  = "sends-" + std::to_string(workers);
        config.input      = json{{"items", json::array({"a", "b", "c"})},
                                 {"ask", json::array({"b", "c"})}};
        const auto paused = engine->run(config);
        ASSERT_EQ(paused.status(), RunStatus::Interrupted);
        ASSERT_EQ(paused.interrupts.size(), 2U);
        EXPECT_EQ(paused.interrupts[0].node, "worker");
        EXPECT_EQ(paused.interrupts[0].value["value"], json({{"asks", "b"}}));
        EXPECT_EQ(paused.interrupts[1].value["value"], json({{"asks", "c"}}));
        EXPECT_NE(paused.interrupts[0].id.find(":send[1]:worker:"), std::string::npos)
            << paused.interrupts[0].id;
        EXPECT_NE(paused.interrupts[1].id.find(":send[2]:worker:"), std::string::npos)
            << paused.interrupts[1].id;

        // Answer c first, by name; b asks again.
        const auto again = resume(*engine, config.thread_id, "C", paused.interrupts[1].id);
        ASSERT_EQ(again.status(), RunStatus::Interrupted);
        EXPECT_EQ(again.interrupt_id, paused.interrupts[0].id);
        EXPECT_EQ(again.interrupt_value["value"], json({{"asks", "b"}}));

        const auto done = engine->resume(config.thread_id, json("B"));
        ASSERT_EQ(done.status(), RunStatus::Completed);
        EXPECT_EQ(results_of(done),
                  (std::multiset<std::string>{"anchor", "a", "b=B", "c=C"}));
        EXPECT_EQ(runs(probe, "map"), 1);
        EXPECT_EQ(runs(probe, "worker:a"), 1) << "the finished Send replays";
        EXPECT_EQ(runs(probe, "worker:b"), 3);
        EXPECT_EQ(runs(probe, "worker:c"), 2);
    }
}

TEST_P(InterruptIdentity, ASingleInterruptHasAnIdAndKeepsItsCheckpointBytes) {
    const auto probe  = unique_probe("single");
    auto       engine = build(one_asker(probe), store());
    RunConfig  config;
    config.thread_id  = "single";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_id, "s0:left");
    EXPECT_EQ(ids_of(paused), (std::vector<std::string>{"s0:left"}));
    const auto saved = store()->load_by_id(paused.checkpoint_id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->metadata.dump().find("interrupts"), std::string::npos)
        << "no new key in a single interrupt's checkpoint: " << saved->metadata.dump();
    EXPECT_THROW((void)resume(*engine, "single", "x", "s0:other"), std::invalid_argument);
    const auto done = resume(*engine, "single", "ok", "s0:left");
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"left=ok"}));
}

TEST_P(InterruptIdentity, AResumeOfAnotherKindOfCheckpointRejectsAnInterruptId) {
    const auto probe  = unique_probe("gate");
    auto       graph  = one_asker(probe);
    graph["interrupt_before"] = json::array({"left"});
    auto       engine = build(graph, store());
    RunConfig  config;
    config.thread_id  = "gate";
    const auto paused = engine->run(config);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_TRUE(paused.interrupt_id.empty()) << "a gate is not a NodeInterrupt";
    EXPECT_TRUE(paused.interrupts.empty());
    EXPECT_THROW((void)resume(*engine, "gate", "x", "s0:left"), std::invalid_argument);
}

TEST(InterruptIdentitySqlite, AFreshEngineAnswersTheNamedInterruptAfterARestart) {
    const TempDir dir;
    const auto    probe = unique_probe("restart");
    const auto    file  = dir.file("checkpoints.sqlite");
    std::string   right_id;
    {
        auto       engine = build(two_askers(probe), std::make_shared<SqliteCheckpointStore>(file));
        RunConfig  config;
        config.thread_id  = "restart";
        const auto paused = engine->run(config);
        ASSERT_EQ(paused.status(), RunStatus::Interrupted);
        right_id = paused.interrupts.at(1).id;
    }
    auto       engine = build(two_askers(probe), std::make_shared<SqliteCheckpointStore>(file));
    const auto again  = resume(*engine, "restart", "R", right_id);
    ASSERT_EQ(again.status(), RunStatus::Interrupted);
    EXPECT_EQ(again.interrupt_node, "left");
    const auto done = engine->resume("restart", json("L"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"anchor", "calm", "left=L", "right=R"}));
    EXPECT_EQ(runs(probe, "calm"), 1);
}

INSTANTIATE_TEST_SUITE_P(Stores, InterruptIdentity, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Sqlite" : "InMemory"; });
