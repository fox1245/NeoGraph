// NeoGraph Example 101: RetryPolicy
//
// A flaky node fails twice and works on the third attempt. A RetryPolicy tells the engine to
// run it again after a growing pause, so the caller only sees the final result:
//
//   pause before retry n = initial_delay_ms * backoff_multiplier^(n-1), capped at max_delay_ms
//
// The policy here applies to one node (EngineConfig::node_retry_policies); EngineConfig::retry_policy
// sets a default for every node. A second run with a smaller budget shows what the caller gets
// when the retries run out: a NodeExecutionError that says how many attempts were made.
//
// No API key required.
//
// Usage: ./example_retry_policy
//
// smoke-expect: max_retries=3: ok, attempts: 3, answer: done, backoff: 20 ms, 40 ms
// smoke-expect: max_retries=1: gave up after 2 attempts, backoff: 20 ms
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <atomic>
#include <iostream>
#include <stdexcept>

using namespace neograph;
using namespace neograph::graph;

// Fails until it has been called three times.
class FlakyNode : public SyncGraphNode {
public:
    explicit FlakyNode(std::shared_ptr<std::atomic<int>> calls)
        : SyncGraphNode("flaky"), calls_(std::move(calls)) {}

protected:
    NodeOutput run_sync(NodeInput) override {
        if (++*calls_ < 3) throw std::runtime_error("upstream timeout");
        return NodeOutput{{ChannelWrite{"answer", json("done")}}};
    }

private:
    std::shared_ptr<std::atomic<int>> calls_;
};

int main() {
    auto calls = std::make_shared<std::atomic<int>>(0);
    NodeFactory::instance().register_type("flaky",
        [calls](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<FlakyNode>(calls);
        });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "retry_policy"},
        {"channels", {{"answer", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"flaky", {{"type", "flaky"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "flaky"}},
            {{"from", "flaky"}, {"to", "__end__"}}
        })}
    };

    for (const int max_retries : {3, 1}) {
        calls->store(0);

        RetryPolicy policy;
        policy.max_retries         = max_retries;  // retries after the first attempt
        policy.initial_delay_ms    = 20;           // pause before the first retry
        policy.backoff_multiplier  = 2.0f;         // each pause is twice the last one
        policy.max_delay_ms        = 200;          // and never longer than this
        EngineConfig config;
        config.node_retry_policies["flaky"] = policy;
        auto engine = GraphEngine::build(definition, std::move(config));

        // In DEBUG mode every failed attempt that will be retried raises an ERROR event
        // that carries the pause the engine is about to take.
        std::string backoff;
        RunConfig   run;
        run.stream_mode = StreamMode::DEBUG;
        auto on_event   = [&](const GraphEvent& event) {
            if (event.type != GraphEvent::Type::ERROR) return;
            backoff += (backoff.empty() ? "" : ", ") + std::to_string(event.data["delay_ms"].get<int>()) + " ms";
        };

        std::cout << "max_retries=" << max_retries << ": ";
        try {
            auto result = engine->run_stream(run, on_event);
            std::cout << "ok, attempts: " << calls->load()
                      << ", answer: " << result.channel<std::string>("answer");
        } catch (const NodeExecutionError& error) {
            std::cout << "gave up after " << error.attempts() << " attempts";
        }
        std::cout << ", backoff: " << backoff << "\n";
    }
    return 0;
}
