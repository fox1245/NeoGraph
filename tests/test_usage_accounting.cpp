// Token usage comes out of a run (issue #88).
//
// ChatCompletion::Usage existed and SchemaProvider parsed it, but nothing
// downstream kept it: LLMCallNode never read completion.usage and RunResult had
// no field for it. Call a Provider directly and you could see token counts; run
// a graph and they were gone. For anything cost-sensitive — budgets, per-tenant
// billing, rate-limit planning — that is a hard blocker.
//
// The accumulator rides on RunContext, which already flows into every node and
// down into subgraphs via RunConfig, exactly like cancel_token. Two consequences
// worth testing rather than assuming:
//
//   * Usage is recorded where the completion is *received* (the node), not where
//     it is produced (the provider). RateLimitedProvider wraps another provider
//     and delegates to it, so counting in the provider layer would count the
//     same completion twice. Counting on receipt cannot.
//
//   * A subgraph runs on its own engine with its own RunConfig. If the
//     accumulator does not ride that config down, a graph that delegates its LLM
//     work to a subgraph reports zero tokens — the most misleading possible
//     answer, since it looks like a free run.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/llm/agent.h>

#include <memory>
#include <atomic>
#include <limits>

#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Reports a fixed usage on every completion.
class UsageProvider : public Provider {
public:
    UsageProvider(int prompt, int completion)
        : prompt_(prompt), completion_(completion) {}

    ChatCompletion complete(const CompletionParams&) override {
        ChatCompletion c;
        c.message = ChatMessage{"assistant", "ok"};
        c.usage.prompt_tokens     = prompt_;
        c.usage.completion_tokens = completion_;
        c.usage.total_tokens      = prompt_ + completion_;
        return c;
    }
    ChatCompletion complete_stream(const CompletionParams& p,
                                   const StreamCallback&) override {
        return complete(p);
    }
    std::string get_name() const override { return "usage-stub"; }

private:
    int prompt_;
    int completion_;
};

json llm_graph(int llm_nodes) {
    json nodes  = json::object();
    json edges  = json::array();
    std::string prev = "__start__";
    for (int i = 0; i < llm_nodes; ++i) {
        std::string name = "llm" + std::to_string(i);
        nodes[name] = {{"type", "llm_call"}};
        edges.push_back({{"from", prev}, {"to", name}});
        prev = name;
    }
    edges.push_back({{"from", prev}, {"to", "__end__"}});

    return {
        {"name", "usage_test"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", nodes},
        {"edges", edges}
    };
}

RunResult run_graph(const json& def, std::shared_ptr<Provider> provider,
                    const std::string& thread) {
    NodeContext ctx;
    ctx.provider = std::move(provider);
    auto engine = GraphEngine::compile(def, ctx);

    RunConfig cfg;
    cfg.thread_id = thread;
    return engine->run(cfg);
}

}  // namespace

// One LLM node: whatever the provider reported must come back out.
TEST(UsageAccounting, SingleLLMNodeReportsUsage) {
    auto result = run_graph(llm_graph(1),
                            std::make_shared<UsageProvider>(10, 5), "single");

    EXPECT_EQ(result.usage.prompt_tokens, 10);
    EXPECT_EQ(result.usage.completion_tokens, 5);
    EXPECT_EQ(result.usage.total_tokens, 15);
}

// Two LLM nodes: the run total is the sum, not the last node's.
TEST(UsageAccounting, MultipleLLMNodesSum) {
    auto result = run_graph(llm_graph(2),
                            std::make_shared<UsageProvider>(10, 5), "double");

    EXPECT_EQ(result.usage.total_tokens, 30);
    EXPECT_EQ(result.usage.prompt_tokens, 20);
    EXPECT_EQ(result.usage.completion_tokens, 10);
}

// A graph with no LLM node reports zero, not an error and not garbage.
TEST(UsageAccounting, NoLLMNodeYieldsZero) {
    json def = {
        {"name", "no_llm"},
        {"channels", {{"x", {{"reducer", "overwrite"}}}}},
        {"nodes", json::object()},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "__end__"}}
        })}
    };
    def["nodes"] = json::object();

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    RunConfig cfg;
    cfg.thread_id = "empty";
    auto result = engine->run(cfg);

    EXPECT_EQ(result.usage.total_tokens, 0);
}

// A subgraph runs on its own engine with its own RunConfig. Its tokens are still
// the parent run's tokens — otherwise delegating LLM work to a subgraph makes a
// run look free.
TEST(UsageAccounting, SubgraphUsageRollsUpIntoTheParent) {
    json inner = llm_graph(1);
    inner["name"] = "inner";

    json outer = {
        {"name", "outer"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"child", {{"type", "subgraph"},
                       {"definition", inner}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "child"}},
            {{"from", "child"},     {"to", "__end__"}}
        })}
    };

    auto result = run_graph(outer, std::make_shared<UsageProvider>(10, 5), "sub");

    EXPECT_EQ(result.usage.total_tokens, 15)
        << "the subgraph's tokens did not reach the parent run";
}

