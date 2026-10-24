// Multi-tenant chatbot server demo — one process concurrently serves N
// different graph topologies for N customers.
//
// Key points demonstrated:
//   1) Compile cache (graph_def hash → shared_ptr<GraphEngine>) —
//      customers with the same topology share an engine instance.
//   2) Per-customer topology — one backend serves ReAct-shaped,
//      reflexive-shaped, and fanout-shaped topologies concurrently.
//   3) Per-session isolation — thread_id isolation + history accumulation.
//   4) Memory and latency measurement — 1000 concurrent requests.
//
// Build and run from the repository root:
//   cmake -S . -B build-cookbook \
//       -DNEOGRAPH_BUILD_EXAMPLES=ON -DNEOGRAPH_BUILD_LLM=ON
//   cmake --build build-cookbook --target cookbook_multi_tenant_mock -j4
//   ./build-cookbook/cookbook_multi_tenant_mock
//
// smoke-expect: OK:               1000
// smoke-expect: Errors:           0
// smoke-expect: Distinct engines: 3  (expected: 3)

#include <neograph/neograph.h>

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
using clk = std::chrono::steady_clock;

// ── (1) Mock-LLM nodes — deterministic replies without Provider calls ─
//
// A real multi-tenant server would use LLMCallNode + typed SchemaProvider,
// but this demo has zero external dependencies to isolate topology differences.
// Replies are deterministic from the prompt — same input → same output (verifiable).

class StyleNode : public GraphNode {
    std::string style_, name_;
public:
    StyleNode(std::string style, std::string name)
        : style_(std::move(style)), name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto msgs = in.state.get("messages");
        std::string last_user;
        if (msgs.is_array()) {
            // neograph::json (yyjson) has no reverse iterator — track the
            // last user message via forward iteration.
            for (const auto& m : msgs) {
                if (m.value("role", "") == "user")
                    last_user = m.value("content", "");
            }
        }
        std::string reply_text;
        if (style_ == "concise")  reply_text = "ack(" + std::to_string(last_user.size()) + ")";
        else if (style_ == "verbose") reply_text = "long response to: " + last_user.substr(0, 20) + "...";
        else if (style_ == "formal")  reply_text = "Dear customer, regarding '" + last_user.substr(0, 15) + "', noted.";
        else if (style_ == "critique") reply_text = "[critique] last reply could be tighter";
        else reply_text = "[" + style_ + "] " + last_user;

        auto reply = json{{"role", "assistant"}, {"content", reply_text}, {"node", name_}};
        NodeOutput out;
        out.writes.push_back({"messages", json::array({reply})});
        co_return out;
    }
    std::string get_name() const override { return name_; }
};

class MergeNode : public GraphNode {
    std::string name_;
public:
    explicit MergeNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        // Join of the fanout topology — merge the last N assistant messages
        // into one message to present multiple perspectives to the user.
        auto msgs = in.state.get("messages");
        std::vector<std::string> recent;
        if (msgs.is_array()) {
            // Collect in forward order and take the last 3 (no reverse iterator).
            std::vector<std::string> all_assistant;
            for (const auto& m : msgs) {
                if (m.value("role", "") == "assistant")
                    all_assistant.push_back(m.value("content", ""));
            }
            size_t take = std::min<size_t>(3, all_assistant.size());
            for (size_t i = all_assistant.size() - take; i < all_assistant.size(); ++i)
                recent.push_back(all_assistant[i]);
        }
        std::string merged = "[fanout-merge] ";
        for (auto& s : recent) merged += "(" + s + ") ";
        NodeOutput out;
        out.writes.push_back({"messages", json::array({
            json{{"role", "assistant"}, {"content", merged}, {"node", name_}}
        })});
        co_return out;
    }
    std::string get_name() const override { return name_; }
};

// ── (2) 3 customer topology JSONs — genuinely different shapes ───────

static json topology_simple() {
    return {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "simple"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"respond", {{"type", "style"}, {"style", "concise"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "respond"}},
            {{"from", "respond"},   {"to", "__end__"}},
        })},
    };
}

