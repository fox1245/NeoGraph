// Self-evolving multi-tenant chatbot — graph topology 가 사용자 행동을
// 보고 *자기 자신을* 진화시킴.
//
// 시나리오: alice 의 chatbot harness 가 처음엔 'simple' topology 로 시작.
// 매 turn 끝나면 LLM judge 가 conversation history + 현재 topology 를
// 보고 "이 사용자에게 더 적합한 topology" 를 판단. 다르면 즉시 in-place
// 로 customer_db 의 graph_def 를 업데이트 → 다음 turn 부터 새 topology
// 로 처리.
//
// 이게 multi_tenant_chatbot cookbook 의 자연스러운 확장. 그쪽은 customer
// 별 harness 가 *고정* — 이쪽은 *진화*. LangGraph 로는 StateGraph 가
// Python 객체라 runtime 에 자기 자신을 reshape 하는 게 사실상 불가능,
// NG 의 graph-as-JSON 모델만 가능한 카테고리.
//
// 비용 추정: 5 turn × (main LLM call ~2.3 + judge LLM call 1) = ~16
// gpt-4o-mini call ≈ $0.003.
//
// 빌드/실행:
//   cmake --build build --target cookbook_self_evolving_chatbot
//   ./build/cookbook_self_evolving_chatbot

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <cppdotenv/dotenv.hpp>

#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// ── (1) 3 후보 topology — multi_tenant_chatbot 과 동일 shape ────────

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

// ── (2) Compile cache (multi_tenant_chatbot 그대로) ─────────────────

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

// ── (3) LLM judge — history + 현재 topology → 다음 topology ─────────
//
// 진짜 self-evolution 의 핵심. gpt-4o-mini 가 사용자 대화 스타일을 보고
// 'simple / reflexive / fanout' 중 best fit 을 한 단어로 응답.
// Production 이면 judgment prompt + few-shot + 더 풍부한 metric (응답
// 만족도, 사용자 후속 질문 패턴 등) 결합 가능.

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
        "and the current topology, decide which topology fits the user best. "
        "Options:\n"
        "- simple: 1 LLM call, short direct answer. Good for short/factual Q.\n"
        "- reflexive: 3 LLM calls (draft→critique→final). Good when user "
        "asks for accuracy / careful answers.\n"
        "- fanout: 3 parallel LLM perspectives merged. Good when user wants "
        "multiple angles / comprehensive views.\n"
        "Respond with EXACTLY one word: simple OR reflexive OR fanout. "
        "No punctuation, no explanation."
    });
    p.messages.push_back({
        "user",
        "Current topology: " + current_topology +
        "\n\nConversation so far:\n" + hist_str +
        "\nBest topology for THIS user (one word):"
    });

    // complete() 는 v1.0 에서 deprecated (invoke() 로 통합 예정). 데모
    // 는 sync 단순성 위해 그대로 사용 — 1 turn 마다 1 judge call.
    auto result = provider->complete(p);
    std::string raw = result.message.content;
    // 소문자 + 첫 단어 추출
    std::string word;
    for (char c : raw) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            word += static_cast<char>(std::tolower(c));
        } else if (!word.empty()) {
            break;
        }
    }
    if (word == "simple" || word == "reflexive" || word == "fanout") return word;
    return current_topology;   // judge 결과 unparseable → 그대로 유지
}

// ── (4) Customer record + DB ────────────────────────────────────────

struct CustomerRecord {
    std::string id;
    std::string topology_name;
    json        topology_def;
    std::string system_prompt;
    std::vector<json> history;     // session memory (per-customer, demo 단순화)
    std::vector<std::pair<int, std::string>> evolution_log;  // (turn, topology)
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

// ── (5) main — 5-turn 시뮬레이션, 매 turn 끝에 judge → evolve ────────

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

    // Alice — 처음엔 'simple' topology 로 시작. 사용자 행동 보고 진화.
    CustomerRecord alice;
    alice.id = "alice";
    alice.topology_name = "simple";
    alice.topology_def = topology_simple();
    alice.system_prompt =
        "You are a helpful assistant. Respond in 1-2 short sentences max.";

    // 5-turn 사용자 발화 — 점점 다중 관점/심층 분석 요구가 늘어남.
    std::vector<std::string> user_messages = {
        "What is a cloud?",
        "What is HTTP?",
        "Now explain blockchain to me — I want both the technical view and the economic view.",
        "Compare microservices vs monolith from multiple angles: cost, complexity, team scaling.",
        "Give me three different perspectives on whether AI agents will replace SaaS.",
    };

    std::cout << "\n=== Self-evolving chatbot demo — alice 의 harness 진화 ===\n";
    std::cout << "Starting topology: " << alice.topology_name << "\n\n";

