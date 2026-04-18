// NeoGraph Example 09: All New Features Demo
//
// Demonstrates all 6 new features in a single example:
//
//   1. NodeInterrupt  — dynamic breakpoint (throw from inside a node)
//   2. RetryPolicy    — exponential backoff retry on failure
//   3. StreamMode     — observe internal behavior via EVENTS | DEBUG mode
//   4. Send           — dynamic fan-out (map-reduce pattern)
//   5. Command        — return routing + state modification simultaneously
//   6. Store          — cross-thread shared memory
//
// No API key required (uses custom nodes)
//
// Usage: ./example_all_features

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>

#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>

using namespace neograph;
using namespace neograph::graph;

// =========================================================================
// 1. NodeInterrupt — dynamic interrupt on high-risk amount
// =========================================================================
class PaymentNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto amount_json = state.get("amount");
        int amount = amount_json.is_number() ? amount_json.get<int>() : 0;

        // Dynamic breakpoint if amount >= 1,000,000
        if (amount >= 1000000) {
            throw NodeInterrupt(
                "High-value payment detected: " + std::to_string(amount) + " KRW. Admin approval required.");
        }

        return {ChannelWrite{"result", json("Payment complete: " + std::to_string(amount) + " KRW")}};
    }
    std::string get_name() const override { return "payment"; }
};

// =========================================================================
// 2. RetryPolicy — intermittently failing node (succeeds on 3rd attempt)
// =========================================================================
class UnstableAPINode : public GraphNode {
    static std::atomic<int> call_count_;
public:
    std::vector<ChannelWrite> execute(const GraphState&) override {
        int count = ++call_count_;
        if (count < 3) {
            throw std::runtime_error(
                "API temporary failure (attempt " + std::to_string(count) + ")");
        }
        return {ChannelWrite{"api_result", json("External API response success (attempt " + std::to_string(count) + ")")}};
    }
    std::string get_name() const override { return "unstable_api"; }
};
std::atomic<int> UnstableAPINode::call_count_{0};

// =========================================================================
// 5. Command — route + modify state simultaneously based on score
// =========================================================================
// (Demo showing the Command struct itself. Since the engine is ChannelWrite-based,
//  Command's updates are applied directly, and goto is expressed via the __route__ channel.)
class ScoreRouterNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto score = state.get("score");
        int s = score.is_number() ? score.get<int>() : 0;

        // Command pattern: routing decision + state modification at once
        Command cmd;
        if (s >= 90) {
            cmd.goto_node = "premium";
            cmd.updates = {ChannelWrite{"tier", json("PREMIUM")}};
        } else if (s >= 60) {
            cmd.goto_node = "standard";
            cmd.updates = {ChannelWrite{"tier", json("STANDARD")}};
        } else {
            cmd.goto_node = "basic";
            cmd.updates = {ChannelWrite{"tier", json("BASIC")}};
        }

        // Routing goes via __route__ channel, state is written directly
        std::vector<ChannelWrite> writes;
        writes.push_back(ChannelWrite{"__route__", json(cmd.goto_node)});
        for (auto& u : cmd.updates) writes.push_back(std::move(u));

        return writes;
    }
    std::string get_name() const override { return "score_router"; }
};

// Tier-specific processing node
class TierNode : public GraphNode {
    std::string name_;
    std::string message_;
public:
    TierNode(const std::string& n, const std::string& msg) : name_(n), message_(msg) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto tier = state.get("tier");
        return {ChannelWrite{"result", json(message_ + " (tier: " + tier.get<std::string>() + ")")}};
    }
    std::string get_name() const override { return name_; }
};

// =========================================================================
// Helper: print section separator
// =========================================================================
static void section(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n"
              << " " << title << "\n"
              << std::string(60, '=') << "\n\n";
}

