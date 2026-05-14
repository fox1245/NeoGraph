// Multi-customer self-evolving chatbot — 5 customer 가 각자 다른 발화
// 패턴을 보이고, 각자 별도 evolution timeline 으로 진화. emergent
// cluster discovery 시연.
//
// server.cpp 는 alice 한 명 시연. 이쪽은 그걸 multi-tenant 로 확장.
// 핵심 관찰:
//   1) 각 customer 가 자기 history 기반으로 *독립적* 으로 evolve.
//   2) compile cache 가 hash 기반이라 *같은 shape 으로 수렴한 customer
//      끼리는 engine 공유* — 6000 customer → ~10 engine 의 emergent
//      효율성.
//   3) 사용자 행동 패턴 (factual / accuracy-seeking / multi-perspective
//      / oscillation) 이 자연스럽게 topology distribution 으로 분류됨
//      — graph_def 가 customer behavior 의 본질적 cluster 발견 메커니즘.
//
// 비용 추정: 5 customer × 5 turn × (~2.3 main + 1 judge) ≈ 82 LLM call
// × gpt-4o-mini ≈ $0.02.
//
// 빌드/실행:
//   cmake --build build --target cookbook_self_evolving_chatbot_multi
//   ./build/cookbook_self_evolving_chatbot_multi

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <cppdotenv/dotenv.hpp>

#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// ── 3 topology shape (server.cpp 와 동일) ────────────────────────────

static json topology_simple() {
    return {
        {"name", "simple"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"respond", {{"type", "llm_call"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "respond"}},
            {{"from", "respond"},   {"to", "__end__"}},
        })},
    };
}

static json topology_reflexive() {
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

class MergeNode : public GraphNode {
    std::string name_;
public:
    explicit MergeNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto msgs = in.state.get("messages");
        std::vector<std::string> all_assistant;
        if (msgs.is_array()) {
            for (const auto& m : msgs)
                if (m.value("role", "") == "assistant")
                    all_assistant.push_back(m.value("content", ""));
        }
        size_t take = std::min<size_t>(3, all_assistant.size());
        std::string merged = "Multiple perspectives:\n";
        for (size_t i = all_assistant.size() - take; i < all_assistant.size(); ++i)
            merged += "- " + all_assistant[i] + "\n";
        NodeOutput out;
        out.writes.push_back({"messages", json::array({
            json{{"role", "assistant"}, {"content", merged}, {"node", name_}}
        })});
        co_return out;
    }
    std::string get_name() const override { return name_; }
};

static json topology_fanout() {
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
            {{"from", "__start__"},     {"to", "perspective_a"}},
            {{"from", "__start__"},     {"to", "perspective_b"}},
            {{"from", "__start__"},     {"to", "perspective_c"}},
            {{"from", "perspective_a"}, {"to", "merge"}},
            {{"from", "perspective_b"}, {"to", "merge"}},
            {{"from", "perspective_c"}, {"to", "merge"}},
            {{"from", "merge"},         {"to", "__end__"}},
        })},
    };
}

static std::unordered_map<std::string, std::function<json()>>
make_topology_registry() {
    return {
        {"simple",    topology_simple},
        {"reflexive", topology_reflexive},
        {"fanout",    topology_fanout},
    };
}

// ── CompileCache (server.cpp 와 동일) ────────────────────────────────

class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<size_t, std::shared_ptr<GraphEngine>> cache_;
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        size_t key = std::hash<std::string>{}(def.dump());
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        }
        auto raw = GraphEngine::compile(def, ctx);
        std::shared_ptr<GraphEngine> engine(raw.release());
        std::unique_lock lk(mu_);
        cache_.emplace(key, engine);
        return engine;
    }
    std::size_t size() { std::shared_lock lk(mu_); return cache_.size(); }
};

// ── LLM judge (server.cpp 와 동일 prompt) ────────────────────────────

static std::string llm_judge_topology(
    const std::shared_ptr<neograph::Provider>& provider,
    const std::vector<json>& history,
    const std::string& current_topology)
{
    std::string hist_str;
    for (const auto& m : history) {
        std::string role = m.value("role", "");
        std::string content = m.value("content", "");
        if (content.size() > 200) content = content.substr(0, 200) + "...";
        hist_str += "- " + role + ": " + content + "\n";
    }

    neograph::CompletionParams p;
    p.model = "gpt-4o-mini";
    p.messages.push_back({
        "system",
        "You are a chatbot harness optimizer. Given a conversation history "
        "and the current topology, decide which topology fits the user best.\n"
        "Options:\n"
        "- simple: 1 LLM call, short direct answer. Good for short/factual Q.\n"
        "- reflexive: 3 LLM calls (draft→critique→final). Good when user "
        "asks for accuracy / careful answers / verification.\n"
        "- fanout: 3 parallel LLM perspectives merged. Good when user wants "
        "multiple angles / comparisons / comprehensive views.\n"
        "Respond with EXACTLY one word: simple OR reflexive OR fanout."
    });
    p.messages.push_back({
        "user",
        "Current topology: " + current_topology +
        "\n\nConversation so far:\n" + hist_str +
        "\nBest topology for THIS user (one word):"
    });

    auto result = provider->complete(p);
    std::string raw = result.message.content;
    std::string word;
    for (char c : raw) {
        if (std::isalpha(static_cast<unsigned char>(c)))
            word += static_cast<char>(std::tolower(c));
        else if (!word.empty()) break;
    }
    if (word == "simple" || word == "reflexive" || word == "fanout") return word;
    return current_topology;
}

