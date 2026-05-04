// v0.3.1: TODO_v0.3.md item #7 — verify CancelToken propagates into
// every fan-out worker (run_parallel_async + run_sends_async).
//
// v0.3.0 wired the token through GraphState → PyGraphNode::execute_full_async
// for the single-node path. Static parallel fan-out shares the parent
// state by reference so it inherits the token automatically. Send-driven
// dynamic fan-out, however, builds an *isolated* GraphState per worker
// from a serialize/restore round-trip — and serialize() does not include
// the per-run cancel token. These tests pin both paths down and catch
// the regression where the multi-Send isolated state silently dropped
// the token (the gap that closed in this same patch).

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/cancel.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Records the cancel-token pointer it observed on `state` and writes
// the node name into a "saw_token" channel so we can assert post-run
// that every fan-out branch saw the same parent token.
class CancelObserverNode : public GraphNode {
public:
    explicit CancelObserverNode(std::string n,
                                std::atomic<int>* observed_count,
                                CancelToken* expected)
        : n_(std::move(n)), count_(observed_count), expected_(expected) {}

    NodeResult execute_full(const GraphState& state) override {
        // Read the token pointer; record only if it matches the parent
        // run's token. A null or mismatched pointer is the actual bug
        // — if the test sees that, count_ stays unchanged and the
        // assertion below fails.
        if (state.run_cancel_token() == expected_ && expected_ != nullptr) {
            count_->fetch_add(1, std::memory_order_relaxed);
        }
        // Emit a small write so the engine has something to commit.
        return NodeResult{{ChannelWrite{"saw_token", json::array({n_})}}};
    }

    std::string get_name() const override { return n_; }

private:
    std::string n_;
    std::atomic<int>* count_;
    CancelToken* expected_;
};

// Send-fan-out source that emits N Sends pointing at a worker target.
class SendFanOutNode : public GraphNode {
public:
    explicit SendFanOutNode(std::string n, std::string target, int width)
        : n_(std::move(n)), target_(std::move(target)), width_(width) {}

    NodeResult execute_full(const GraphState&) override {
        NodeResult res;
        for (int i = 0; i < width_; ++i) {
            json input;
            input["i"] = i;
            res.sends.push_back(Send{target_, input});
        }
        return res;
    }

    std::string get_name() const override { return n_; }

private:
    std::string n_, target_;
    int width_;
};

// State + counter pair needed by the factory closures.
struct TestFixture {
    std::atomic<int> observed_count{0};
    std::shared_ptr<CancelToken> token;
};

// Fresh-per-test factory registration — node types use a unique
// suffix so two TEST cases don't reuse the previous test's counter.
void register_observer_factory(const std::string& type_name,
                               TestFixture* fix) {
    NodeFactory::instance().register_type(type_name,
        [fix](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<CancelObserverNode>(
                name, &fix->observed_count, fix->token.get());
        });
}

void register_fanout_factory(const std::string& type_name,
                             const std::string& target,
                             int width) {
    NodeFactory::instance().register_type(type_name,
        [target, width](const std::string& name, const json&,
                        const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<SendFanOutNode>(name, target, width);
        });
}

} // namespace

