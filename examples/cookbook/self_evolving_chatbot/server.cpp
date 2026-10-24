// Self-evolving multi-tenant chatbot — the graph topology observes user
// behavior and evolves *itself*.
//
// Scenario: alice's chatbot harness starts on the 'simple' topology.
// At the end of each turn, an LLM judge reads the conversation history and
// the current topology to pick the best-fit topology for this user. If it
// differs, customer_db's graph_def is updated in place so the next turn
// runs on the new topology.
//
// This extends the multi_tenant_chatbot cookbook: there each customer's
// harness is *fixed*, whereas here it *evolves*. In LangGraph, StateGraph is
// a Python object that cannot practically reshape itself at runtime; NeoGraph's
// graph-as-JSON model makes this possible.
//
// Cost estimate: 5 turns × (main LLM call ~2.3 + judge LLM call 1) = ~16
// pinned DeepSeek via OpenRouter calls.
//
// Build/run:
//   cmake --build build --target cookbook_self_evolving_chatbot
//   ./build/cookbook_self_evolving_chatbot
//
// smoke-skip: needs OPENROUTER_API_KEY (live model call)

#include <neograph/neograph.h>
#include "../../provider_example_support.h"
#include <cppdotenv/dotenv.hpp>

#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// ── (1) 3 candidate topologies — same shapes as multi_tenant_chatbot ─

static json topology_simple() {
    return {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
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

// ── (2) Compile cache (same as multi_tenant_chatbot) ─────────────────

class CompileCache {
    std::shared_mutex mu_;
    using Key = std::pair<std::shared_ptr<Provider>, std::string>;
    std::map<Key, std::shared_ptr<GraphEngine>> cache_;
public:
    std::shared_ptr<GraphEngine> get_or_compile(
        const std::string& customer_id, const json& def, const NodeContext& ctx) {
        // Pin the exact provider capability and every captured request origin.
        Key key{ctx.provider, json::array({customer_id, def, ctx.model,
            ctx.instructions, ctx.extra_config, ctx.provider_name}).dump()};
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        }
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        std::unique_lock lk(mu_);
        return cache_.emplace(std::move(key), engine).first->second;
    }
    std::size_t size() { std::shared_lock lk(mu_); return cache_.size(); }
};

// ── (3) LLM judge — history + current topology → next topology ───────
//
// Core of self-evolution. ~deepseek/deepseek-v4-flash-latest reads the user's
// conversation style and replies with one word: 'simple', 'reflexive', or
// 'fanout'.
// In production, use a richer judgment prompt, few-shot examples, and metrics.

static std::string llm_judge_topology(
    const std::shared_ptr<neograph::Provider>& provider,
    const std::vector<json>& history,
    const std::string& current_topology,
    UsageAccumulator& usage)
{
    std::string hist_str;
    for (const auto& m : history) {
        std::string role = m.value("role", "");
        std::string content = m.value("content", "");
        if (content.size() > 200) content = content.substr(0, 200) + "...";
        hist_str += "- " + role + ": " + content + "\n";
    }

    // A separate optimizer role consumes an explicit portable text summary,
    // not the answering model's native continuation authority.
    std::vector<sp::Message> messages;
    messages.push_back(portable_message({
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
    }));
    messages.push_back(portable_message({
        "user",
        "Current topology: " + current_topology +
        "\n\nConversation so far:\n" + hist_str +
        "\nBest topology for THIS user (one word):"
    }));

    auto result = provider->invoke(make_provider_request(
        *provider, "~deepseek/deepseek-v4-flash-latest", std::move(messages)));
    usage.add(outcome_usage(*result));
    result = outcome_or_throw(std::move(result));
    std::string raw = outcome_text(*result);
    // Lowercase and extract the first word
    std::string word;
    for (char c : raw) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            word += static_cast<char>(std::tolower(c));
        } else if (!word.empty()) {
            break;
        }
    }
    if (word == "simple" || word == "reflexive" || word == "fanout") return word;
    return current_topology;   // unparseable judge reply → keep current topology
}

// ── (4) Customer record + DB ────────────────────────────────────────

