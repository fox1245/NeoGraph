// NeoGraph Example 43: Cross-thread Store driving per-user node behaviour
//
// The README touts the cross-thread `Store` (LangGraph's equivalent) for
// "long-term user preferences, shared knowledge, agent memory" — but
// example 09 only shows the Store API in isolation. The realistic use
// case is a node that READS the Store to personalise its output for the
// current user, and another node that WRITES learned facts back so they
// survive across sessions and threads.
//
// ## Two ways to reach the Store from a node
//
// 1. **`in.ctx.store`** (recommended, available since issue #27).
//    `RunContext::store` mirrors `GraphEngine::get_store()` so any
//    node body can read/write through `in.ctx.store->get(...)` /
//    `->put(...)` directly. No factory-closure plumbing needed.
//
// 2. **Capture `shared_ptr<Store>` in the `NodeFactory` closure**
//    (what this example shows). Pre-#27 this was the only working
//    shape; it still works and is left in place during the
//    deprecation window so older examples / downstream code keeps
//    compiling unchanged.
//
// New code should prefer (1) — fewer captures, less boilerplate, and
// the engine guarantees `in.ctx.store` is the same Store instance
// `set_store(...)` was called with. This example sticks with (2) so
// the contrast with example 09 (Store-in-main-only) stays direct.
//
// No API key required.
//
// Usage: ./example_store_personalization

#include <neograph/neograph.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/state.h>
#include <neograph/graph/store.h>

#include <iostream>
#include <memory>

using namespace neograph;
using namespace neograph::graph;

// ── Greet node: reads `user_id` from state, looks up `preferred_name`
//                and `language` from the Store, emits a personalised
//                greeting on the `reply` channel.
//
// The Store is captured in the closure — there is no
// NodeContext::store. If you forget to set_store() on the engine and
// also forget to capture it, the node silently degrades to defaults.
class GreetNode : public GraphNode {
    std::shared_ptr<Store> store_;
public:
    explicit GreetNode(std::shared_ptr<Store> s) : store_(std::move(s)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string user_id = in.state.get("user_id").is_string()
                                  ? in.state.get("user_id").get<std::string>()
                                  : "anonymous";

        std::string preferred_name = "stranger";
        std::string language       = "en";

        if (store_) {
            auto name = store_->get({"users", user_id}, "preferred_name");
            if (name) preferred_name = name->value.get<std::string>();

            auto lang = store_->get({"users", user_id}, "language");
            if (lang) language = lang->value.get<std::string>();
        }

        std::string greeting;
        if      (language == "ko") greeting = "안녕하세요, " + preferred_name + "!";
        else if (language == "es") greeting = "¡Hola, " + preferred_name + "!";
        else                       greeting = "Hello, "  + preferred_name + "!";

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"reply", json(greeting)});
        co_return out;
    }
    std::string get_name() const override { return "greet"; }
};

// ── Learn node: at the end of each session, write the `learned_fact`
//                channel (if any) into the Store, namespaced under
//                this user. Future sessions on any thread_id can read
//                it back via Store::search.
class LearnNode : public GraphNode {
    std::shared_ptr<Store> store_;
public:
    explicit LearnNode(std::shared_ptr<Store> s) : store_(std::move(s)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string user_id = in.state.get("user_id").is_string()
                                  ? in.state.get("user_id").get<std::string>()
                                  : "anonymous";

        auto fact = in.state.get("learned_fact");
        if (store_ && fact.is_string() && !fact.get<std::string>().empty()) {
            // Append-on-write: store as a list keyed by a counter so
            // we accumulate across sessions without overwriting.
            auto existing = store_->search({"users", user_id, "facts"});
            std::string key = "fact_" + std::to_string(existing.size() + 1);
            store_->put({"users", user_id, "facts"}, key, fact);
        }

        NodeOutput out;
        co_return out;
    }
    std::string get_name() const override { return "learn"; }
};

static json make_graph() {
    return json{
        {"name", "personalized"},
        {"channels", {
            {"user_id",      {{"reducer", "overwrite"}}},
            {"learned_fact", {{"reducer", "overwrite"}}},
            {"reply",        {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"greet", {{"type", "greet"}}},
            {"learn", {{"type", "learn"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "greet"}},
            {{"from", "greet"},     {"to", "learn"}},
            {{"from", "learn"},     {"to", "__end__"}},
        })},
    };
}

int main() {
    // The Store is shared between the engine (for inspection) AND the
    // node factories (so the running nodes can read/write).
    auto store = std::make_shared<InMemoryStore>();

    // Seed initial preferences for user alice — typically these would
    // come from a database or user profile service.
    store->put({"users", "alice"}, "preferred_name", json("Alice"));
    store->put({"users", "alice"}, "language",       json("ko"));

    store->put({"users", "bob"},   "preferred_name", json("Bobby"));
    store->put({"users", "bob"},   "language",       json("es"));
    // carol intentionally absent — exercise the defaults path.

    // Register node factories that capture the Store by shared_ptr.
    NodeFactory::instance().register_type("greet",
        [store](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<GreetNode>(store);
        });
    NodeFactory::instance().register_type("learn",
        [store](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<LearnNode>(store);
        });

    NodeContext ctx;
    auto engine = GraphEngine::compile(make_graph(), ctx);
    engine->set_store(store);  // Engine also holds it for get_store().

    // ── Three different user sessions on three different thread_ids ──
    struct Session {
        std::string user_id;
        std::string fact_to_learn;
    };
    std::vector<Session> sessions = {
        {"alice", "Alice prefers tea over coffee."},
        {"bob",   "Bob's favorite team is Real Madrid."},
        {"carol", ""},                       // no fact to learn this time
    };

    for (const auto& s : sessions) {
        RunConfig cfg;
        cfg.thread_id = "session-" + s.user_id;
        cfg.input = {
            {"user_id",      s.user_id},
            {"learned_fact", s.fact_to_learn},
        };
        auto r = engine->run(cfg);
        std::cout << "[" << s.user_id << "] "
                  << r.output["channels"]["reply"]["value"].get<std::string>()
                  << "\n";
    }

    // ── Inspect the Store after all sessions: facts learned per user ──
    std::cout << "\n── Store contents after all sessions ──────────\n";
    auto all_user_namespaces = store->list_namespaces({"users"});
    for (const auto& ns : all_user_namespaces) {
        // Drill into each user's `facts` namespace.
        if (ns.size() >= 2 && ns[0] == "users") {
            // skip the bare {"users","x"} prefs entries — print only fact paths
            if (ns.size() == 3 && ns[2] == "facts") {
                auto facts = store->search(ns);
                std::cout << "users/" << ns[1] << "/facts ("
                          << facts.size() << " items):\n";
                for (const auto& it : facts) {
                    std::cout << "  " << it.key << ": " << it.value << "\n";
                }
            }
        }
    }

    // ── Assertion: carol got the default "Hello, stranger!" path ─────
    // We re-run carol and check the reply contains "stranger".
    {
        RunConfig cfg;
        cfg.thread_id = "session-carol-check";
        cfg.input = {{"user_id", "carol"}, {"learned_fact", ""}};
        auto r = engine->run(cfg);
        std::string reply = r.output["channels"]["reply"]["value"].get<std::string>();
        bool defaulted = reply.find("stranger") != std::string::npos;
        std::cout << "\nUnknown-user fallback: "
                  << std::boolalpha << defaulted << " (\"" << reply << "\")\n";
        if (!defaulted) {
            std::cerr << "FAIL: expected default greeting for unknown user.\n";
            return 1;
        }
    }

    return 0;
}