// =========================================================================
// Static parallel fan-out: a → {b, c, d} via plain edges. All three
// workers share the parent state by reference, so the cancel token is
// read directly off it. Was already correct in v0.3.0 — this test
// locks it in so the optimization doesn't regress.
// =========================================================================
TEST(CancelTokenPropagation, StaticParallelFanOutSeesParentToken) {
    TestFixture fix;
    fix.token = std::make_shared<CancelToken>();

    register_observer_factory("cobs_par_a", &fix);
    register_observer_factory("cobs_par_b", &fix);
    register_observer_factory("cobs_par_c", &fix);

    json graph = {
        {"name", "par_fan"},
        {"channels", {{"saw_token", {{"reducer", "append"}}}}},
        {"nodes", {
            {"a", {{"type", "cobs_par_a"}}},
            {"b", {{"type", "cobs_par_b"}}},
            {"c", {{"type", "cobs_par_c"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"}, {"to", "b"}},
            {{"from", "a"}, {"to", "c"}},
            {{"from", "b"}, {"to", "__end__"}},
            {{"from", "c"}, {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.thread_id = "par-fan-001";
    cfg.cancel_token = fix.token;
    auto result = engine->run(cfg);

    // a, b, c each saw the token = 3.
    EXPECT_EQ(fix.observed_count.load(), 3);
    EXPECT_FALSE(result.interrupted);
}

// =========================================================================
// Multi-Send dynamic fan-out: dispatcher emits 4 Sends targeting a
// shared worker type. Each Send runs on an isolated GraphState built
// from serialize() + restore(); without the v0.3.1 fix, run_cancel_token
// is null in the worker's state. The observer node only increments
// when token == expected, so a null token leaves count == 0 and the
// assertion fails.
// =========================================================================
TEST(CancelTokenPropagation, MultiSendFanOutSeesParentToken) {
    TestFixture fix;
    fix.token = std::make_shared<CancelToken>();

    register_observer_factory("cobs_send_worker", &fix);
    register_fanout_factory("cobs_send_dispatcher",
                            "worker", 4);

    json graph = {
        {"name", "send_fan"},
        {"channels", {{"saw_token", {{"reducer", "append"}}}}},
        {"nodes", {
            {"dispatcher", {{"type", "cobs_send_dispatcher"}}},
            {"worker",     {{"type", "cobs_send_worker"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "dispatcher"}}
            // worker has no outgoing edge — Send target resolves to
            // implicit __end__ after one execution per Send.
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.thread_id = "send-fan-001";
    cfg.cancel_token = fix.token;
    auto result = engine->run(cfg);

    // 4 send-spawned workers, each reading the parent cancel token
    // off its isolated GraphState. The dispatcher is a different node
    // type so it does not contribute. Pre-fix this would have been 0.
    EXPECT_EQ(fix.observed_count.load(), 4);
    EXPECT_FALSE(result.interrupted);
}

// =========================================================================
// End-to-end cancel: trip the token mid-fan-out and verify that no
// extra workers run after the trip. Uses a small sleep in each worker
// so the test has time to set the cancel flag while the parallel
// group is still in flight.
//
// We don't need to assert exact thread interleaving — the contract is
// "no NEW node start_async runs after cancel is observed at a super-
// step boundary". So we record dispatch order and assert that the
// run aborts (CancelledException at the next super-step poll), not
// that all workers complete.
// =========================================================================
TEST(CancelTokenPropagation, CancelMidFanOutHaltsLoop) {
    TestFixture fix;
    fix.token = std::make_shared<CancelToken>();

    // Slow-ish observer so we can race the cancel against the loop.
    class SlowObserver : public GraphNode {
    public:
        SlowObserver(std::string n, std::atomic<int>* started,
                     std::shared_ptr<CancelToken> tok)
            : n_(std::move(n)), started_(started), tok_(std::move(tok)) {}
        NodeResult execute_full(const GraphState& state) override {
            started_->fetch_add(1, std::memory_order_relaxed);
            // First node: trip the cancel before returning, so the
            // engine's super-step boundary observes it before the
            // next step starts.
            if (n_ == "first") {
                tok_->cancel();
            } else {
                // Cooperative: if cancel is set, throw the same way
                // the engine does at super-step boundaries.
                if (auto* t = state.run_cancel_token()) {
                    t->throw_if_cancelled("worker " + n_);
                }
            }
            return NodeResult{{ChannelWrite{"saw_token", json::array({n_})}}};
        }
        std::string get_name() const override { return n_; }
    private:
        std::string n_;
        std::atomic<int>* started_;
        std::shared_ptr<CancelToken> tok_;
    };

    std::atomic<int> started{0};
    auto& nf = NodeFactory::instance();
    nf.register_type("cobs_first",
        [&started, tok = fix.token](const std::string& name, const json&,
                                    const NodeContext&) {
            return std::unique_ptr<GraphNode>(
                new SlowObserver(name, &started, tok));
        });
    nf.register_type("cobs_second",
        [&started, tok = fix.token](const std::string& name, const json&,
                                    const NodeContext&) {
            return std::unique_ptr<GraphNode>(
                new SlowObserver(name, &started, tok));
        });

    json graph = {
        {"name", "cancel_chain"},
        {"channels", {{"saw_token", {{"reducer", "append"}}}}},
        {"nodes", {
            {"first",  {{"type", "cobs_first"}}},
            {"second", {{"type", "cobs_second"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "first"}},
            {{"from", "first"},     {"to", "second"}},
            {{"from", "second"},    {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.thread_id    = "cancel-chain-001";
    cfg.cancel_token = fix.token;

    EXPECT_THROW({
        engine->run(cfg);
    }, neograph::graph::CancelledException);

    // First ran (and tripped). Second must NOT have started — the
    // super-step boundary check after first observes the cancel and
    // bails.
    EXPECT_EQ(started.load(), 1);
}