struct CustomerRecord {
    std::string id;
    std::string topology_name;
    json        topology_def;
    std::string system_prompt;
    // Portable user/final-reply text. Native output can't be carried over: the
    // topology changes between turns, and fan-out branches don't extend each
    // other's request prefix, so the next turn's calls could not replay it.
    std::vector<sp::Message> answer_history;
    std::vector<json> history;              // Portable final-text observations for the judge.
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

// ── (5) main — 5-turn simulation, judge → evolve at the end of each turn ──

static int run_demo() {
    cppdotenv::auto_load_dotenv();
    const char* api_key = std::getenv("OPENROUTER_API_KEY");
    if (!api_key) {
        std::cerr << "OPENROUTER_API_KEY missing (.env or export 필요).\n";
        return 1;
    }

    std::shared_ptr<Provider> provider = examples::make_openrouter_provider(api_key, "chat");

    NodeFactory::instance().register_type("merge",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MergeNode>(name);
        });

    auto topo_registry = make_topology_registry();
    CompileCache cache;

    // Alice — starts on the 'simple' topology and evolves with user behavior.
    CustomerRecord alice;
    alice.id = "alice";
    alice.topology_name = "simple";
    alice.topology_def = topology_simple();
    alice.system_prompt =
        "You are a helpful assistant. Respond in 1-2 short sentences max.";

    // 5-turn user utterances — progressively demanding multiple angles and deeper analysis.
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
    auto reported_usage = std::make_shared<UsageAccumulator>();

    for (size_t turn = 0; turn < user_messages.size(); ++turn) {
        const std::string& umsg = user_messages[turn];
        std::cout << "── Turn " << (turn + 1)
                  << " [topology=" << alice.topology_name << "] ──\n";
        std::cout << "User: " << umsg << "\n";

        // (a) Respond using the current topology.
        NodeContext ctx;
        ctx.provider = provider;
        ctx.model = "~deepseek/deepseek-v4-flash-latest";
        ctx.instructions = alice.system_prompt;
        auto engine = cache.get_or_compile(alice.id, alice.topology_def, ctx);

        auto answering_history = alice.answer_history;
        answering_history.push_back(portable_message({"user", umsg}));

        RunConfig rcfg;
        rcfg.thread_id = "alice__main";
        rcfg.usage = reported_usage;
        rcfg.provider_messages = std::move(answering_history);

        auto result = engine->run(rcfg);
        auto out_msgs = result.output["channels"]["messages"]["value"];

        // Extract the last assistant reply.
        std::string final_reply;
        if (out_msgs.is_array()) {
            for (const auto& m : out_msgs)
                if (m.value("role", "") == "assistant")
                    final_reply = m.value("content", "");
        }
        if (final_reply.find_first_not_of(" \t\r\n") == std::string::npos)
            throw std::runtime_error("Answering graph produced no visible reply");
        alice.answer_history.push_back(portable_message({"user", umsg}));
        alice.answer_history.push_back(portable_message({"assistant", final_reply}));
        std::cout << "Bot:  " << (final_reply.size() > 200
                                  ? final_reply.substr(0, 200) + "..."
                                  : final_reply) << "\n";

        // Update history (user + last assistant).
        alice.history.push_back({{"role", "user"}, {"content", umsg}});
        alice.history.push_back({{"role", "assistant"}, {"content", final_reply}});

        // Accumulate LLM call count per topology (for demo reporting).
        if (alice.topology_name == "simple") main_llm_calls += 1;
        else                                  main_llm_calls += 3;

        // (b) LLM judge — choose the topology for the next turn.
        std::cout << "[Evaluating harness fit...] ";
        std::string suggested = llm_judge_topology(
            provider, alice.history, alice.topology_name, *reported_usage);
        judge_llm_calls += 1;
        std::cout << "judge → " << suggested << "\n";

        // (c) If different, evolve in place. A single customer_db UPDATE is
        //     NeoGraph's hot-swap; the new hash misses the cache on the next
        //     turn, compiling and caching the new engine automatically.
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
    std::cout << "Reported usage:      " << usage_to_json(reported_usage->snapshot()).dump() << "\n";
    std::cout << "Monetary charge:     unknown (not inferred from call counts)\n";
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

// Report provider and node failures with a deliberate failure exit, rather
// than leaving them uncaught at the process boundary.
int main() {
    try {
        return run_demo();
    } catch (const std::exception& error) {
        std::cout << std::flush;
        std::cerr << "cookbook_self_evolving_chatbot: " << error.what() << "\n";
    } catch (...) {
        std::cerr << "cookbook_self_evolving_chatbot: unknown error\n";
    }
    return 1;
}