    auto t_start = std::chrono::steady_clock::now();
    int main_llm_calls = 0;
    int judge_llm_calls = 0;

    for (size_t turn = 0; turn < user_messages.size(); ++turn) {
        const std::string& umsg = user_messages[turn];
        std::cout << "── Turn " << (turn + 1)
                  << " [topology=" << alice.topology_name << "] ──\n";
        std::cout << "User: " << umsg << "\n";

        // (a) 현재 topology 로 응답.
        NodeContext ctx;
        ctx.provider = provider;
        ctx.model = "gpt-4o-mini";
        ctx.instructions = alice.system_prompt;
        auto engine = cache.get_or_compile(alice.topology_def, ctx);

        // history 누적해서 input 으로 전달.
        std::vector<json> input_msgs = alice.history;
        input_msgs.push_back({{"role", "user"}, {"content", umsg}});

        RunConfig rcfg;
        rcfg.thread_id = "alice__main";
        rcfg.input = {{"messages", json::array()}};
        for (const auto& m : input_msgs)
            rcfg.input["messages"].push_back(m);

        auto result = engine->run(rcfg);
        auto out_msgs = result.output["channels"]["messages"]["value"];

        // 마지막 assistant 응답 추출.
        std::string final_reply;
        if (out_msgs.is_array()) {
            for (const auto& m : out_msgs)
                if (m.value("role", "") == "assistant")
                    final_reply = m.value("content", "");
        }
        std::cout << "Bot:  " << (final_reply.size() > 200
                                  ? final_reply.substr(0, 200) + "..."
                                  : final_reply) << "\n";

        // history 업데이트 (user + last assistant).
        alice.history.push_back({{"role", "user"}, {"content", umsg}});
        alice.history.push_back({{"role", "assistant"}, {"content", final_reply}});

        // topology 별 LLM call 수 누적 (시연용 카운트).
        if (alice.topology_name == "simple") main_llm_calls += 1;
        else                                  main_llm_calls += 3;

        // (b) LLM judge — 다음 turn 의 topology 결정.
        std::cout << "[Evaluating harness fit...] ";
        std::string suggested = llm_judge_topology(
            provider, alice.history, alice.topology_name);
        judge_llm_calls += 1;
        std::cout << "judge → " << suggested << "\n";

        // (c) 다르면 in-place evolve. customer_db UPDATE 한 줄 = NG 의 진짜
        //     hot-swap. cache 는 새 hash 라 다음 turn 에서 자동으로 새
        //     engine 컴파일 후 영구 캐시.
        if (suggested != alice.topology_name) {
            std::cout << "  ⟹ EVOLVE: " << alice.topology_name
                      << " → " << suggested << " (in-place, deploy 0)\n";
            alice.evolution_log.push_back({(int)turn + 1, suggested});
            alice.topology_name = suggested;
            alice.topology_def = topo_registry[suggested]();
        }
        std::cout << "\n";
    }
    auto t_end = std::chrono::steady_clock::now();
    long total_s = std::chrono::duration_cast<std::chrono::seconds>(
        t_end - t_start).count();

    std::cout << "=== Summary ===\n";
    std::cout << "Turns:                " << user_messages.size() << "\n";
    std::cout << "Final topology:       " << alice.topology_name << "\n";
    std::cout << "Main LLM calls:       " << main_llm_calls << "\n";
    std::cout << "Judge LLM calls:      " << judge_llm_calls << "\n";
    std::cout << "Total LLM calls:      " << (main_llm_calls + judge_llm_calls) << "\n";
    std::cout << "Wall time:            " << total_s << " s\n";
    std::cout << "Peak RSS:             " << peak_rss_kb() << " KB ("
              << peak_rss_kb() / 1024.0 << " MB)\n";
    std::cout << "Compile cache size:   " << cache.size()
              << " (= distinct topology 수 used)\n";

    std::cout << "\n--- Evolution timeline ---\n";
    if (alice.evolution_log.empty()) {
        std::cout << "(no evolution — judge thinks initial topology fits)\n";
    } else {
        std::cout << "Turn 0:  simple  (initial)\n";
        for (const auto& [t, name] : alice.evolution_log)
            std::cout << "Turn " << t << ":  " << name << "  (evolved)\n";
    }

    std::cout << "\nKey demonstration:\n";
    std::cout << "  - Customer harness reshaped itself live based on user behavior\n";
    std::cout << "  - DB-level change (graph_def JSON) only — zero deploy / restart\n";
    std::cout << "  - Compile cache automatic — new hash → new engine compiled once\n";
    std::cout << "  - LangGraph cannot do this: StateGraph is a Python object,\n";
    std::cout << "    not data — runtime reshape requires module reload + state loss.\n";

    return 0;
}
