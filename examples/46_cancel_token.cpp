// NeoGraph Example 41: Cooperative cancellation via CancelToken
//
// Demonstrates how a long-running graph can be aborted from another
// thread (or signal handler, or HTTP cancel button) by sharing a
// CancelToken via RunConfig::cancel_token.
//
// No API key — uses a sleep-and-increment node so the cancel
// observably truncates the run mid-flight.
//
// Usage:  ./example_cancel_token

#include <neograph/neograph.h>
#include <neograph/graph/cancel.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace neograph;
using namespace neograph::graph;

// Each invocation polls is_cancelled() between 50 ms naps; throws
// CancelledException so the engine unwinds cleanly.
class SlowIncNode : public GraphNode {
public:
    explicit SlowIncNode(std::string n, std::shared_ptr<CancelToken> tok)
        : n_(std::move(n)), tok_(std::move(tok)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        for (int i = 0; i < 20; ++i) {
            if (tok_ && tok_->is_cancelled()) {
                throw CancelledException("aborted inside " + n_);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        int cur = 0;
        auto v = in.state.get("counter");
        if (v.is_number()) cur = v.get<int>();
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"counter", json(cur + 1)});
        co_return out;
    }
    std::string get_name() const override { return n_; }

private:
    std::string n_;
    std::shared_ptr<CancelToken> tok_;
};

int main() {
    auto tok = std::make_shared<CancelToken>();

    // 4-node chain — each takes ~1s. Total ~4s if uncancelled.
    NodeFactory::instance().register_type("slow_inc",
        [tok](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SlowIncNode>(name, tok);
        });

    json def = {
        {"name", "slow_chain"},
        {"channels", {{"counter", {{"reducer", "overwrite"}}}}},
        {"nodes", {
            {"a", {{"type", "slow_inc"}}},
            {"b", {{"type", "slow_inc"}}},
            {"c", {{"type", "slow_inc"}}},
            {"d", {{"type", "slow_inc"}}},
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"}, {"to", "b"}},
            {{"from", "b"}, {"to", "c"}},
            {{"from", "c"}, {"to", "d"}},
            {{"from", "d"}, {"to", "__end__"}},
        }},
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);

    RunConfig cfg;
    cfg.input = {{"counter", 0}};
    cfg.cancel_token = tok;

    // Fire the cancel from another thread after 250 ms — should land
    // mid-way through node "a" or "b".
    std::thread killer([tok]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::cerr << "[killer] requesting cancel...\n";
        tok->cancel();
    });

    auto t0 = std::chrono::steady_clock::now();
    bool cancelled = false;
    try {
        auto result = engine->run(cfg);
        std::cerr << "[run] finished without cancel — counter=" << result.output.dump() << "\n";
    } catch (const CancelledException& e) {
        cancelled = true;
        std::cerr << "[run] cancelled: " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[run] unexpected exception: " << e.what() << "\n";
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    killer.join();

    std::cout << "elapsed_ms=" << elapsed.count()
              << " cancelled=" << (cancelled ? "true" : "false")
              << " token_state=" << (tok->is_cancelled() ? "cancelled" : "active") << "\n";

    // Smoke gate — cancel should land well before the natural 4s
    // completion. Allow generous slack for CI noise.
    if (!cancelled) {
        std::cerr << "FAIL: cancellation did not propagate\n";
        return 1;
    }
    if (elapsed.count() > 2000) {
        std::cerr << "FAIL: cancel took " << elapsed.count() << "ms (> 2s budget)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
