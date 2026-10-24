// Multi-tenant chatbot server — live OpenRouter DeepSeek API call version.
//
// server.cpp's Mock nodes are replaced with LLMCallNode (NG built-in). The
// same scenario (6 customers × 3 topologies) runs through OpenRouter.
//
// Key checks:
//   1) Whether the NG engine can sustain 100 concurrent live LLM calls
//   2) How memory, wall time, and RPS change compared to the mock version
//   3) Whether the compile cache and thread_id isolation still hold
//
// Cost depends on the OpenRouter model pricing and request usage.
//
// Build and run from the repository root:
//   cmake -S . -B build-cookbook \
//       -DNEOGRAPH_BUILD_EXAMPLES=ON -DNEOGRAPH_BUILD_LLM=ON
//   cmake --build build-cookbook --target cookbook_multi_tenant_live -j4
//   ./build-cookbook/cookbook_multi_tenant_live
//   NG_MULTI_TENANT_REQUESTS=6 NG_MULTI_TENANT_WORKERS=2 \
//       ./build-cookbook/cookbook_multi_tenant_live      # small live check
//
// Defaults: 1,000 requests on 32 workers (about 2,330 model calls). The two
// variables lower them (requests 1..1000, workers 1..256).
// Runtime requires OPENROUTER_API_KEY in the repository-root .env (or environment).
//
// smoke-skip: needs OPENROUTER_API_KEY (live model call)

#include <neograph/neograph.h>
#include "../../provider_example_support.h"
#include <cppdotenv/dotenv.hpp>

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
using clk = std::chrono::steady_clock;

// ── (1) 3 customer topologies — *same shapes* as the mock version, with
// node type "style" replaced by the built-in "llm_call". Demonstrates that
// the same graph structure runs unchanged on both mock and live nodes. ──

static json topology_simple() {
    return {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "simple"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"respond", {{"type", "llm_call"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "respond"}},
            {{"from", "respond"},   {"to", "__end__"}},
        })},
    };
}

static json topology_reflexive() {
    // draft → critique → final. 3 live LLM API calls per request.
    return {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "reflexive"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"draft",    {{"type", "llm_call"}}},
            {"critique", {{"type", "llm_call"}}},
            {"final",    {{"type", "llm_call"}}},
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
    // 3 perspectives in parallel — 3 concurrent live LLM API calls per request.
    // merge stays as in the mock version (deterministic, no LLM needed).
    return {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "fanout"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"perspective_a", {{"type", "llm_call"}}},
            {"perspective_b", {{"type", "llm_call"}}},
            {"perspective_c", {{"type", "llm_call"}}},
            {"merge",         {{"type", "merge"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"},      {"to", "perspective_a"}},
            {{"from", "__start__"},      {"to", "perspective_b"}},
            {{"from", "__start__"},      {"to", "perspective_c"}},
            {{"from", "perspective_a"},  {"to", "merge"}},
            {{"from", "perspective_b"},  {"to", "merge"}},
            {{"from", "perspective_c"},  {"to", "merge"}},
            {{"from", "merge"},          {"to", "__end__"}},
        })},
    };
}

// Merge node — fanout join. Deterministic (does not call the LLM).
class MergeNode : public GraphNode {
    std::string name_;
public:
    explicit MergeNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto msgs = in.state.get("messages");
        std::vector<std::string> recent;
        if (msgs.is_array()) {
            std::vector<std::string> all_assistant;
            for (const auto& m : msgs) {
                if (m.value("role", "") == "assistant")
                    all_assistant.push_back(m.value("content", ""));
            }
            size_t take = std::min<size_t>(3, all_assistant.size());
            for (size_t i = all_assistant.size() - take; i < all_assistant.size(); ++i)
                recent.push_back(all_assistant[i]);
        }
        std::string merged = "[fanout-merge of " + std::to_string(recent.size())
            + " perspectives]";
        NodeOutput out;
        out.writes.push_back({"messages", json::array({
            json{{"role", "assistant"}, {"content", merged}, {"node", name_}}
        })});
        co_return out;
    }
    std::string get_name() const override { return name_; }
};

// Compile cache: exact tenant, topology, and captured provider/model/instructions.
// Shared provider ownership keeps the capability alive while its engine is cached.