// Budget controls are execution-scoped state and must survive the separate
// RunConfig that SubgraphNode creates for a child engine.
TEST(UsageAccounting, SubgraphPropagatesProgramBudgetContext) {
    auto budget = std::make_shared<std::atomic_bool>(false);
    auto seen_tokens = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto saw_same_budget = std::make_shared<std::atomic_bool>(false);
    NodeFactory::instance().register_type(
        "usage_budget_probe_2026",
        [budget, seen_tokens, saw_same_budget](const std::string& name, const json&,
                                                const NodeContext&) {
            class BudgetProbeNode final : public GraphNode {
            public:
                BudgetProbeNode(std::string name, std::shared_ptr<std::atomic_bool> expected_budget,
                                std::shared_ptr<std::atomic<std::uint64_t>> seen_tokens,
                                std::shared_ptr<std::atomic_bool> saw_same_budget)
                    : name_(std::move(name)),
                      expected_budget_(std::move(expected_budget)),
                      seen_tokens_(std::move(seen_tokens)),
                      saw_same_budget_(std::move(saw_same_budget)) {}

                asio::awaitable<NodeOutput> run(NodeInput in) override {
                    seen_tokens_->store(in.ctx.model_token_budget, std::memory_order_relaxed);
                    saw_same_budget_->store(in.ctx.budget_exhausted == expected_budget_,
                                            std::memory_order_relaxed);
                    co_return NodeOutput{};
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                std::shared_ptr<std::atomic_bool> expected_budget_;
                std::shared_ptr<std::atomic<std::uint64_t>> seen_tokens_;
                std::shared_ptr<std::atomic_bool> saw_same_budget_;
            };
            return std::make_unique<BudgetProbeNode>(
                name, std::move(budget), std::move(seen_tokens), std::move(saw_same_budget));
        });

    const json inner = {
        {"name", "budget_inner"},
        {"channels", json::object()},
        {"nodes", {{"probe", {{"type", "usage_budget_probe_2026"}}}}},
        {"edges", json::array({
                      {{"from", "__start__"}, {"to", "probe"}},
                      {{"from", "probe"}, {"to", "__end__"}}
                  })}};
    const json outer = {
        {"name", "budget_outer"},
        {"channels", json::object()},
        {"nodes", {{"child", {{"type", "subgraph"}, {"definition", inner}}}}},
        {"edges", json::array({
                      {{"from", "__start__"}, {"to", "child"}},
                      {{"from", "child"}, {"to", "__end__"}}
                  })}};
    auto engine = GraphEngine::compile(outer, NodeContext{});

    RunConfig config;
    config.model_token_budget = 123;
    config.budget_exhausted = budget;
    ASSERT_NO_THROW(engine->run(config));
    EXPECT_EQ(seen_tokens->load(std::memory_order_relaxed), 123U);
    EXPECT_TRUE(saw_same_budget->load(std::memory_order_relaxed));
}

TEST(UsageAccounting, WideTotalSurvivesPublicIntCounterOverflow) {
    UsageAccumulator usage;
    ChatCompletion::Usage first;
    first.total_tokens = std::numeric_limits<int>::max();
    usage.add(first);
    ChatCompletion::Usage second;
    second.total_tokens = 1;
    usage.add(second);

    EXPECT_EQ(usage.total_tokens_wide(),
              static_cast<long long>(std::numeric_limits<int>::max()) + 1);
}

// The other way to drive an LLM. Agent is not a graph run and has no
// RunContext, so it keeps its own total — and it has to, or token accounting
// would exist on one of the two paths and not the other, which is the exact
// split #87 was about.
TEST(UsageAccounting, AgentAccumulatesAcrossCalls) {
    neograph::llm::Agent agent(std::make_shared<UsageProvider>(10, 5),
                               std::vector<std::unique_ptr<Tool>>{});

    EXPECT_EQ(agent.usage().total_tokens, 0) << "nothing called yet";

    std::vector<ChatMessage> messages{{"user", "hi"}};
    agent.run(messages);
    EXPECT_EQ(agent.usage().total_tokens, 15);

    // Cumulative over the agent's lifetime, not reset per run: an agent loop
    // makes several calls and the number people want is what the conversation
    // cost, not what the last turn cost.
    std::vector<ChatMessage> more{{"user", "again"}};
    agent.run(more);
    EXPECT_EQ(agent.usage().total_tokens, 30);
}

// A provider that reports prompt/completion but leaves total at zero — several
// real APIs do. The accumulator normalizes rather than under-reporting.
TEST(UsageAccounting, TotalIsDerivedWhenTheProviderOmitsIt) {
    class PartialUsageProvider : public Provider {
    public:
        ChatCompletion complete(const CompletionParams&) override {
            ChatCompletion c;
            c.message = ChatMessage{"assistant", "ok"};
            c.usage.prompt_tokens     = 7;
            c.usage.completion_tokens = 3;
            c.usage.total_tokens      = 0;   // provider did not fill it in
            return c;
        }
        ChatCompletion complete_stream(const CompletionParams& p,
                                       const StreamCallback&) override {
            return complete(p);
        }
        std::string get_name() const override { return "partial"; }
    };

    auto result = run_graph(llm_graph(1), std::make_shared<PartialUsageProvider>(),
                            "partial");
    EXPECT_EQ(result.usage.total_tokens, 10) << "total should fall back to prompt + completion";
}

// ── Streaming counts too ──
//
// Worth pinning because the usual way to lose it is upstream: OpenAI omits usage
// from a streamed response unless `stream_options: {include_usage: true}` is
// set. Both bundled providers do set it; this asserts the engine does not then
// drop what they hand back.
TEST(UsageAccounting, StreamingRunsCountToo) {
    NodeContext ctx;
    ctx.provider = std::make_shared<UsageProvider>(10, 5);
    auto engine = GraphEngine::compile(llm_graph(1), ctx);

    RunConfig cfg;
    cfg.thread_id = "stream";
    auto result = engine->run_stream(cfg, [](const GraphEvent&) {});

    EXPECT_EQ(result.usage.total_tokens, 15);
}

// ── The contract around a failed run, and its sharp edge ──

namespace {
// Calls the LLM — really spending tokens — and only then decides to fail.
class PaysThenCrashesNode : public GraphNode {
public:
    PaysThenCrashesNode(std::shared_ptr<Provider> p, std::atomic<bool>* fail)
        : provider_(std::move(p)), fail_(fail) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        CompletionParams params;
        params.messages = {{"user", "hi"}};
        auto completion = co_await provider_->invoke(params, nullptr);
        record_usage(in.ctx, completion);              // the tokens are spent

        if (fail_->load()) throw std::runtime_error("crash after paying");

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"done", json(true)});
        co_return out;
    }
    std::string get_name() const override { return "pays_then_crashes"; }

