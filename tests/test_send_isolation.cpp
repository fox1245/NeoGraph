// #389: one Send contract for one Send or many.
//
// The node executor had two Send paths. A step with several Sends ran each
// target on an isolated copy of the state, its payload applied to that copy
// only. A step with a single Send applied the payload to the shared run state
// and ran the target on it, so the payload leaked: the nodes of the next step
// read it, and it stayed in the run's final state. A resumed single Send that
// replayed did not apply it, so even the same step disagreed with itself.
//
// The contract, for one Send and for many, live and replayed, with and
// without a worker pool: a payload is visible only to its own Send execution,
// including every retry attempt of it; what the step leaves behind is the
// writes the targets return.

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
#include <stdexcept>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

std::mutex&                 runs_mutex() { static std::mutex m; return m; }
std::map<std::string, int>& runs_map() { static std::map<std::string, int> m; return m; }

int count_run(const std::string& key) {
    std::lock_guard lock(runs_mutex());
    return ++runs_map()[key];
}

int runs(const std::string& key) {
    std::lock_guard lock(runs_mutex());
    const auto      found = runs_map().find(key);
    return found == runs_map().end() ? 0 : found->second;
}

std::string unique_probe() {
    static std::atomic<int> next{0};
    return "isolation-" + std::to_string(++next);
}

std::string text(const json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

// One Send to "worker" per entry of "items". The first payload also carries
// a "secret", which no other Send may see.
class MapNode final : public GraphNode {
public:
    explicit MapNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        NodeOutput out;
        const auto items = in.state.get("items");
        for (std::size_t index = 0; index < items.size(); ++index) {
            json payload{{"item", items[index]}};
            if (index == 0) payload["secret"] = "s-" + items[index].get<std::string>();
            out.sends.push_back(Send{"worker", std::move(payload)});
        }
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Reports what it sees. The item named by "flaky" fails its first attempt;
// the one named by "ask" asks until it is answered.
class WorkerNode final : public GraphNode {
public:
    WorkerNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto item    = text(in.state.get("item"));
        const auto probe   = config_.value("probe", std::string());
        const int  attempt = count_run(probe + "/worker:" + item);
        if (in.state.get("flaky") == json(item) && attempt == 1)
            throw std::runtime_error("transient failure of " + item);
        if (in.state.get("ask") == json(item) && !in.ctx.resume_value)
            throw NodeInterrupt("approve " + item, json{{"item", item}});
        NodeOutput out;
        out.writes.push_back(ChannelWrite{
            "seen", json::array({item + ":secret=" + text(in.state.get("secret"))})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Runs in the step after the Sends and reports the shared state it reads.
class AfterNode final : public GraphNode {
public:
    explicit AfterNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{
            "after", json::array({"item=" + text(in.state.get("item")) +
                                  ",secret=" + text(in.state.get("secret"))})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

void register_types() {
    static const bool registered = [] {
        auto& factory = NodeFactory::instance();
        factory.register_type(
            "send_isolation.map",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<MapNode>(name);
            },
            json{{"type", "object"}});
        factory.register_type(
            "send_isolation.worker",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<WorkerNode>(name, config);
            },
            json{{"type", "object"}});
        factory.register_type(
            "send_isolation.after",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<AfterNode>(name);
            },
            json{{"type", "object"}});
        return true;
    }();
    (void)registered;
}

json definition(const std::string& probe) {
    const auto overwrite = [](json initial) {
        return json{{"reducer", "overwrite"}, {"initial", std::move(initial)}};
    };
    const auto append = json{{"reducer", "append"}, {"initial", json::array()}};
    return json{{"name", "send_isolation"},
                {"channels", json{{"items", overwrite(json::array())},
                                  {"item", overwrite("")},
                                  {"secret", overwrite("")},
                                  {"flaky", overwrite("")},
                                  {"ask", overwrite("")},
                                  {"seen", append},
                                  {"after", append}}},
                {"nodes", json{{"map", json{{"type", "send_isolation.map"}}},
                               {"worker", json{{"type", "send_isolation.worker"}, {"probe", probe}}},
                               {"after", json{{"type", "send_isolation.after"}}}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "map"}},
                                       json{{"from", "map"}, {"to", "__end__"}},
                                       json{{"from", "worker"}, {"to", "after"}},
                                       json{{"from", "after"}, {"to", "__end__"}}})}};
}

std::unique_ptr<GraphEngine> build(const std::string& probe, std::shared_ptr<CheckpointStore> store,
                                   int workers) {
    register_types();
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.stderr_notices   = false;
    config.worker_count     = workers;
    auto engine             = GraphEngine::build(definition(probe), std::move(config));
    RetryPolicy retry;
    retry.max_retries      = 1;
    retry.initial_delay_ms = 0;
    retry.max_delay_ms     = 0;
    engine->set_node_retry_policy("worker", retry);
    return engine;
}

json channel(const RunResult& result, const char* name) {
    return result.output["channels"][name]["value"];
}

std::multiset<std::string> sorted(const json& values) {
    std::multiset<std::string> out;
    for (std::size_t index = 0; index < values.size(); ++index)
        out.insert(values[index].get<std::string>());
    return out;
}

// What every case must observe: each Send saw its own payload and only its
// own, and the next step and the final state see none of them.
void expect_isolated(const RunResult& done, const std::vector<std::string>& items) {
    ASSERT_EQ(done.status(), RunStatus::Completed);
    std::multiset<std::string> seen;
    for (std::size_t index = 0; index < items.size(); ++index)
        seen.insert(items[index] + ":secret=" + (index == 0 ? "s-" + items[index] : ""));
    EXPECT_EQ(sorted(channel(done, "seen")), seen) << "a payload is visible only to its own Send";
    EXPECT_EQ(channel(done, "after"), json::array({"item=,secret="}))
        << "the next step sees no Send payload";
    EXPECT_EQ(channel(done, "item"), json("")) << "no payload is left in the run state";
    EXPECT_EQ(channel(done, "secret"), json(""));
}

struct Case {
    std::size_t sends   = 1;
    int         workers = 1;
    std::string name() const {
        return std::to_string(sends) + (sends == 1 ? " Send" : " Sends") + ", workers=" +
               std::to_string(workers);
    }
};

std::vector<Case> matrix() {
    return {{1, 1}, {1, 4}, {3, 1}, {3, 4}};
}

std::vector<std::string> items_for(const Case& c) {
    std::vector<std::string> items;
    for (std::size_t index = 0; index < c.sends; ++index)
        items.push_back(std::string(1, static_cast<char>('a' + index)));
    return items;
}

json as_json(const std::vector<std::string>& items) {
    json out = json::array();
    for (const auto& item : items) out.push_back(item);
    return out;
}

class TempDir : public neograph::graph::test_support::UniqueTempDir {
public:
    TempDir() : UniqueTempDir("ng-send-isolation-") {}
};

}  // namespace