class CompileCache {
    std::shared_mutex mu_;
    using Key = std::pair<std::shared_ptr<Provider>, std::string>;
    std::map<Key, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(
        const std::string& customer_id,
        const json& def,
        const NodeContext& ctx)
    {
        Key key{ctx.provider, json::array({customer_id, def, ctx.model,
            ctx.instructions, ctx.extra_config, ctx.provider_name}).dump()};
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        {
            std::unique_lock lk(mu_);
            auto [it, inserted] = cache_.emplace(key, engine);
            if (!inserted) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
    std::size_t hits()   const { return hits_.load(); }
    std::size_t misses() const { return misses_.load(); }
    std::size_t size()   { std::shared_lock lk(mu_); return cache_.size(); }
};

// ── (3) Customer DB ─────────────────────────────────────────────────

struct CustomerRecord {
    std::string id;
    std::string topology_name;
    json        topology_def;
    std::string system_prompt;   // distinct persona per customer
};

static std::unordered_map<std::string, CustomerRecord> make_customer_db() {
    return {
        {"alice",   {"alice",   "simple",    topology_simple(),
                     "You are a concise assistant. Reply in 1 sentence max."}},
        {"bob",     {"bob",     "simple",    topology_simple(),
                     "You are a friendly assistant. Reply in 1 sentence."}},
        {"charlie", {"charlie", "reflexive", topology_reflexive(),
                     "You draft, critique, then finalize. Each step 1 sentence."}},
        {"david",   {"david",   "reflexive", topology_reflexive(),
                     "You draft a response, then revise once. 1 sentence each."}},
        {"eve",     {"eve",     "fanout",    topology_fanout(),
                     "You provide one perspective in 1 sentence."}},
        {"frank",   {"frank",   "fanout",    topology_fanout(),
                     "You provide a different perspective in 1 sentence."}},
    };
}

static long peak_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            long v = 0; std::sscanf(line.c_str(), "VmHWM: %ld kB", &v);
            return v;
        }
    }
    return 0;
}

