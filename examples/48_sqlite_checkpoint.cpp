// NeoGraph Example 43: SqliteCheckpointStore — single-file durable runs
//
// Mirror of the Postgres checkpoint example but with SQLite — no DB
// server, single .db file. Demonstrates the resume_if_exists flow:
// first run saves a checkpoint, second run resumes from where it left
// off (HITL pattern), third run reads back history with list().
//
// Uses ":memory:" backing so the example is self-contained.
//
// Usage: ./example_sqlite_checkpoint

#include <neograph/neograph.h>
#include <neograph/graph/sqlite_checkpoint.h>

#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// Simple counter node — increments and tags itself in the history.
class StampNode : public GraphNode {
public:
    explicit StampNode(std::string n) : n_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int cur = 0;
        auto v = in.state.get("counter");
        if (v.is_number()) cur = v.get<int>();
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"counter", json(cur + 1)});
        out.writes.push_back(ChannelWrite{"last", json(n_)});
        co_return out;
    }
    std::string get_name() const override { return n_; }
private:
    std::string n_;
};

int main() {
    NodeFactory::instance().register_type("stamp",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<StampNode>(name);
        });

    // Two-node chain — a → b.
    json def = {
        {"name", "checkpoint_demo"},
        {"channels", {
            {"counter", {{"reducer", "overwrite"}}},
            {"last",    {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"a", {{"type", "stamp"}}},
            {"b", {{"type", "stamp"}}},
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"}, {"to", "b"}},
            {{"from", "b"}, {"to", "__end__"}},
        }},
    };

    auto store = std::make_shared<SqliteCheckpointStore>(":memory:");
    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx, store);

    // RunResult::channel<T>(name) handles both shapes — channels-wrapper
    // (canonical) and flat-key projections (e.g. react_graph's
    // final_response). See engine.h's RunResult docstring for the full
    // shape contract. Issue #25.

    // First run on thread "alice".
    RunConfig cfg;
    cfg.thread_id = "alice";
    cfg.input = {{"counter", 0}};
    auto r1 = engine->run(cfg);
    std::cout << "run1 output=" << r1.output.dump() << "\n";

    // List checkpoints — should have entries for the two steps.
    auto hist = store->list("alice");
    std::cout << "alice has " << hist.size() << " checkpoint(s) after run 1\n";
    if (hist.empty()) {
        std::cerr << "FAIL: expected checkpoints saved for thread alice\n";
        return 1;
    }

    // Second run — same thread_id + resume_if_exists. With this flag the engine
    // picks up from the last saved state rather than re-running from input.
    RunConfig cfg2;
    cfg2.thread_id = "alice";
    cfg2.input = {{"counter", 0}};   // ignored when resume_if_exists hits
    cfg2.resume_if_exists = true;
    auto r2 = engine->run(cfg2);
    std::cout << "run2 (resumed) output=" << r2.output.dump() << "\n";

    // Independent thread "bob" — own state, own history.
    RunConfig cfg3;
    cfg3.thread_id = "bob";
    cfg3.input = {{"counter", 100}};
    auto r3 = engine->run(cfg3);
    std::cout << "run3 (bob) output=" << r3.output.dump() << "\n";

    auto bob_hist = store->list("bob");
    auto alice_hist = store->list("alice");
    std::cout << "alice=" << alice_hist.size()
              << " bob=" << bob_hist.size()
              << " blobs=" << store->blob_count() << "\n";

    // Gates.
    bool ok = true;
    if (alice_hist.size() < 2) {
        std::cerr << "FAIL: expected at least 2 alice checkpoints (got " << alice_hist.size() << ")\n";
        ok = false;
    }
    if (bob_hist.size() < 2) {
        std::cerr << "FAIL: expected at least 2 bob checkpoints (got " << bob_hist.size() << ")\n";
        ok = false;
    }
    // resume_if_exists should not have re-counted from 0.
    int r2_counter = r2.has_channel("counter") ? r2.channel<int>("counter") : -1;
    if (r2_counter < 2) {
        std::cerr << "FAIL: resume_if_exists did not pick up prior state (counter="
                  << r2_counter << ")\n";
        ok = false;
    }
    if (!ok) return 1;
    std::cout << "PASS\n";
    return 0;
}
