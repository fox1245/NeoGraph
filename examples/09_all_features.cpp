// NeoGraph Example 09: a tour of five engine features
//
// Each section is a few lines and names the example that teaches the feature on its own:
//
//   1. NodeInterrupt  a node pauses the run and a person answers   -> 102_node_interrupt.cpp
//   2. RetryPolicy    a flaky node is retried with backoff         -> 101_retry_policy.cpp
//   3. StreamMode     choose which events a streaming run emits    -> 100_stream_modes.cpp
//   4. Command        choose the next node and write state at once -> 10_send_command.cpp
//   5. Store          memory shared across threads                 -> 43_store_personalization.cpp
//
// (Send, the sixth, is the subject of 10_send_command.cpp.) Nodes here are lambdas wrapped in
// one small class, which keeps the tour short.
//
// No API key required.
//
// Usage: ./example_all_features
//
// smoke-expect: Result: "External API response success (attempt 3)"
// smoke-expect: Reason: High-value payment detected
// smoke-expect: (tier: PREMIUM)
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <atomic>
#include <functional>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

using Body = std::function<NodeOutput(NodeInput&)>;

// A node whose body is a lambda.
class LambdaNode : public SyncGraphNode {
public:
    LambdaNode(std::string name, Body body) : SyncGraphNode(std::move(name)), body_(std::move(body)) {}
protected:
    NodeOutput run_sync(NodeInput in) override { return body_(in); }
private:
    Body body_;
};

void register_node(const std::string& type, Body body) {
    NodeFactory::instance().register_type(type,
        [body](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<LambdaNode>(name, body);
        });
}

NodeOutput write(const char* channel, json value) {
    return NodeOutput{{ChannelWrite{channel, std::move(value)}}};
}

// __start__ -> <type> -> __end__, with every channel set to overwrite.
std::unique_ptr<GraphEngine> chain(const std::string& type, Body body,
                                   const std::vector<std::string>& channels, EngineConfig config = {}) {
    register_node(type, std::move(body));
    json channel_defs = json::object();
    for (const auto& name : channels) channel_defs[name] = json{{"reducer", "overwrite"}};
    return GraphEngine::build(json{
        {"schema_version", TOPOLOGY_SCHEMA_VERSION}, {"name", type}, {"channels", channel_defs},
        {"nodes", {{type, {{"type", type}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", type}}, {{"from", type}, {"to", "__end__"}}})}
    }, std::move(config));
}

int main() {
    // 1. NodeInterrupt: a large payment pauses; the admin's answer arrives as resume_value.
    {
        EngineConfig config;
        config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
        auto engine = chain("payment", [](NodeInput& in) {
            const int amount = in.state.get("amount").get<int>();
            if (amount >= 1000000 && !in.ctx.resume_value)
                throw NodeInterrupt("High-value payment detected: " + std::to_string(amount) +
                                    " KRW. Admin approval required.", json{{"amount", amount}});
            return write("result", json("Payment complete: " + std::to_string(amount) + " KRW"));
        }, {"amount", "result"}, std::move(config));
        RunConfig run;
        run.thread_id = "pay-1";
        run.input     = {{"amount", 2500000}};
        auto paused   = engine->run(run);
        std::cout << "1. paused at " << paused.interrupt_node << "\n   Reason: "
                  << paused.interrupt_value.value("reason", "") << "\n";
        std::cout << "   resumed: " << engine->resume("pay-1", json{{"approved", true}}).channel_raw("result").dump() << "\n";
    }

    // 2. RetryPolicy: fails twice, the third attempt works.
    {
        auto calls = std::make_shared<std::atomic<int>>(0);
        RetryPolicy retry;
        retry.max_retries      = 5;
        retry.initial_delay_ms = 10;
        EngineConfig config;
        config.node_retry_policies["unstable_api"] = retry;
        auto engine = chain("unstable_api", [calls](NodeInput&) {
            const int attempt = ++*calls;
            if (attempt < 3) throw std::runtime_error("API temporary failure");
            return write("api_result", json("External API response success (attempt " + std::to_string(attempt) + ")"));
        }, {"api_result"}, std::move(config));
        std::cout << "2. Result: " << engine->run(RunConfig{}).channel_raw("api_result").dump() << "\n";
    }

    // 3. StreamMode: VALUES | UPDATES delivers state snapshots and writes, but no node events.
    {
        auto engine = chain("greet", [](NodeInput&) { return write("greeting", json("hello")); }, {"greeting"});
        RunConfig run;
        run.stream_mode = StreamMode::VALUES | StreamMode::UPDATES;
        int events = 0;
        engine->run_stream(run, [&](const GraphEvent&) { ++events; });
        std::cout << "3. VALUES|UPDATES delivered " << events << " events\n";
    }

    // 4. Command: the router picks the next node and writes `tier` in one return value. The
    //    two edges list where it may go; the Command it returns chooses one.
    {
        register_node("router", [](NodeInput& in) {
            const bool premium = in.state.get("score").get<int>() >= 90;
            NodeOutput out;
            out.command = Command{premium ? "premium" : "basic", {ChannelWrite{"tier", json(premium ? "PREMIUM" : "BASIC")}}};
            return out;
        });
        for (const char* name : {"premium", "basic"})
            register_node(name, [](NodeInput& in) {
                return write("result", json("service provided (tier: " + in.state.get("tier").get<std::string>() + ")"));
            });
        auto engine = GraphEngine::build(json{
            {"schema_version", TOPOLOGY_SCHEMA_VERSION}, {"name", "command"},
            {"channels", {{"score", {{"reducer", "overwrite"}}}, {"tier", {{"reducer", "overwrite"}}},
                          {"result", {{"reducer", "overwrite"}}}}},
            {"nodes", {{"router", {{"type", "router"}}}, {"premium", {{"type", "premium"}}}, {"basic", {{"type", "basic"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "router"}},
                {{"from", "router"}, {"to", "premium"}}, {{"from", "router"}, {"to", "basic"}},
                {{"from", "premium"}, {"to", "__end__"}}, {{"from", "basic"}, {"to", "__end__"}}})}
        }, EngineConfig{});
        for (int score : {95, 40}) {
            RunConfig run;
            run.input = {{"score", score}};
            std::cout << "4. score " << score << ": " << engine->run(run).channel<std::string>("result") << "\n";
        }
    }

    // 5. Store: memory that outlives one thread's checkpoints. Example 43 reads it inside nodes.
    {
        auto store = std::make_shared<InMemoryStore>();
        store->put({"users", "user123"}, "language", json("ko"));
        store->put({"users", "user123"}, "theme", json("dark"));
        store->put({"users", "user456"}, "language", json("en"));
        std::cout << "5. user123 language: " << store->get({"users", "user123"}, "language")->value
                  << ", " << store->search({"users", "user123"}).size() << " items, "
                  << store->list_namespaces({"users"}).size() << " users\n";
    }
    return 0;
}