// ── Customer behavior patterns — 5 distinct cluster ──────────────────

struct BehaviorPattern {
    std::string id;
    std::string description;
    std::string system_prompt;
    std::vector<std::string> messages;
};

static std::vector<BehaviorPattern> make_patterns() {
    return {
        {
            "alice",
            "factual → multi-perspective (점진 진화)",
            "You are a helpful assistant.",
            {
                "What is HTTP?",
                "What is DNS?",
                "Explain blockchain from both technical and economic perspectives.",
                "Compare microservices vs monolith — cost, complexity, scaling.",
                "Three perspectives on whether AI replaces SaaS?",
            }
        },
        {
            "bob",
            "factual only (변화 없음 — simple 유지 예상)",
            "You are a helpful assistant. Respond concisely.",
            {
                "What is REST?",
                "What is JSON?",
                "What is gRPC?",
                "What is GraphQL?",
                "What is OAuth?",
            }
        },
        {
            "charlie",
            "accuracy-seeking (reflexive 진화 예상)",
            "You are a careful assistant. Be precise.",
            {
                "What is the time complexity of quicksort? Is your answer verified?",
                "Please double-check your last answer for accuracy.",
                "Give me a carefully verified answer about CAP theorem trade-offs.",
                "Analyze B-tree vs B+ tree differences — be precise and verify.",
                "Please review and verify your understanding of Raft consensus.",
            }
        },
        {
            "david",
            "처음부터 multi-perspective (fanout 빠르게 수렴 예상)",
            "You are a helpful assistant.",
            {
                "Compare PostgreSQL vs MongoDB from multiple angles.",
                "Three perspectives on Kubernetes vs Docker Swarm?",
                "What are different views on AI safety from researchers vs industry?",
                "Compare functional vs OOP from various angles.",
                "Multiple perspectives on remote work productivity?",
            }
        },
        {
            "eve",
            "혼합 — oscillation test (anti-flapping 가 없으면 진동 가능)",
            "You are a helpful assistant.",
            {
                "What is TCP?",
                "Compare TCP vs UDP from multiple angles — performance, reliability, use cases.",
                "What is ICMP?",
                "Carefully analyze IPv4 vs IPv6 trade-offs.",
                "What is BGP?",
            }
        },
    };
}

struct CustomerState {
    std::string id;
    std::string description;
    std::string topology_name;
    json        topology_def;
    std::string system_prompt;
    std::vector<json> history;
    std::vector<std::pair<int, std::string>> evolution_log;
    int main_llm_calls = 0;
    int judge_llm_calls = 0;
};

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

// ── main — 5 customer × 5 turn, 각자 별도 evolution timeline ─────────

