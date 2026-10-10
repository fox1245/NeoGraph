// Provider reports retain nullable uint64 counters; budget commitments are a
// separate conservative ledger carried through graph and subgraph execution.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/llm/agent.h>
#include "fixtures/typed_provider.h"

#include <memory>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>
#include <limits>

#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

class UsageProvider : public neograph::test::LocalProvider {
public:
    UsageProvider(std::uint64_t input, std::uint64_t output)
        : LocalProvider([input, output](ProviderRequest, const PreparedProviderRequest&,
                                        const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
              co_return neograph::test::success("ok", neograph::test::usage(input, output, input + output));
          }, "usage-fixture") {}
};

std::optional<std::uint64_t> reported(const std::optional<sp::Count>& count) {
    return count ? std::optional(count->value) : std::nullopt;
}

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
    ctx.model = "test-model";
    auto engine = GraphEngine::compile(def, ctx);

    RunConfig cfg;
    cfg.thread_id = thread;
    cfg.input = {{"messages", json::array({{{"role", "user"}, {"content", "account this turn"}}})}};
    return engine->run(cfg);
}

}  // namespace

// One LLM node: whatever the provider reported must come back out.
TEST(UsageAccounting, SingleLLMNodeReportsUsage) {
    auto result = run_graph(llm_graph(1),
                            std::make_shared<UsageProvider>(10, 5), "single");

    EXPECT_EQ(reported(result.usage.input_total), 10U);
    EXPECT_EQ(reported(result.usage.output_total), 5U);
    EXPECT_EQ(reported(result.usage.total), 15U);
}

// Two LLM nodes: the run total is the sum, not the last node's.
TEST(UsageAccounting, MultipleLLMNodesSum) {
    auto result = run_graph(llm_graph(2),
                            std::make_shared<UsageProvider>(10, 5), "double");

    EXPECT_EQ(reported(result.usage.total), 30U);
    EXPECT_EQ(reported(result.usage.input_total), 20U);
    EXPECT_EQ(reported(result.usage.output_total), 10U);
}

TEST(UsageAccounting, CachedAndReasoningTokensReachRunResult) {
    auto provider = std::make_shared<neograph::test::LocalProvider>(
        [](ProviderRequest, const PreparedProviderRequest&,
           const neograph::test::LocalProvider::EventCallback&)
            -> asio::awaitable<sp::runtime::Result> {
            auto usage = neograph::test::usage(100, 40, 140);
            usage.input_uncached = sp::Count{26, sp::Evidence::Reported};
            usage.cache_read = sp::Count{64, sp::Evidence::Reported};
            usage.cache_write = sp::Count{10, sp::Evidence::Reported};
            usage.reasoning = sp::Count{25, sp::Evidence::Reported};
            co_return neograph::test::success("ok", std::move(usage));
        }, "detailed-usage-fixture");
    const auto result = run_graph(llm_graph(2), std::move(provider), "detailed");

    EXPECT_EQ(reported(result.usage.input_total), 200U);
    EXPECT_EQ(reported(result.usage.output_total), 80U);
    EXPECT_EQ(reported(result.usage.input_uncached), 52U);
    EXPECT_EQ(reported(result.usage.cache_read), 128U);
    EXPECT_EQ(reported(result.usage.cache_write), 20U);
    EXPECT_EQ(reported(result.usage.reasoning), 50U);
    EXPECT_EQ(reported(result.usage.total), 280U);
}

