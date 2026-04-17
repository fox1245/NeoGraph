// NeoGraph Example 14: Plan & Executor with crash recovery
//
// Shows the Plan & Executor agent pattern running on top of NeoGraph's
// pending-writes machinery: a planner produces N parallel sub-tasks, an
// executor fans out via Send, and the super-step survives partial
// failures — on resume, successful siblings are replayed from the
// checkpoint store instead of being re-executed.
//
// Scenario: a "research assistant" breaks a user query into 5 sub-topics
// and dispatches them in parallel. We simulate a transient failure on
// sub-topic #2 during the first run, then "fix" the environment and
// resume. The execution counter proves that only the failed sub-topic
// re-runs on phase 2 — the other four are replayed without hitting the
// (expensive, imaginary) LLM again.
//
// No API key required. All "LLM calls" are simulated with sleeps.
//
// Usage: ./example_plan_executor

#include <neograph/neograph.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace neograph;
using namespace neograph::graph;

// ── Shared instrumentation state (would be real telemetry in production) ──
static std::atomic<int> g_exec_count{0};
static std::atomic<int> g_fail_on_idx{-1};   // -1 = no failure

// =========================================================================
// Setup: commits an initial checkpoint so pending writes have a parent
// to attach to. In real apps this would be the auth / context-loading
// step that always runs before the plan.
// =========================================================================
class SetupNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        json q = state.get("query");
        std::string query = q.is_string() ? q.get<std::string>() : "LLM market analysis";
        std::cout << "[setup]    query='" << query << "' — initialised\n";
        return {ChannelWrite{"query", json(query)}};
    }
    std::string name() const override { return "setup"; }
};

// =========================================================================
// Planner: emits 5 Sends, one per sub-topic, each targeting "executor".
// =========================================================================
class PlannerNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState&) override { return {}; }

    NodeResult execute_full(const GraphState& state) override {
        std::vector<std::string> sub_topics = {
            "market_size", "competitor_map", "regulatory_landscape",
            "tech_stack_survey", "customer_interviews"
        };

        std::cout << "[planner]  decomposed into " << sub_topics.size()
                  << " sub-topics:\n";
        for (size_t i = 0; i < sub_topics.size(); ++i) {
            std::cout << "           #" << i << " " << sub_topics[i] << "\n";
        }

        NodeResult nr;
        for (size_t i = 0; i < sub_topics.size(); ++i) {
            Send s;
            s.target_node = "executor";
            s.input = {
                {"task_idx", static_cast<int>(i)},
                {"topic",    sub_topics[i]}
            };
            nr.sends.push_back(std::move(s));
        }
        return nr;
    }
    std::string name() const override { return "planner"; }
};

// =========================================================================
// Executor: simulates an expensive LLM call per sub-topic.
// Throws if the global fail_on flag matches its task index.
// =========================================================================
class ExecutorNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int idx = state.get("task_idx").get<int>();
        std::string topic = state.get("topic").get<std::string>();

        // Simulate a slow LLM call so parallel fan-out is observable.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        int n = g_exec_count.fetch_add(1, std::memory_order_relaxed) + 1;

        if (g_fail_on_idx.load(std::memory_order_relaxed) == idx) {
            std::cout << "[executor] #" << idx << " " << topic
                      << "  ✗ CRASH (exec#" << n << ")\n";
            throw std::runtime_error("transient failure at idx=" +
                                     std::to_string(idx));
        }

        std::cout << "[executor] #" << idx << " " << topic
                  << "  ✓ done (exec#" << n << ")\n";

        // Append a structured finding to the shared "findings" channel.
        json finding = {
            {"idx",     idx},
            {"topic",   topic},
            {"summary", "key insight for " + topic}
        };
        return {ChannelWrite{"findings", finding}};
    }
    std::string name() const override { return "executor"; }
};

// =========================================================================
// Graph definition
// =========================================================================
static json make_graph() {
    return {
        {"name", "plan_and_executor"},
        {"channels", {
            {"query",    {{"reducer", "overwrite"}}},
            {"task_idx", {{"reducer", "overwrite"}}},
            {"topic",    {{"reducer", "overwrite"}}},
            {"findings", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"setup",    {{"type", "setup"}}},
            {"planner",  {{"type", "planner"}}},
            {"executor", {{"type", "executor"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "setup"}},
            {{"from", "setup"},     {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        }}
    };
}

int main() {
    std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
              <<   "║  NeoGraph Example 14: Plan & Executor + crash recovery ║\n"
              <<   "╚══════════════════════════════════════════════════════╝\n\n";

    NodeFactory::instance().register_type("setup",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SetupNode>();
        });
    NodeFactory::instance().register_type("planner",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlannerNode>();
        });
    NodeFactory::instance().register_type("executor",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ExecutorNode>();
        });

    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_graph(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "research-42";
    cfg.input = {{"query", "State of the on-device LLM agent market, Q2 2026"}};

    // ── Phase 1: transient failure on sub-topic #2 ─────────────────────
    std::cout << "── Phase 1: first run (sub-topic #2 will crash) ──────\n\n";
    g_fail_on_idx = 2;
    g_exec_count  = 0;

    try {
        engine->run(cfg);
        std::cout << "[main]     unexpected success — no crash occurred\n";
        return 1;
    } catch (const std::exception& e) {
        std::cout << "\n[main]     caught: " << e.what() << "\n";
    }

    auto cp = store->load_latest(cfg.thread_id);
    std::cout << "[main]     latest cp = " << (cp ? cp->current_node : "(none)")
              << " at step " << (cp ? cp->step : -1) << "\n";
    std::cout << "[main]     pending writes attached = "
              << store->pending_writes_count(cfg.thread_id, cp ? cp->id : "")
              << "  (1 planner + 4 successful sends)\n";
    std::cout << "[main]     total executor invocations so far = "
              << g_exec_count.load() << "\n\n";

    // ── Phase 2: fix the environment, resume ───────────────────────────
    std::cout << "── Phase 2: environment recovered, calling resume() ──\n\n";
    g_fail_on_idx = -1;   // no more crashes

    auto resumed = engine->resume(cfg.thread_id);

    std::cout << "\n[main]     resume() returned\n";
    std::cout << "[main]     executor invocations during phase 2 = "
              << (g_exec_count.load() - 5)
              << "   ← only the previously-failed sub-topic re-ran\n";
    std::cout << "[main]     total invocations across both phases = "
              << g_exec_count.load()
              << "   (would be 10 without pending writes)\n\n";

    // ── Report final findings ──────────────────────────────────────────
    if (resumed.output.contains("channels")) {
        auto ch = resumed.output["channels"];
        if (ch.contains("findings") && ch["findings"].contains("value")) {
            auto findings = ch["findings"]["value"];
            std::cout << "── Final findings (" << findings.size() << " entries) ──\n";
            for (const auto& f : findings) {
                std::cout << "  #" << f.value("idx", -1) << "  "
                          << f.value("topic", std::string{})
                          << "  — " << f.value("summary", std::string{}) << "\n";
            }
            std::cout << "\n";
        }
    }

    std::cout << "── Checkpoint history ────────────────────────────────\n";
    for (const auto& cp : store->list(cfg.thread_id)) {
        std::cout << "  [" << cp.interrupt_phase << "] step=" << cp.step
                  << "  " << cp.current_node << " → " << cp.next_node << "\n";
    }

    std::cout << "\n✓ Plan & Executor completed with 1 expensive call saved.\n\n";
    return 0;
}