int main() try {
    cppdotenv::auto_load_dotenv();
    const char* api_key = std::getenv("OPENROUTER_API_KEY");
    if (!api_key) {
        std::cerr << "OPENROUTER_API_KEY 미설정. .env 에 박거나 export 필요.\n";
        return 1;
    }

    // OpenRouter provider — shared across all customers.
    std::shared_ptr<Provider> provider = examples::make_openrouter_provider(api_key, "chat", std::chrono::seconds(180));

    // Register only MergeNode manually (llm_call is built in).
    NodeFactory::instance().register_type("merge",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MergeNode>(name);
        });

    auto customers = make_customer_db();
    CompileCache cache;

    // 1000 concurrent LIVE LLM stress test by default; each request makes 1-3 model calls.
    const int N_REQUESTS = static_cast<int>(
        examples::env_count("NG_MULTI_TENANT_REQUESTS", 1000, 1, 1000));
    const int N_SESSIONS = std::min(200, N_REQUESTS);  // average 5 turns/session
    // I/O bound — more workers to keep more requests in flight concurrently
    const int N_WORKERS  = static_cast<int>(
        examples::env_count("NG_MULTI_TENANT_WORKERS", 32, 1, 256));

    std::vector<std::string> customer_ids = {
        "alice","bob","charlie","david","eve","frank"
    };

    asio::thread_pool pool(N_WORKERS);
    std::atomic<int> ok{0}, errors{0};
    std::atomic<long> latency_sum_ms{0}, latency_max_ms{0};
    std::atomic<int> done{0};
    std::promise<void> all_done;
    auto all_done_fut = all_done.get_future();
    auto reported_usage = std::make_shared<UsageAccumulator>();

    // Sample one response per topology — verify that live LLM responses arrive.
    std::mutex sample_mu;
    std::unordered_map<std::string, std::string> samples;

    std::cout << "Firing " << N_REQUESTS << " concurrent LLM-backed chat "
              << "requests across " << customer_ids.size() << " customers / "
              << N_SESSIONS << " sessions...\n";

    auto t_start = clk::now();
    for (int i = 0; i < N_REQUESTS; ++i) {
        std::string cust = customer_ids[i % customer_ids.size()];
        std::string sess = "session_" + std::to_string(i % N_SESSIONS);

        asio::post(pool, [&, cust, sess, i]() {
            auto t0 = clk::now();
            try {
                const auto& rec = customers[cust];
                NodeContext ctx;
                ctx.provider = provider;
                ctx.model = "~deepseek/deepseek-v4-flash-latest";
                ctx.instructions = rec.system_prompt;
                auto engine = cache.get_or_compile(cust, rec.topology_def, ctx);

                RunConfig rcfg;
                rcfg.thread_id = cust + "__" + sess;
                rcfg.usage = reported_usage;
                rcfg.input = {{"messages", json::array({
                    json{{"role","user"},
                         {"content", "Reply with one word about the topic 'cloud'. Request #" + std::to_string(i)}}
                })}};
                auto result = engine->run(rcfg);

                // Save one sample per topology
                {
                    std::lock_guard lk(sample_mu);
                    if (samples.find(rec.topology_name) == samples.end()) {
                        auto msgs = result.output["channels"]["messages"]["value"];
                        if (msgs.is_array() && !msgs.empty()) {
                            std::string last_content;
                            for (const auto& m : msgs)
                                if (m.value("role","") == "assistant")
                                    last_content = m.value("content", "");
                            samples[rec.topology_name] = last_content;
                        }
                    }
                }
                ok.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                errors.fetch_add(1, std::memory_order_relaxed);
                if (errors.load() <= 3)
                    std::cerr << "err req " << i << " (" << cust << "): " << e.what() << "\n";
            }
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                          clk::now() - t0).count();
            latency_sum_ms.fetch_add(dt, std::memory_order_relaxed);
            long cur = latency_max_ms.load(std::memory_order_relaxed);
            while (dt > cur && !latency_max_ms.compare_exchange_weak(
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
    long mean_ms = ok.load() ? latency_sum_ms.load() / ok.load() : 0;

    std::cout << "\n=== Multi-tenant chatbot LIVE LLM demo ===\n";
    std::cout << "Provider:         OpenRouter "
                 "~deepseek/deepseek-v4-flash-latest (ZDR)\n";
    std::cout << "Requests:         " << N_REQUESTS << " across "
              << customer_ids.size() << " customers / "
              << N_SESSIONS << " sessions\n";
    std::cout << "Topologies:       3 distinct shapes\n";
    std::cout << "Worker pool:      " << N_WORKERS << " threads\n";
    std::cout << "\n--- Results ---\n";
    std::cout << "OK:               " << ok.load() << "\n";
    std::cout << "Errors:           " << errors.load() << "\n";
    std::cout << "Reported usage:   " << usage_to_json(reported_usage->snapshot()).dump() << "\n";
    std::cout << "Monetary charge:  unknown (not inferred from request counts)\n";
    std::cout << "Total wall time:  " << total_ms << " ms ("
              << total_ms / 1000.0 << " sec)\n";
    std::cout << "Mean req latency: " << mean_ms << " ms\n";
    std::cout << "Max req latency:  " << latency_max_ms.load() << " ms\n";
    std::cout << "Throughput:       "
              << (ok.load() * 1000.0 / std::max<long>(total_ms, 1))
              << " req/sec\n";
    std::cout << "Peak RSS:         " << peak_rss_kb() << " KB  ("
              << peak_rss_kb() / 1024.0 << " MB)\n";
    std::cout << "\n--- Compile cache ---\n";
    std::cout << "Distinct engines: " << cache.size() << "  (expected: 6 = customer 수)\n";
    std::cout << "Cache hits:       " << cache.hits() << "\n";
    std::cout << "Cache misses:     " << cache.misses() << "\n";
    std::cout << "Hit rate:         "
              << (100.0 * cache.hits() / std::max<std::size_t>(cache.hits() + cache.misses(), 1))
              << "%\n";

    std::cout << "\n--- Sample responses (1 per topology) ---\n";
    for (auto& [topo, content] : samples) {
        std::string preview = content.substr(0, 120);
        if (content.size() > 120) preview += "...";
        std::cout << "[" << topo << "] " << preview << "\n";
    }

    return errors.load() > 0 ? 1 : 0;
}
catch (const std::exception& error) {
    std::cerr << "cookbook_multi_tenant_live: " << error.what() << "\n";
    return 1;
}
