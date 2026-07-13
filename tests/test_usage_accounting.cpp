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
