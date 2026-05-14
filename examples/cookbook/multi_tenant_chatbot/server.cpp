// Multi-tenant chatbot server demo — 한 process 가 N customer 의
// N 다른 graph topology 를 동시 서빙.
//
// 핵심 시연:
//   1) Compile cache (graph_def hash → shared_ptr<GraphEngine>) —
//      같은 토폴로지를 쓰는 customer 끼리 engine 인스턴스 공유.
//   2) Per-customer topology — 같은 backend 가 ReAct-shape /
//      reflexive-shape / fanout-shape 셋을 동시 처리.
//   3) Per-session isolation — thread_id 격리 + history 누적.
//   4) 메모리 / 시간 측정 — 1000 동시 요청.
//
// 빌드 (repo root 에서):
//   g++ -std=c++20 -O2 -DNDEBUG \
//       -Iinclude -Ideps -Ideps/yyjson -Ideps/asio/include \
//       -DASIO_STANDALONE \
//       projects/multi_tenant_chatbot/server.cpp \
//       -Lbuild -lneograph_core -lyyjson \
//       -Wl,-rpath,'$ORIGIN/../../build' \
//       -pthread -o /tmp/multi_tenant_server
//
// 실행:
//   LD_LIBRARY_PATH=build /tmp/multi_tenant_server

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

// ── (1) Mock-LLM nodes — 실제 Provider 호출 없이 결정적 응답 ─────────
//
// 진짜 multi-tenant 서버라면 LLMCallNode + 진짜 OpenAIProvider/Anthropic
// 를 쓰겠지만, 데모는 외부 의존성 0 으로 토폴로지 차이만 보여줌.
// 응답은 prompt 내용 기반 결정적 — 같은 입력 → 같은 출력 (검증 가능).

class StyleNode : public GraphNode {
    std::string style_, name_;
public:
    StyleNode(std::string style, std::string name)
        : style_(std::move(style)), name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto msgs = in.state.get("messages");
        std::string last_user;
        if (msgs.is_array()) {
            // neograph::json (yyjson) 은 reverse iterator 없음 — 순방향
            // iteration 으로 마지막 user 메시지 추적.
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
        // fanout 토폴로지의 join — 마지막 N개의 assistant 메시지를
        // 한 메시지로 합쳐 사용자에게 다양한 관점 제공.
        auto msgs = in.state.get("messages");
        std::vector<std::string> recent;
        if (msgs.is_array()) {
            // 순방향으로 모은 후 마지막 3개를 추출 (reverse iter 없음).
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

// ── (2) 3 customer topology JSON — shape 이 진짜로 다름 ─────────────

static json topology_simple() {
    return {
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
    // 응답 → 자기 점검 → 재응답
    return {
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
    // 3 perspective 병렬 → merge
    return {
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
// 진짜 multi-tenant 서버의 핵심. 같은 토폴로지 쓰는 customer 끼리
// engine 공유. 메모리 사용량이 customer 수가 아니라 *distinct topology
// 수* 에 비례하게 만듦.

class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<size_t, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        // json::dump() 으로 정규화 후 std::hash. 진짜 production 이면
        // SHA-256 같은 충돌 안 나는 hash 권장 — 데모는 std::hash 로 충분.
        size_t key = std::hash<std::string>{}(def.dump());
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        // Miss — compile (lock 밖에서, 다른 customer 차단 안 함).
        auto raw = GraphEngine::compile(def, ctx);
        std::shared_ptr<GraphEngine> engine(raw.release());
        {
            std::unique_lock lk(mu_);
            auto [it, inserted] = cache_.emplace(key, engine);
            if (!inserted) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;  // race — 다른 thread 가 먼저 넣음
            }
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
    std::size_t hits()   const { return hits_.load(); }
    std::size_t misses() const { return misses_.load(); }
    std::size_t size()   { std::shared_lock lk(mu_); return cache_.size(); }
};

// ── (4) Customer DB 시뮬레이션 ──────────────────────────────────────
//
// 진짜 서버라면 Postgres `customer_graphs` table 한 row 씩 lookup.
// 데모는 in-memory map. 핵심은 customer_id 마다 "어떤 토폴로지" 가 매핑.

struct CustomerRecord {
    std::string id;
    std::string topology_name;
    json        topology_def;
};

static std::unordered_map<std::string, CustomerRecord> make_customer_db() {
    std::unordered_map<std::string, CustomerRecord> db;
    // 6 customer, 3 distinct topology
    db["alice"]   = {"alice",   "simple",    topology_simple()};
    db["bob"]     = {"bob",     "simple",    topology_simple()};      // alice 와 동일 토폴로지
    db["charlie"] = {"charlie", "reflexive", topology_reflexive()};
    db["david"]   = {"david",   "reflexive", topology_reflexive()};   // charlie 와 동일
    db["eve"]     = {"eve",     "fanout",    topology_fanout()};
    db["frank"]   = {"frank",   "fanout",    topology_fanout()};
    return db;
}

// ── (5) RSS 측정 ────────────────────────────────────────────────────
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

// ── (6) main — 1000 동시 요청 시뮬레이션 ────────────────────────────
int main() {
    // 노드 type 등록 (서버 시작 시 1회).
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
    const int N_SESSIONS    = 100;   // 평균 10 turn / session
    const int N_WORKERS     = 8;

    asio::thread_pool pool(N_WORKERS);
    std::atomic<int> ok{0}, errors{0};
    std::atomic<long> latency_sum_us{0}, latency_max_us{0};

    // 시뮬레이션: 각 요청은 (customer, session, message) 셋.
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
                cfg.thread_id = cust + "__" + sess;   // session 격리 키
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

    // ── (7) Hot-swap 시연 — alice 의 토폴로지를 simple → fanout 으로 변경 ──
    std::cout << "\n--- Hot-swap demo ---\n";
    std::cout << "alice 의 토폴로지를 'simple' → 'fanout' 으로 변경 (deploy/restart 없이)\n";
    customers["alice"].topology_def = topology_fanout();
    customers["alice"].topology_name = "fanout";

    // 같은 process 에서 alice 의 다음 요청은 새 토폴로지로 처리.
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