// No provider report is unknown, not a fabricated known zero.
TEST(UsageAccounting, NoLLMNodeReportsUnknownUsage) {
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

    EXPECT_FALSE(result.usage.total);
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

    EXPECT_EQ(reported(result.usage.total), 15U)
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

TEST(UsageAccounting, WideReportedTotalsRemainLossless) {
    UsageAccumulator accumulator;
    const auto input = std::uint64_t{1} << 40;
    accumulator.add(neograph::test::usage(input, 1, input + 1));
    accumulator.add(neograph::test::usage(input, 2, input + 2));
    EXPECT_EQ(reported(accumulator.snapshot().total), 2 * input + 3);
    EXPECT_EQ(accumulator.total_tokens_wide(), static_cast<long long>(2 * input + 3));
}

TEST(UsageAccounting, UnknownAndInconsistentUsageCannotRenewBudget) {
    UsageAccumulator accumulator;
    ASSERT_TRUE(accumulator.try_reserve(10, 10));
    accumulator.settle_reservation(10, {});
    EXPECT_EQ(accumulator.total_tokens_wide(), 10);
    EXPECT_FALSE(accumulator.snapshot().total);
    EXPECT_FALSE(accumulator.try_reserve(1, 10));
    auto inconsistent = neograph::test::usage(4, 6, 1);
    inconsistent.quality = sp::UsageQuality::Inconsistent;
    accumulator.settle_reservation(10, inconsistent);
    EXPECT_EQ(accumulator.total_tokens_wide(), 10);
    EXPECT_EQ(accumulator.snapshot().quality, sp::UsageQuality::Inconsistent);
    EXPECT_FALSE(accumulator.try_reserve(1, 10));
}

TEST(UsageAccounting, KnownZeroSettlementReleasesOnlyItsReservation) {
    UsageAccumulator accumulator;
    ASSERT_TRUE(accumulator.try_reserve(4, 10));
    ASSERT_TRUE(accumulator.try_reserve(3, 10));
    accumulator.settle_reservation(4, neograph::test::usage(0, 0, 0));
    EXPECT_EQ(reported(accumulator.snapshot().total), 0U);
    EXPECT_EQ(accumulator.total_tokens_wide(), 3);
    EXPECT_FALSE(accumulator.try_reserve(8, 10));
    EXPECT_TRUE(accumulator.try_reserve(7, 10));
}

TEST(UsageAccounting, MixedKnownAndUnknownReportsNeverFabricateTotals) {
    UsageAccumulator accumulator;
    accumulator.add(neograph::test::usage(0, 0, 0));
    ASSERT_TRUE(accumulator.try_reserve(5, 5));
    auto partial = neograph::test::usage(std::nullopt, 2, std::nullopt, sp::UsageStage::Partial);
    partial.output_total->evidence = sp::Evidence::Derived;
    accumulator.settle_reservation(5, partial);
    const auto reports = accumulator.snapshot();
    EXPECT_FALSE(reports.input_total);
    EXPECT_FALSE(reports.total);
    ASSERT_TRUE(reports.output_total);
    EXPECT_EQ(reports.output_total->value, 2U);
    EXPECT_EQ(reports.output_total->evidence, sp::Evidence::Derived);
    EXPECT_EQ(reports.stage, sp::UsageStage::Partial);
    EXPECT_EQ(accumulator.total_tokens_wide(), 5);
    EXPECT_FALSE(accumulator.try_reserve(1, 5));
}

TEST(UsageAccounting, OverspendingSettlementKeepsFullChargeAndSiblingAuthority) {
    UsageAccumulator accumulator;
    ASSERT_TRUE(accumulator.try_reserve(4, 10));
    ASSERT_TRUE(accumulator.try_reserve(3, 10));
    accumulator.settle_reservation(4, neograph::test::usage(0, 8, 8));
    EXPECT_EQ(reported(accumulator.snapshot().total), 8U);
    EXPECT_EQ(accumulator.total_tokens_wide(), 11);
    EXPECT_FALSE(accumulator.try_reserve(1, 11));
    EXPECT_TRUE(accumulator.try_reserve(1, 12));
}

TEST(UsageAccounting, ConcurrentReservationsRespectTheAdmittedCeiling) {
    UsageAccumulator usage;
    constexpr long long ceiling = 100;
    constexpr int        workers = 16;
    std::barrier         ready(workers + 1);
    std::atomic<int>     accepted{0};
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (int i = 0; i != workers; ++i) {
        threads.emplace_back([&] {
            ready.arrive_and_wait();
            if (usage.try_reserve(20, ceiling)) {
                accepted.fetch_add(1, std::memory_order_relaxed);
                usage.settle_reservation(20, neograph::test::usage(0, 20, 20));
            }
        });
    }
    ready.arrive_and_wait();
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(accepted.load(std::memory_order_relaxed), ceiling / 20);
    EXPECT_EQ(usage.total_tokens_wide(), ceiling);
}


// The other way to drive an LLM. Agent is not a graph run and has no
// RunContext, so it keeps its own total — and it has to, or token accounting
// would exist on one of the two paths and not the other, which is the exact
// split #87 was about.
TEST(UsageAccounting, AgentAccumulatesAcrossCalls) {
    neograph::llm::Agent agent(std::make_shared<UsageProvider>(10, 5),
                               std::vector<std::unique_ptr<Tool>>{}, "", "test-model");

    EXPECT_FALSE(agent.usage().total) << "nothing reported yet";

    std::vector<sp::Message> messages{neograph::test::message("hi", sp::Role::User)};
    agent.run(messages);
    EXPECT_EQ(reported(agent.usage().total), 15U);

    // Cumulative over the agent's lifetime, not reset per run: an agent loop
    // makes several calls and the number people want is what the conversation
    // cost, not what the last turn cost.
    std::vector<sp::Message> more{neograph::test::message("again", sp::Role::User)};
    agent.run(more);
    EXPECT_EQ(reported(agent.usage().total), 30U);
}

TEST(UsageAccounting, MissingTotalRemainsUnknownWithKnownComponents) {
    auto provider = std::make_shared<neograph::test::LocalProvider>(
        [](ProviderRequest, const PreparedProviderRequest&,
           const neograph::test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            co_return neograph::test::success("ok", neograph::test::usage(7, 3, std::nullopt));
        });
    auto result = run_graph(llm_graph(1), provider, "partial");
    EXPECT_EQ(reported(result.usage.input_total), 7U);
    EXPECT_EQ(reported(result.usage.output_total), 3U);
    EXPECT_FALSE(result.usage.total);
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
    ctx.model = "test-model";
    auto engine = GraphEngine::compile(llm_graph(1), ctx);

    RunConfig cfg;
    cfg.thread_id = "stream";
    cfg.input = {{"messages", json::array({{{"role", "user"}, {"content", "account this stream"}}})}};
    auto result = engine->run_stream(cfg, [](const GraphEvent&) {});

    EXPECT_EQ(reported(result.usage.total), 15U);
}

// ── The contract around a failed run, and its sharp edge ──

namespace {
// Calls the LLM — really spending tokens — and only then decides to fail.
class PaysThenCrashesNode : public GraphNode {
public:
    PaysThenCrashesNode(std::shared_ptr<Provider> p, std::atomic<bool>* fail)
        : provider_(std::move(p)), fail_(fail) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto completion = co_await provider_->invoke_async(neograph::test::request());
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

    EXPECT_EQ(reported(result.usage.total), 15U)
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

    EXPECT_EQ(reported(budget->snapshot().total), 30U) << "the crashed attempt's tokens went missing";
    EXPECT_EQ(reported(result.usage.total), 30U) << "RunResult snapshots the accumulator it was given";
}

TEST(UsageAccounting, ObservationSealRetainsReportsAndUnknownHoldBeforeEveryRejectedMutation) {
    UsageAccumulator live;
    live.add(neograph::test::usage(2, 3, 5));
    ASSERT_TRUE(live.try_reserve(11, 20));
    ASSERT_TRUE(live.remember_provider_effect("owner:source:effect-1"));
    UsageAccumulator replay;
    replay.restore_authority(live.authority_snapshot());
    replay.seal_observation();

    const auto zero = neograph::test::usage(0, 0, 0);
    EXPECT_THROW(replay.remember_provider_effect("owner:replay:fresh-effect"), std::logic_error);
    EXPECT_THROW(replay.observe(zero), std::logic_error);
    EXPECT_THROW(replay.add(zero), std::logic_error);
    EXPECT_THROW(replay.try_reserve(1, 20), std::logic_error);
    EXPECT_THROW(replay.release_reservation(11), std::logic_error);
    EXPECT_THROW(replay.settle_reservation(11, zero), std::logic_error);
    EXPECT_THROW(replay.restore_charge(1), std::logic_error);
    EXPECT_THROW(replay.restore_reservation(1), std::logic_error);
    EXPECT_THROW(replay.restore_authority({}), std::logic_error);

    const auto authority = replay.authority_snapshot();
    EXPECT_EQ(authority.charged, 5U);
    EXPECT_EQ(authority.reserved, 11U);
    EXPECT_EQ(authority.provider_effects,
              std::vector<std::string>({"owner:source:effect-1"}));
    EXPECT_TRUE(authority.has_report);
    EXPECT_EQ(reported(replay.snapshot().input_total), 2U);
    EXPECT_EQ(reported(replay.snapshot().output_total), 3U);
    EXPECT_EQ(reported(replay.snapshot().total), 5U);
    EXPECT_EQ(replay.total_tokens_wide(), 16U);

    // The observation seal belongs to the hydrated replay bank, not its live source.
    EXPECT_TRUE(live.try_reserve(4, 20));
    EXPECT_EQ(live.total_tokens_wide(), 20U);
    EXPECT_EQ(replay.total_tokens_wide(), 16U);
}

TEST(UsageAccounting, ObservationSealRejectsRestorationAndZeroClaimsOnAPristineBank) {
    UsageAccumulator replay;
    replay.seal_observation();
    UsageAccumulator::AuthoritySnapshot forged;
    forged.reserved = 8;
    forged.provider_effects = {"owner:forged"};
    EXPECT_THROW(replay.restore_authority(std::move(forged)), std::logic_error);
    EXPECT_THROW(replay.try_reserve(0, 0), std::logic_error);
    const auto authority = replay.authority_snapshot();
    EXPECT_EQ(authority.charged, 0U);
    EXPECT_EQ(authority.reserved, 0U);
    EXPECT_TRUE(authority.provider_effects.empty());
    EXPECT_FALSE(authority.has_report);
    EXPECT_FALSE(replay.snapshot().total);
}

TEST(UsageAccounting, MultiReportTokenBankDoesNotRetainOrInventMonetaryAggregate) {
    UsageAccumulator bank;
    auto first = neograph::test::usage(2, 3, 5);
    first.provider_cost.total = sp::UsdAmount{0};
    first.provider_cost.status[0] = sp::CostStatus::Available;
    first.provider_cost.is_byok = false;
    first.provider_cost.byok_status = sp::CostStatus::Available;
    first.provider_cost.source = sp::CostSource::OpenRouterUsd;
    const auto original = provider_codec::encode_usage(first);

    bank.add(first);
    EXPECT_EQ(bank.total_tokens_wide(), 5U);
    ASSERT_TRUE(bank.snapshot().provider_cost.total);
    EXPECT_EQ(bank.snapshot().provider_cost.total->nano_usd, 0U);
    EXPECT_EQ(bank.snapshot().provider_cost.is_byok, std::optional<bool>(false));

    bank.add(neograph::test::usage(7, 11, 18));
    const auto aggregate = bank.snapshot();
    EXPECT_EQ(reported(aggregate.total), 23U);
    EXPECT_EQ(bank.total_tokens_wide(), 23U);
    EXPECT_TRUE(provider_codec::provider_cost_absent(aggregate.provider_cost));
    EXPECT_FALSE(provider_codec::encode_usage(aggregate).contains("provider_cost"));

    // Neither another report with cost nor restoration of the token-only
    // aggregate resurrects the first report's metadata as a monetary total.
    bank.add(first);
    EXPECT_EQ(bank.total_tokens_wide(), 28U);
    EXPECT_TRUE(provider_codec::provider_cost_absent(bank.snapshot().provider_cost));
    UsageAccumulator restored;
    restored.restore_authority(bank.authority_snapshot());
    EXPECT_EQ(restored.total_tokens_wide(), 28U);
    EXPECT_TRUE(provider_codec::provider_cost_absent(restored.snapshot().provider_cost));
    EXPECT_EQ(provider_codec::encode_usage(first), original);
    EXPECT_TRUE(original.contains("provider_cost"));
}

TEST(UsageAccounting, MultiReportMoneyClearingDoesNotSettleAnUnknownTokenHold) {
    for (const auto status : {sp::CostStatus::Missing, sp::CostStatus::Malformed,
                             sp::CostStatus::Overflow, sp::CostStatus::UnknownCurrency,
                             sp::CostStatus::Conflict}) {
        UsageAccumulator bank;
        auto first = neograph::test::usage(2, 3, 5);
        first.provider_cost.total = sp::UsdAmount{100};
        first.provider_cost.status[0] = sp::CostStatus::Available;
        first.provider_cost.source = sp::CostSource::OpenRouterUsd;
        bank.add(first);
        ASSERT_TRUE(bank.try_reserve(13, 18));

        sp::Usage partial;
        partial.stage = sp::UsageStage::Partial;
        partial.provider_cost.status[0] = status;
        if (status != sp::CostStatus::Missing) {
            partial.provider_cost.source = status == sp::CostStatus::UnknownCurrency
                ? sp::CostSource::UnknownCurrency : sp::CostSource::OpenRouterUsd;
            partial.provider_cost.quality = sp::UsageQuality::Inconsistent;
        }
        const auto observed = provider_codec::encode_usage(partial);
        bank.settle_reservation(13, partial);
        const auto authority = bank.authority_snapshot();
        EXPECT_EQ(authority.charged, 5U);
        EXPECT_EQ(authority.reserved, 13U);
        EXPECT_EQ(bank.total_tokens_wide(), 18U);
        EXPECT_TRUE(provider_codec::provider_cost_absent(authority.reports.provider_cost));
        EXPECT_EQ(provider_codec::encode_usage(partial), observed);
    }
}
