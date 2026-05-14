// Multi-tenant chatbot server — 진짜 OpenAI API 호출 버전.
//
// server.cpp 의 Mock 노드를 LLMCallNode (NG built-in) 로 교체. 같은
// 시나리오 (6 customer × 3 topology) 를 진짜 gpt-4o-mini 로 처리.
//
// 핵심 검증:
//   1) NG 엔진이 100 동시 실제 LLM 호출을 들고 있을 수 있나
//   2) 메모리 / wall time / RPS 가 어떻게 변하나 (Mock 대비)
//   3) compile cache + thread_id 격리는 그대로 유효한가
//
// 비용 추정: gpt-4o-mini × 100 요청 × 평균 2.3 LLM call (topology 평균)
//          ≈ 230 call × ~$0.0002/call ≈ $0.05.
//
// 빌드:
//   g++ -std=c++20 -O2 -DNDEBUG \
//       -Iinclude -Ideps -Ideps/yyjson -Ideps/asio/include \
//       -DASIO_STANDALONE \
//       projects/multi_tenant_chatbot/server_live_llm.cpp \
//       -Lbuild -lneograph_core -lneograph_async -lneograph_llm \
//       -lcppdotenv -lyyjson -lssl -lcrypto \
//       -Wl,-rpath,'$ORIGIN/build' \
//       -pthread -o /tmp/multi_tenant_live
//
// 실행 (repo root 에 .env 가 OPENAI_API_KEY 박혀있어야):
//   LD_LIBRARY_PATH=build /tmp/multi_tenant_live

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <cppdotenv/dotenv.hpp>

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
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
using clk = std::chrono::steady_clock;

// ── (1) 3 customer topology — Mock 버전과 *shape 동일*, 노드 타입만
// "style" 대신 NG built-in "llm_call" 로 교체. 같은 graph 구조가
// Mock/Real 양쪽 다 동작한다는 게 시연 포인트. ──────────────────────

static json topology_simple() {
    return {
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
    // draft → critique → final. 진짜 LLM 이라 3 API call/요청.
    return {
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
    // 3 perspective 병렬 — 진짜 LLM 이라 3 동시 API call/요청.
    // merge 는 mock 그대로 (deterministic, LLM 불필요).
    return {
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

// Merge 노드 — fanout 의 join. 결정적 (LLM 안 부름).
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

// ── (2) Compile cache — key = (topology_hash, customer_id).
//
// Customer 별 system prompt 가 NodeContext 에 박혀있어서, 같은
// 토폴로지여도 customer 가 다르면 engine 인스턴스가 별도. 그래도
// 같은 customer 의 여러 요청은 engine 1개 공유.

class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(
        const std::string& customer_id,
        const json& def,
        const NodeContext& ctx)
    {
        std::string key = customer_id + "::"
            + std::to_string(std::hash<std::string>{}(def.dump()));
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        auto raw = GraphEngine::compile(def, ctx);
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
    std::string system_prompt;   // customer 별로 다른 페르소나
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

int main() {
    cppdotenv::auto_load_dotenv();
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "OPENAI_API_KEY 미설정. .env 에 박거나 export 필요.\n";
        return 1;
    }

    // OpenAI provider — 모든 customer 가 공유. 진짜 multi-tenant 라면
    // customer 마다 다른 provider/모델 가능 (alice=gpt-4o-mini,
    // bob=claude-haiku 등). 데모는 단순화.
    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key = api_key;
    cfg.default_model = "gpt-4o-mini";
    auto provider = neograph::llm::OpenAIProvider::create_shared(cfg);

    // MergeNode 만 직접 등록 (llm_call 은 built-in).
    NodeFactory::instance().register_type("merge",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MergeNode>(name);
        });

    auto customers = make_customer_db();
    CompileCache cache;

    const int N_REQUESTS = 1000;    // 1000 동시 LIVE LLM stress test
    const int N_SESSIONS = 200;     // 평균 5 turn/session
    const int N_WORKERS  = 32;      // I/O bound — 동시 in-flight 늘리려 worker 증가

    std::vector<std::string> customer_ids = {
        "alice","bob","charlie","david","eve","frank"
    };

    asio::thread_pool pool(N_WORKERS);
    std::atomic<int> ok{0}, errors{0};
    std::atomic<long> latency_sum_ms{0}, latency_max_ms{0};
    std::atomic<int> done{0};
    std::promise<void> all_done;
    auto all_done_fut = all_done.get_future();

    // Sample one response per topology — 진짜 LLM 응답 받아오는지 확인.
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
                ctx.model = "gpt-4o-mini";
                ctx.instructions = rec.system_prompt;
                auto engine = cache.get_or_compile(cust, rec.topology_def, ctx);

                RunConfig rcfg;
                rcfg.thread_id = cust + "__" + sess;
                rcfg.input = {{"messages", json::array({
                    json{{"role","user"},
                         {"content", "Reply with one word about the topic 'cloud'. Request #" + std::to_string(i)}}
                })}};
                auto result = engine->run(rcfg);

                // Sample 저장 — topology 마다 1개씩
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
    std::cout << "Provider:         OpenAI gpt-4o-mini\n";
    std::cout << "Requests:         " << N_REQUESTS << " across "
              << customer_ids.size() << " customers / "
              << N_SESSIONS << " sessions\n";
    std::cout << "Topologies:       3 distinct shapes\n";
    std::cout << "Worker pool:      " << N_WORKERS << " threads\n";
    std::cout << "\n--- Results ---\n";
    std::cout << "OK:               " << ok.load() << "\n";
    std::cout << "Errors:           " << errors.load() << "\n";
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