int main() {
    cppdotenv::auto_load_dotenv();
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "OPENAI_API_KEY missing (.env or export 필요).\n";
        return 1;
    }

    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key = api_key;
    cfg.default_model = "gpt-4o-mini";
    auto provider = neograph::llm::OpenAIProvider::create_shared(cfg);

    NodeFactory::instance().register_type("merge",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MergeNode>(name);
        });

    auto topo_registry = make_topology_registry();
    CompileCache cache;

    // 5 customer 초기화 — 모두 simple topology 로 시작.
    auto patterns = make_patterns();
    std::vector<CustomerState> customers;
    for (auto& p : patterns) {
        CustomerState c;
        c.id = p.id;
        c.description = p.description;
        c.topology_name = "simple";
        c.topology_def = topology_simple();
        c.system_prompt = p.system_prompt;
        customers.push_back(std::move(c));
    }

    std::cout << "\n=== Multi-customer self-evolving chatbot demo ===\n";
    std::cout << "5 customer × 5 turn — 각자 별도 evolution timeline\n";
    std::cout << "All start with 'simple' topology.\n\n";

    auto t_start = std::chrono::steady_clock::now();

    // 각 customer 시퀀셜 처리 (병렬도 가능하지만 demo 출력 명확성).
    for (size_t ci = 0; ci < customers.size(); ++ci) {
        auto& cust = customers[ci];
        const auto& pat = patterns[ci];

        std::cout << "════ Customer: " << cust.id
                  << "  (" << cust.description << ")\n";

        for (size_t turn = 0; turn < pat.messages.size(); ++turn) {
            const std::string& umsg = pat.messages[turn];

            NodeContext ctx;
            ctx.provider = provider;
            ctx.model = "gpt-4o-mini";
            ctx.instructions = cust.system_prompt;
            auto engine = cache.get_or_compile(cust.topology_def, ctx);

            std::vector<json> input_msgs = cust.history;
            input_msgs.push_back({{"role", "user"}, {"content", umsg}});

            RunConfig rcfg;
            rcfg.thread_id = cust.id + "__main";
            rcfg.input = {{"messages", json::array()}};
            for (const auto& m : input_msgs)
                rcfg.input["messages"].push_back(m);

            auto result = engine->run(rcfg);
            auto out_msgs = result.output["channels"]["messages"]["value"];
            std::string final_reply;
            if (out_msgs.is_array()) {
                for (const auto& m : out_msgs)
                    if (m.value("role", "") == "assistant")
                        final_reply = m.value("content", "");
            }

            cust.history.push_back({{"role", "user"}, {"content", umsg}});
            cust.history.push_back({{"role", "assistant"}, {"content", final_reply}});

            cust.main_llm_calls += (cust.topology_name == "simple") ? 1 : 3;

            std::string suggested = llm_judge_topology(
                provider, cust.history, cust.topology_name);
            cust.judge_llm_calls += 1;

            std::string evolve_marker = "";
            if (suggested != cust.topology_name) {
                cust.evolution_log.push_back({(int)turn + 1, suggested});
                evolve_marker = "  ⟹ EVOLVE → " + suggested;
                cust.topology_name = suggested;
                cust.topology_def = topo_registry[suggested]();
            }

            std::string preview = final_reply.size() > 80
                ? final_reply.substr(0, 80) + "..."
                : final_reply;
            std::cout << "  Turn " << (turn + 1) << " [" << cust.topology_name
                      << "]: " << preview << evolve_marker << "\n";
        }
        std::cout << "\n";
    }

    auto t_end = std::chrono::steady_clock::now();
    long total_s = std::chrono::duration_cast<std::chrono::seconds>(
        t_end - t_start).count();

    // ── 종합 stats ──────────────────────────────────────────────────
    std::cout << "════════════════════════════════════════════════════\n";
    std::cout << "=== Aggregate stats ===\n";
    int total_main = 0, total_judge = 0;
    std::map<std::string, int> final_topology_dist;
    for (const auto& c : customers) {
        total_main += c.main_llm_calls;
        total_judge += c.judge_llm_calls;
        final_topology_dist[c.topology_name]++;
    }
    std::cout << "Customers:           " << customers.size() << "\n";
    std::cout << "Total turns:         " << (customers.size() * 5) << "\n";
    std::cout << "Total main LLM:      " << total_main << "\n";
    std::cout << "Total judge LLM:     " << total_judge << "\n";
    std::cout << "Total LLM calls:     " << (total_main + total_judge) << "\n";
    std::cout << "Wall time:           " << total_s << " s\n";
    std::cout << "Peak RSS:            " << peak_rss_kb() << " KB ("
              << peak_rss_kb() / 1024.0 << " MB)\n";
    std::cout << "Compile cache size:  " << cache.size()
              << "  (distinct engine 인스턴스 — customer 수보다 적으면 emergent cluster)\n";

    std::cout << "\n=== Evolution timelines ===\n";
    for (const auto& c : customers) {
        std::cout << c.id << ":  simple(initial)";
        for (const auto& [t, name] : c.evolution_log)
            std::cout << "  →  " << name << "(t" << t << ")";
        std::cout << "  | final = " << c.topology_name << "\n";
    }

    std::cout << "\n=== Final topology distribution ===\n";
    for (const auto& [topo, count] : final_topology_dist) {
        std::cout << "  " << topo << ": " << count << " customer"
                  << (count > 1 ? "s" : "") << "\n";
    }

    std::cout << "\n=== Emergent cluster observation ===\n";
    std::cout << "5 customer 의 행동 패턴이 자연스럽게 "
              << final_topology_dist.size() << " 개 topology cluster 로 분류됨.\n";
    std::cout << "Compile cache 가 distinct shape 별 engine 공유 — "
              << customers.size() << " customer → "
              << cache.size() << " engine 인스턴스.\n";
    if (cache.size() < customers.size()) {
        std::cout << "즉 customer "
                  << (customers.size() - cache.size())
                  << " 명 분량의 engine 메모리가 cache 공유로 절감됨.\n";
        std::cout << "1000 customer 로 scale-up 시: distinct shape 이 여전히\n";
        std::cout << (cache.size() + 1) << "~10 개 정도면 engine 인스턴스 ~"
                  << (cache.size() + 1) << " 개로 유지 → 메모리 거의 일정.\n";
    }

    return 0;
}