static json topology_reflexive() {
    // response → self-check → revised response
    return {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "reflexive"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"draft",    {{"type", "style"}, {"style", "verbose"}}},
            {"critique", {{"type", "style"}, {"style", "critique"}}},
            {"final",    {{"type", "style"}, {"style", "formal"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "draft"}},
            {{"from", "draft"},     {"to", "critique"}},
            {{"from", "critique"},  {"to", "final"}},
            {{"from", "final"},     {"to", "__end__"}},
        })},
    };
}

static json topology_fanout() {
    // 3 perspectives in parallel → merge
    return {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "fanout"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"concise", {{"type", "style"}, {"style", "concise"}}},
            {"verbose", {{"type", "style"}, {"style", "verbose"}}},
            {"formal",  {{"type", "style"}, {"style", "formal"}}},
            {"merge",   {{"type", "merge"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "concise"}},
            {{"from", "__start__"}, {"to", "verbose"}},
            {{"from", "__start__"}, {"to", "formal"}},
            {{"from", "concise"},   {"to", "merge"}},
            {{"from", "verbose"},   {"to", "merge"}},
            {{"from", "formal"},    {"to", "merge"}},
            {{"from", "merge"},     {"to", "__end__"}},
        })},
    };
}

// ── (3) Compile cache — graph_def hash → shared_ptr<GraphEngine> ─────
//
// Core of a multi-tenant server. Customers using the same topology share
// an engine, so memory scales with the number of *distinct topologies*
// rather than the number of customers.

class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        // This provider-free style demo still binds all captured configuration.
        const std::string key = json::array({def, ctx.model, ctx.instructions,
            ctx.extra_config, ctx.provider_name}).dump();
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        // Miss — compile outside the lock so other customers are not blocked.
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        {
            std::unique_lock lk(mu_);
            auto [it, inserted] = cache_.emplace(key, engine);
            if (!inserted) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;  // race — another thread inserted first
            }
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
    std::size_t hits()   const { return hits_.load(); }
    std::size_t misses() const { return misses_.load(); }
    std::size_t size()   { std::shared_lock lk(mu_); return cache_.size(); }
};

// ── (4) Simulated customer DB ────────────────────────────────────────
//
// A real server would look up rows in a Postgres `customer_graphs` table.
// This demo uses an in-memory map mapping each customer_id to a topology.

struct CustomerRecord {
    std::string id;
    std::string topology_name;
    json        topology_def;
};

static std::unordered_map<std::string, CustomerRecord> make_customer_db() {
    std::unordered_map<std::string, CustomerRecord> db;
    // 6 customers, 3 distinct topologies
    db["alice"]   = {"alice",   "simple",    topology_simple()};
    db["bob"]     = {"bob",     "simple",    topology_simple()};      // same topology as alice
    db["charlie"] = {"charlie", "reflexive", topology_reflexive()};
    db["david"]   = {"david",   "reflexive", topology_reflexive()};   // same as charlie
    db["eve"]     = {"eve",     "fanout",    topology_fanout()};
    db["frank"]   = {"frank",   "fanout",    topology_fanout()};
    return db;
}

// ── (5) RSS measurement ──────────────────────────────────────────────
static long peak_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            long v = 0;
            std::sscanf(line.c_str(), "VmHWM: %ld kB", &v);
            return v;
        }
    }
    return 0;
}