private:
    std::shared_ptr<Provider> provider_;
    std::atomic<bool>*        fail_;
};

json crashy_graph(const std::string& type) {
    return {
        {"name", "crashy"},
        {"channels", {{"done", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"n", {{"type", type}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "n"}},
            {{"from", "n"},         {"to", "__end__"}}
        })}
    };
}
}  // namespace

// RunResult::usage is what THIS call to run() spent. A previous attempt that
// threw never produced a RunResult, so its tokens are not in this one — even
// though they were really spent.
//
// This is the semantics, not a bug, but it is a trap for exactly the person #88
// is for: run 15 tokens, crash, retry 15 tokens, and a cost tracker reading
// RunResult::usage books 15 against a bill of 30. The next test is the way out,
// and the reason this one is written down.
TEST(UsageAccounting, ACrashedAttemptIsInvisibleToRunResult) {
    static std::atomic<bool> fail{true};
    auto provider = std::make_shared<UsageProvider>(10, 5);
    NodeFactory::instance().register_type("pays_then_crashes",
        [provider](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PaysThenCrashesNode>(provider, &fail);
        });

    auto engine = GraphEngine::compile(crashy_graph("pays_then_crashes"), NodeContext{},
                                       std::make_shared<InMemoryCheckpointStore>());
    RunConfig cfg;
    cfg.thread_id = "crash-default";

    fail = true;
    EXPECT_THROW(engine->run(cfg), std::exception);   // 15 tokens spent and lost

    fail = false;
    auto result = engine->run(cfg);                   // 15 more

    EXPECT_EQ(result.usage.total_tokens, 15)
        << "RunResult reports this run, not the whole bill — see the next test";
}

// Supply your own accumulator and it outlives the failed attempt, because it is
// yours and the engine only borrows it. This is what anyone doing real cost
// accounting wants, and RunConfig::usage exists to make it possible.
TEST(UsageAccounting, ACallerSuppliedAccumulatorSeesTheCrashedAttempt) {
    static std::atomic<bool> fail{true};
    auto provider = std::make_shared<UsageProvider>(10, 5);
    NodeFactory::instance().register_type("pays_then_crashes_2",
        [provider](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PaysThenCrashesNode>(provider, &fail);
        });

    auto engine = GraphEngine::compile(crashy_graph("pays_then_crashes_2"), NodeContext{},
                                       std::make_shared<InMemoryCheckpointStore>());

    auto budget = std::make_shared<UsageAccumulator>();
    RunConfig cfg;
    cfg.thread_id = "crash-budget";
    cfg.usage     = budget;

    fail = true;
    EXPECT_THROW(engine->run(cfg), std::exception);

    fail = false;
    auto result = engine->run(cfg);

    EXPECT_EQ(budget->snapshot().total_tokens, 30) << "the crashed attempt's tokens went missing";
    EXPECT_EQ(result.usage.total_tokens, 30)       << "RunResult snapshots the accumulator it was given";
}
