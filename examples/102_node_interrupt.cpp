// NeoGraph Example 102: NodeInterrupt, a breakpoint the node itself raises
//
// interrupt_before stops a graph in front of a fixed node. NodeInterrupt is for the case where
// the node decides: a small refund goes through, a large one needs a person. The node throws,
// the engine saves a checkpoint and run() returns with `interrupted == true`; the payload
// tells the caller what to ask. engine->resume(thread, answer) runs the node again, and this
// time `in.ctx.resume_value` holds the answer. An answer is not always "yes": the node reads
// it and may refuse.
//
// The node runs again from the top, so keep everything before the throw free of side effects.
// A checkpoint store is required; the thread_id names the paused run.
//
// No API key required.
//
// Usage: ./example_node_interrupt
//
// smoke-expect: small refund: Refund of 40 USD sent
// smoke-expect: paused: node=refund reason="Refund of 250 USD needs approval" amount=250
// smoke-expect: resumed: Refund of 250 USD sent (approved by alice)
// smoke-expect: refused: Refund of 900 USD declined (bob)
// smoke-expect: node ran 5 times for 3 refunds
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <atomic>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

constexpr int kLimit = 100;   // refunds above this need approval

class RefundNode : public SyncGraphNode {
public:
    explicit RefundNode(std::shared_ptr<std::atomic<int>> visits)
        : SyncGraphNode("refund"), visits_(std::move(visits)) {}

protected:
    NodeOutput run_sync(NodeInput in) override {
        ++*visits_;
        const int         amount = in.state.get("amount").get<int>();
        const std::string money  = "Refund of " + std::to_string(amount) + " USD";

        std::string result = money + " sent";
        if (amount > kLimit) {
            // Empty on the first visit; holds the caller's answer after resume().
            const auto& answer = in.ctx.resume_value;
            if (!answer)
                throw NodeInterrupt(money + " needs approval", json{{"amount", amount}});
            const std::string who = answer->value("by", "?");
            result = answer->value("approved", false) ? money + " sent (approved by " + who + ")"
                                                      : money + " declined (" + who + ")";
        }
        return NodeOutput{{ChannelWrite{"result", json(result)}}};
    }

private:
    std::shared_ptr<std::atomic<int>> visits_;
};

int main() {
    auto visits = std::make_shared<std::atomic<int>>(0);
    NodeFactory::instance().register_type("refund",
        [visits](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<RefundNode>(visits);
        });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "node_interrupt"},
        {"channels", {{"amount", {{"reducer", "overwrite"}}}, {"result", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"refund", {{"type", "refund"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "refund"}},
            {{"from", "refund"}, {"to", "__end__"}}
        })}
    };

    EngineConfig config;
    config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();   // needed to pause
    auto engine = GraphEngine::build(definition, std::move(config));

    auto start = [&](const std::string& thread, int amount) {
        RunConfig run;
        run.thread_id = thread;
        run.input     = {{"amount", amount}};
        return engine->run(run);
    };

    // 1. Under the limit: the node never throws.
    std::cout << "small refund: " << start("t-small", 40).channel<std::string>("result") << "\n";

    // 2. Over the limit: the run pauses, the payload says what to ask.
    auto paused = start("t-large", 250);
    std::cout << "paused: node=" << paused.interrupt_node
              << " reason=\"" << paused.interrupt_value.value("reason", "") << "\""
              << " amount=" << paused.interrupt_value["value"]["amount"].get<int>() << "\n";

    // 3. An operator answers; the node runs again and acts on the answer.
    auto resumed = engine->resume("t-large", json{{"approved", true}, {"by", "alice"}});
    std::cout << "resumed: " << resumed.channel<std::string>("result") << "\n";

    // 4. A refusal is an answer too.
    start("t-refused", 900);
    auto refused = engine->resume("t-refused", json{{"approved", false}, {"by", "bob"}});
    std::cout << "refused: " << refused.channel<std::string>("result") << "\n";

    std::cout << "node ran " << visits->load() << " times for 3 refunds\n";
    return 0;
}