TEST(SendIsolation, EachPayloadIsVisibleOnlyToItsOwnSend) {
    for (const auto& c : matrix()) {
        SCOPED_TRACE(c.name());
        const auto probe  = unique_probe();
        auto       engine = build(probe, nullptr, c.workers);
        const auto items  = items_for(c);
        RunConfig  config;
        config.thread_id = "isolation";
        config.input     = json{{"items", as_json(items)}};
        expect_isolated(engine->run(config), items);
    }
}

TEST(SendIsolation, ARetriedSendSeesItsOwnPayloadOnEveryAttempt) {
    // Both paths retry, and the retry runs on the Send's own state again.
    for (const auto& c : matrix()) {
        SCOPED_TRACE(c.name());
        const auto probe  = unique_probe();
        auto       engine = build(probe, nullptr, c.workers);
        const auto items  = items_for(c);
        RunConfig  config;
        config.thread_id = "retry";
        config.input     = json{{"items", as_json(items)}, {"flaky", items.front()}};
        expect_isolated(engine->run(config), items);
        EXPECT_EQ(runs(probe + "/worker:" + items.front()), 2) << "the failed attempt was retried";
    }
}

TEST(SendIsolation, AResumedStepKeepsThePayloadsIsolated) {
    // A Send asks; resume replays the Sends that finished and runs the one
    // that asked. Live or replayed, no payload reaches the shared state. In
    // memory and on SQLite.
    for (const bool durable : {false, true}) {
        for (const auto& c : matrix()) {
            SCOPED_TRACE(c.name() + (durable ? ", SQLite" : ", in memory"));
            TempDir    dir;
            const auto probe = unique_probe();
            std::shared_ptr<CheckpointStore> store;
            if (durable) store = std::make_shared<SqliteCheckpointStore>(dir.file("cp.sqlite"));
            else store = std::make_shared<InMemoryCheckpointStore>();
            auto       engine = build(probe, store, c.workers);
            const auto items  = items_for(c);
            RunConfig  config;
            config.thread_id  = "resume";
            config.input      = json{{"items", as_json(items)}, {"ask", items.back()}};
            const auto paused = engine->run(config);
            ASSERT_EQ(paused.status(), RunStatus::Interrupted);
            EXPECT_EQ(channel(paused, "item"), json("")) << "the paused step's state has no payload";

            expect_isolated(engine->resume("resume", json("yes")), items);
            for (std::size_t index = 0; index + 1 < items.size(); ++index)
                EXPECT_EQ(runs(probe + "/worker:" + items[index]), 1)
                    << "a finished Send replays";
        }
    }
}