// =========================================================================
// Stream callback — selective output based on StreamMode
// =========================================================================
static void stream_handler(const GraphEvent& event) {
    switch (event.type) {
        case GraphEvent::Type::NODE_START:
            if (event.node_name == "__routing__") {
                // DEBUG mode routing info
                std::cout << "  [debug] routing → " << event.data.dump() << "\n";
            } else {
                std::string extra;
                if (event.data.contains("retry_attempt"))
                    extra = " (retry #" + std::to_string(event.data["retry_attempt"].get<int>()) + ")";
                std::cout << "  [start] " << event.node_name << extra << "\n";
            }
            break;
        case GraphEvent::Type::NODE_END:
            std::cout << "  [done]  " << event.node_name << "\n";
            break;
        case GraphEvent::Type::CHANNEL_WRITE:
            if (event.node_name == "__state__") {
                std::cout << "  [values] full state snapshot emitted\n";
            } else if (event.data.contains("value")) {
                std::cout << "  [update] " << event.node_name << "."
                          << event.data.value("channel", "?") << " = "
                          << event.data["value"].dump().substr(0, 60) << "\n";
            }
            break;
        case GraphEvent::Type::INTERRUPT:
            std::cout << "  [INTERRUPT] " << event.node_name
                      << " — " << event.data.dump() << "\n";
            break;
        case GraphEvent::Type::ERROR:
            std::cout << "  [error] " << event.node_name
                      << " — " << event.data.dump() << "\n";
            break;
        default:
            break;
    }
}

