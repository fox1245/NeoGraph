// Issue #27 — RunContext::store plumbing.
//
// The engine populates ctx.store from GraphEngine::set_store(...) at
// the top of every run, so node bodies can reach the Store through
// in.ctx.store without the factory-closure capture workaround.
//
// Validates: (a) ctx.store is the same shared_ptr the engine was
// configured with, (b) reads inside a running node see whatever the
// caller put into the Store before run(), (c) ctx.store is nullptr
// when no Store is configured (back-compat).

#include <gtest/gtest.h>

#include <neograph/neograph.h>
#include <neograph/graph/store.h>

#include <asio/awaitable.hpp>
#include <atomic>
#include <memory>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Records what it saw on ctx.store during execute. Test asserts on
// the recorded values after run() returns.
struct StoreProbe {
    std::atomic<bool> ctx_store_was_null{true};
    std::atomic<bool> read_succeeded{false};
    std::string       read_value;
    std::mutex        mu;
};

class ProbeNode : public GraphNode {
public:
    ProbeNode(std::string n, std::shared_ptr<StoreProbe> probe,
              std::string ns, std::string key)
        : n_(std::move(n)), probe_(std::move(probe)),
          ns_{std::move(ns)}, key_(std::move(key)) {}

    // Use the v0.4 unified run() entry so we receive NodeInput
    // (which carries RunContext via in.ctx).
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        probe_->ctx_store_was_null.store(in.ctx.store == nullptr,
                                         std::memory_order_release);
        if (in.ctx.store) {
            auto v = in.ctx.store->get(ns_, key_);
            if (v) {
                std::lock_guard<std::mutex> lk(probe_->mu);
                probe_->read_value = v->value.is_string()
                    ? v->value.get<std::string>()
                    : v->value.dump();
                probe_->read_succeeded.store(true, std::memory_order_release);
            }
        }
        NodeOutput out;
        out.writes.push_back({"hits", json(1)});
        co_return out;
    }

    std::string get_name() const override { return n_; }

private:
    std::string n_;
    std::shared_ptr<StoreProbe> probe_;
    Namespace   ns_;
    std::string key_;
};

json make_def() {
    return {
        {"name", "probe_graph"},
        {"channels", {{"hits", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"probe", {{"type", "probe"}}}}},
        {"edges", {
            {{"from", "__start__"}, {"to", "probe"}},
            {{"from", "probe"},     {"to", "__end__"}},
        }},
    };
}

}  // namespace

TEST(RunContextStore, NullByDefaultWhenEngineHasNoStore) {
    auto probe = std::make_shared<StoreProbe>();
    NodeFactory::instance().register_type("probe",
        [probe](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ProbeNode>(name, probe, "users", "alice");
        });

    NodeContext ctx;
    auto engine = GraphEngine::compile(make_def(), ctx);
    // No set_store call.

    RunConfig cfg;
    cfg.input = {{"hits", 0}};
    engine->run(cfg);

    EXPECT_TRUE(probe->ctx_store_was_null.load());
    EXPECT_FALSE(probe->read_succeeded.load());
}

TEST(RunContextStore, PopulatedFromEngineSetStore) {
    auto probe = std::make_shared<StoreProbe>();
    auto store = std::make_shared<InMemoryStore>();
    store->put(Namespace{"users"}, "alice",
        json{{"role", "admin"}, {"city", "Seoul"}});

    NodeFactory::instance().register_type("probe",
        [probe](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ProbeNode>(name, probe, "users", "alice");
        });

    NodeContext ctx;
    auto engine = GraphEngine::compile(make_def(), ctx);
    engine->set_store(store);

    RunConfig cfg;
    cfg.input = {{"hits", 0}};
    engine->run(cfg);

    EXPECT_FALSE(probe->ctx_store_was_null.load());
    EXPECT_TRUE(probe->read_succeeded.load());
    EXPECT_NE(probe->read_value.find("Seoul"), std::string::npos);
}

TEST(RunContextStore, MultipleRunsSeeSameStore) {
    // Repeat the populated case three times — same engine, same Store
    // configured. Reading through ctx.store across runs should be
    // stable.
    //
    // GraphEngine::compile() resolves the node factory once and reuses
    // the same node instance across runs, so the probe captured by the
    // factory closure is shared. We reset its flags between iterations
    // and assert per-iteration that the most-recent run populated them
    // again — that proves ctx.store is repopulated every dispatch.
    auto store = std::make_shared<InMemoryStore>();
    store->put(Namespace{"k"}, "v", json("stable"));

    auto probe = std::make_shared<StoreProbe>();
    NodeFactory::instance().register_type("probe",
        [probe](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ProbeNode>(name, probe, "k", "v");
        });

    NodeContext ctx;
    auto engine = GraphEngine::compile(make_def(), ctx);
    engine->set_store(store);

    for (int i = 0; i < 3; ++i) {
        // Reset probe state so the assertion at the end of this
        // iteration is about THIS run, not a stale value from i-1.
        probe->ctx_store_was_null.store(true, std::memory_order_release);
        probe->read_succeeded.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(probe->mu);
            probe->read_value.clear();
        }

        RunConfig cfg;
        cfg.input = {{"hits", 0}};
        engine->run(cfg);

        EXPECT_FALSE(probe->ctx_store_was_null.load()) << "iter " << i;
        EXPECT_TRUE(probe->read_succeeded.load()) << "iter " << i;
        std::lock_guard<std::mutex> lk(probe->mu);
        EXPECT_EQ(probe->read_value, "stable") << "iter " << i;
    }
}