// ── (6) main — simulate 1000 concurrent requests ─────────────────────
int main() {
    // Register node types once at server startup.
    NodeFactory::instance().register_type("style",
        [](const std::string& name, const json& def, const NodeContext&) {
            std::string style = def.value("style", "default");
            return std::make_unique<StyleNode>(style, name);
        });
    NodeFactory::instance().register_type("merge",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MergeNode>(name);
        });

    auto customers = make_customer_db();
    CompileCache cache;
    NodeContext ctx;

    const int N_REQUESTS    = 1000;
    const int N_SESSIONS    = 100;   // average 10 turns / session
    const int N_WORKERS     = 8;

    asio::thread_pool pool(N_WORKERS);
    std::atomic<int> ok{0}, errors{0};
    std::atomic<long> latency_sum_us{0}, latency_max_us{0};

    // Simulation: each request is a (customer, session, message) tuple.
    std::vector<std::string> customer_ids = {"alice","bob","charlie","david","eve","frank"};

    auto t_start = clk::now();
    std::atomic<int> done{0};
    std::promise<void> all_done;
    auto all_done_fut = all_done.get_future();

    for (int i = 0; i < N_REQUESTS; ++i) {
        std::string cust = customer_ids[i % customer_ids.size()];
        std::string sess = "session_" + std::to_string(i % N_SESSIONS);
        std::string msg  = "Hello from " + cust + " req " + std::to_string(i);

        asio::post(pool, [&, cust, sess, msg]() {
            auto t0 = clk::now();
            try {
                const auto& rec = customers[cust];
                auto engine = cache.get_or_compile(rec.topology_def, ctx);
                RunConfig cfg;
                cfg.thread_id = cust + "__" + sess;   // session isolation key
                cfg.input = {{"messages", json::array({
                    json{{"role", "user"}, {"content", msg}}
                })}};
                (void)engine->run(cfg);
                ok.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                errors.fetch_add(1, std::memory_order_relaxed);
                if (errors.load() < 3)
                    std::cerr << "err: " << e.what() << "\n";
            }
            auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                          clk::now() - t0).count();
            latency_sum_us.fetch_add(dt, std::memory_order_relaxed);
            long cur = latency_max_us.load(std::memory_order_relaxed);
            while (dt > cur && !latency_max_us.compare_exchange_weak(
                cur, dt, std::memory_order_relaxed)) {}
            if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == N_REQUESTS)
                all_done.set_value();
        });
    }
    all_done_fut.wait();
    auto t_end = clk::now();
    pool.stop();
    pool.join();

    long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();
    long mean_us = ok.load() ? latency_sum_us.load() / ok.load() : 0;

    std::cout << "\n=== Multi-tenant chatbot demo ===\n";
    std::cout << "Requests:        " << N_REQUESTS << " across "
              << customer_ids.size() << " customers / "
              << N_SESSIONS << " sessions\n";
    std::cout << "Topologies in DB: 3 distinct shapes (simple / reflexive / fanout)\n";
    std::cout << "Customers in DB:  " << customers.size()
              << " (2 each topology)\n";
    std::cout << "\n--- Results ---\n";
    std::cout << "OK:               " << ok.load() << "\n";
    std::cout << "Errors:           " << errors.load() << "\n";
    std::cout << "Total wall time:  " << total_ms << " ms\n";
    std::cout << "Mean latency:     " << mean_us << " us\n";
    std::cout << "Max latency:      " << latency_max_us.load() << " us\n";
    std::cout << "Peak RSS:         " << peak_rss_kb() << " KB  ("
              << peak_rss_kb() / 1024.0 << " MB)\n";
    std::cout << "\n--- Compile cache ---\n";
    std::cout << "Distinct engines: " << cache.size() << "  (expected: 3)\n";
    std::cout << "Cache hits:       " << cache.hits() << "\n";
    std::cout << "Cache misses:     " << cache.misses() << "  (expected: 3)\n";
    std::cout << "Hit rate:         "
              << (100.0 * cache.hits() / (cache.hits() + cache.misses()))
              << "%\n";

    // ── (7) Hot-swap demo — change alice's topology from simple to fanout ──
    std::cout << "\n--- Hot-swap demo ---\n";
    std::cout << "alice 의 토폴로지를 'simple' → 'fanout' 으로 변경 (deploy/restart 없이)\n";
    customers["alice"].topology_def = topology_fanout();
    customers["alice"].topology_name = "fanout";

    // In the same process, alice's next request runs on the new topology.
    auto engine_new = cache.get_or_compile(customers["alice"].topology_def, ctx);
    RunConfig cfg;
    cfg.thread_id = "alice__hotswap_test";
    cfg.input = {{"messages", json::array({
        json{{"role", "user"}, {"content", "hello after hot-swap"}}
    })}};
    auto result = engine_new->run(cfg);
    std::cout << "Alice 새 토폴로지로 응답 받음 (output channels count: "
              << result.output["channels"].size() << ")\n";
    std::cout << "Cache distinct engines now: " << cache.size()
              << "  (hot-swap 후 새 engine 1개 추가됐을 수도, "
              << "fanout 은 이미 있어서 reuse)\n";

    return errors.load() > 0 ? 1 : 0;
}