int main() {
    // ================================================================
    // Demo 1: NodeInterrupt (dynamic breakpoint)
    // ================================================================
    section("1. NodeInterrupt — dynamic breakpoint");
    {
        NodeFactory::instance().register_type("payment",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<PaymentNode>();
            });

        json def = {
            {"name", "payment_graph"},
            {"channels", {
                {"amount", {{"reducer", "overwrite"}}},
                {"result", {{"reducer", "overwrite"}}}
            }},
            {"nodes", {{"payment", {{"type", "payment"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "payment"}},
                {{"from", "payment"}, {"to", "__end__"}}
            })}
        };

        NodeContext ctx;
        auto store = std::make_shared<InMemoryCheckpointStore>();
        auto engine = GraphEngine::compile(def, ctx, store);

        // Small payment: passes normally
        RunConfig cfg;
        cfg.thread_id = "pay-001";
        cfg.input = {{"amount", 50000}};
        auto r1 = engine->run_stream(cfg, stream_handler);
        std::cout << "  Result: " << r1.output["channels"]["result"]["value"] << "\n\n";

        // Large payment: NodeInterrupt triggered
        cfg.thread_id = "pay-002";
        cfg.input = {{"amount", 2500000}};
        cfg.stream_mode = StreamMode::EVENTS | StreamMode::DEBUG;
        auto r2 = engine->run_stream(cfg, stream_handler);
        std::cout << "  interrupted: " << std::boolalpha << r2.interrupted << "\n";
        std::cout << "  Reason: " << r2.interrupt_value.value("reason", "") << "\n";

        // Resume after approval
        std::cout << "\n  >>> Admin approved -> resume <<<\n";
        auto r3 = engine->resume("pay-002", json(), stream_handler);
        std::cout << "  Result: " << r3.output["channels"]["result"]["value"] << "\n";
    }

    // ================================================================
    // Demo 2: RetryPolicy (automatic retry)
    // ================================================================
    section("2. RetryPolicy — exponential backoff retry");
    {
        NodeFactory::instance().register_type("unstable_api",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<UnstableAPINode>();
            });

        json def = {
            {"name", "retry_graph"},
            {"channels", {{"api_result", {{"reducer", "overwrite"}}}}},
            {"nodes", {{"unstable_api", {{"type", "unstable_api"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "unstable_api"}},
                {{"from", "unstable_api"}, {"to", "__end__"}}
            })}
        };

        NodeContext ctx;
        auto engine = GraphEngine::compile(def, ctx);

        // Set per-node retry policy
        RetryPolicy retry;
        retry.max_retries = 5;
        retry.initial_delay_ms = 10;   // Short for demo purposes
        retry.backoff_multiplier = 2.0f;
        retry.max_delay_ms = 100;
        engine->set_node_retry_policy("unstable_api", retry);

        RunConfig cfg;
        cfg.stream_mode = StreamMode::EVENTS | StreamMode::DEBUG;
        auto result = engine->run_stream(cfg, stream_handler);
        std::cout << "  Result: " << result.output["channels"]["api_result"]["value"] << "\n";
    }

    // ================================================================
    // Demo 3: StreamMode (selective streaming)
    // ================================================================
    section("3. StreamMode — VALUES + UPDATES mode");
    {
        json def = {
            {"name", "stream_demo"},
            {"channels", {
                {"amount", {{"reducer", "overwrite"}}},
                {"result", {{"reducer", "overwrite"}}}
            }},
            {"nodes", {{"payment", {{"type", "payment"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "payment"}},
                {{"from", "payment"}, {"to", "__end__"}}
            })}
        };

        NodeContext ctx;
        auto engine = GraphEngine::compile(def, ctx);

        RunConfig cfg;
        cfg.input = {{"amount", 30000}};
        // VALUES + UPDATES only (without EVENTS)
        cfg.stream_mode = StreamMode::VALUES | StreamMode::UPDATES;

        std::cout << "  (Streaming VALUES + UPDATES only, without EVENTS)\n";
        auto result = engine->run_stream(cfg,
            [](const GraphEvent& ev) {
                if (ev.type == GraphEvent::Type::NODE_START ||
                    ev.type == GraphEvent::Type::NODE_END) {
                    // EVENTS mode is off, so this should never appear
                    std::cout << "  [!] This should not be visible\n";
                }
                if (ev.type == GraphEvent::Type::CHANNEL_WRITE) {
                    if (ev.node_name == "__state__")
                        std::cout << "  [values] state snapshot received\n";
                    else
                        std::cout << "  [update] " << ev.data.dump().substr(0, 80) << "\n";
                }
            });
        std::cout << "  Result: " << result.output["channels"]["result"]["value"] << "\n";
    }

    // ================================================================
    // Demo 4: Send (dynamic fan-out struct)
    // ================================================================
    section("4. Send — dynamic fan-out struct");
    {
        // Demo of Send struct usage (the struct itself is the data)
        std::vector<Send> sends = {
            {"researcher", {{"topic", "AI"}}},
            {"researcher", {{"topic", "Quantum"}}},
            {"researcher", {{"topic", "Biotech"}}},
        };

        std::cout << "  Created " << sends.size() << " Send requests:\n";
        for (const auto& s : sends) {
            std::cout << "    → " << s.target_node
                      << " (input: " << s.input.dump() << ")\n";
        }
        std::cout << "\n  These Sends are processed by the engine's dynamic fan-out scheduler,\n"
                  << "  which runs the same node in parallel with different inputs.\n";
    }

    // ================================================================
    // Demo 5: Command (routing + state modification)
    // ================================================================
    section("5. Command — routing + simultaneous state modification");
    {
        NodeFactory::instance().register_type("score_router",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<ScoreRouterNode>();
            });
        NodeFactory::instance().register_type("tier_node",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<TierNode>(name, config.value("message", "Processing complete"));
            });

        json def = {
            {"name", "command_graph"},
            {"channels", {
                {"score",     {{"reducer", "overwrite"}}},
                {"tier",      {{"reducer", "overwrite"}}},
                {"result",    {{"reducer", "overwrite"}}},
                {"__route__", {{"reducer", "overwrite"}}}
            }},
            {"nodes", {
                {"score_router", {{"type", "score_router"}}},
                {"premium",  {{"type", "tier_node"}, {"message", "VIP exclusive service provided"}}},
                {"standard", {{"type", "tier_node"}, {"message", "Standard service provided"}}},
                {"basic",    {{"type", "tier_node"}, {"message", "Basic service provided"}}}
            }},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "score_router"}},
                {{"from", "score_router"}, {"condition", "route_channel"},
                 {"routes", {{"premium", "premium"}, {"standard", "standard"}, {"basic", "basic"}}}},
                {{"from", "premium"}, {"to", "__end__"}},
                {{"from", "standard"}, {"to", "__end__"}},
                {{"from", "basic"}, {"to", "__end__"}}
            })}
        };

        NodeContext ctx;

        struct TestCase { int score; std::string expected; };
        std::vector<TestCase> cases = {{95, "premium"}, {72, "standard"}, {40, "basic"}};

        for (const auto& tc : cases) {
            auto engine = GraphEngine::compile(def, ctx);
            RunConfig cfg;
            cfg.input = {{"score", tc.score}};
            cfg.stream_mode = StreamMode::EVENTS | StreamMode::UPDATES;

            std::cout << "  Score " << tc.score << ":\n";
            auto result = engine->run_stream(cfg, stream_handler);
            std::cout << "  → " << result.output["channels"]["result"]["value"]
                      << "  (tier: " << result.output["channels"]["tier"]["value"] << ")\n\n";
        }
    }

    // ================================================================
    // Demo 6: Store (cross-thread shared memory)
    // ================================================================
    section("6. Store — cross-thread shared memory");
    {
        auto store = std::make_shared<InMemoryStore>();

        // Thread A: save user preferences
        store->put({"users", "user123"}, "language", json("ko"));
        store->put({"users", "user123"}, "theme", json("dark"));
        store->put({"users", "user456"}, "language", json("en"));

        // Thread B: retrieve preferences
        auto lang = store->get({"users", "user123"}, "language");
        if (lang) {
            std::cout << "  user123 language: " << lang->value << "\n";
        }

        // Namespace search
        auto user123_prefs = store->search({"users", "user123"});
        std::cout << "  user123 preferences (" << user123_prefs.size() << " items):\n";
        for (const auto& item : user123_prefs) {
            std::cout << "    " << item.key << " = " << item.value << "\n";
        }

        // List all namespaces
        auto namespaces = store->list_namespaces({"users"});
        std::cout << "\n  Namespaces under 'users' (" << namespaces.size() << " items):\n";
        for (const auto& ns : namespaces) {
            std::string path;
            for (size_t i = 0; i < ns.size(); ++i) {
                if (i > 0) path += "/";
                path += ns[i];
            }
            std::cout << "    " << path << "\n";
        }

        // Connect Store to GraphEngine
        json def = {
            {"name", "store_demo"},
            {"channels", {
                {"amount", {{"reducer", "overwrite"}}},
                {"result", {{"reducer", "overwrite"}}}
            }},
            {"nodes", {{"payment", {{"type", "payment"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "payment"}},
                {{"from", "payment"}, {"to", "__end__"}}
            })}
        };
        NodeContext ctx;
        auto engine = GraphEngine::compile(def, ctx);
        engine->set_store(store);

        std::cout << "\n  Engine store connection: "
                  << (engine->get_store() ? "OK" : "FAIL")
                  << " (size=" << store->size() << ")\n";
    }

    // ================================================================
    // Summary
    // ================================================================
    section("Summary");
    std::cout << "  All 6 features demonstrated successfully:\n\n"
              << "  1. NodeInterrupt  — dynamic interrupt on high-value payment + resume\n"
              << "  2. RetryPolicy    — 2 failures then 3rd success (backoff)\n"
              << "  3. StreamMode     — selective streaming: VALUES/UPDATES/EVENTS/DEBUG\n"
              << "  4. Send           — dynamic fan-out request struct\n"
              << "  5. Command        — score-based routing + tier set simultaneously\n"
              << "  6. Store          — user preferences cross-thread shared memory\n"
              << "\n";

    return 0;
}
