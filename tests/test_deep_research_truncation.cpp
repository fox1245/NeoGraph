#include "fixtures/unique_temp_dir.h"

#include <gtest/gtest.h>
#include <neograph/graph/deep_research_graph.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/run_context.h>
#include <neograph/async/run_sync.h>
#ifdef NEOGRAPH_TESTS_HAVE_PROGRAM_SQLITE
#include <neograph/program/sqlite_provider_call_broker.h>
#endif
#include "fixtures/typed_provider.h"

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>

using namespace neograph;
namespace {
enum class Role { Brief, Supervisor, Researcher, Compress, FinalReport };
std::string message_text(const sp::Message& message) {
    std::string text;
    for (const auto& part : message.parts)
        if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
    return text;
}
Role classify(const sp::chat::Request& request) {
    const auto system = message_text(request.canonical_messages.front());
    if (system.find("research brief") != std::string::npos && system.find("convert") != std::string::npos) return Role::Brief;
    if (system.find("lead research supervisor") != std::string::npos) return Role::Supervisor;
    if (system.find("focused researcher") != std::string::npos) return Role::Researcher;
    if (system.find("Compress raw research notes") != std::string::npos) return Role::Compress;
    return Role::FinalReport;
}
sp::runtime::Result reply(std::string text, sp::StopKind stop = sp::StopKind::EndTurn,
                          std::vector<sp::ToolCall> calls = {}) {
    sp::Completion completion;
    auto message = test::message(std::move(text));
    for (auto& call : calls) message.parts.emplace_back(std::move(call));
    completion.messages.push_back(std::move(message));
    completion.stop = {stop, stop == sp::StopKind::MaxTokens ? "length" : "stop"};
    completion.usage = test::usage(2, 3, 5);
    completion.usage.cache_read = sp::Count{1};
    completion.usage.reasoning = sp::Count{2};
    return std::make_shared<const sp::Outcome>(std::move(completion));
}
sp::ToolCall call(std::string id, std::string name, std::string arguments) {
    sp::ToolCall result;
    result.id = std::move(id);
    result.name = std::move(name);
    result.input = test::document(arguments);
    return result;
}
struct Script {
    std::mutex mutex;
    std::array<std::deque<sp::runtime::Result>, 5> replies;
    std::array<std::vector<std::uint64_t>, 5> caps;
    std::array<std::string, 5> last_user;
    std::vector<std::optional<std::chrono::steady_clock::time_point>> deadlines;
    std::vector<std::string> models;
    std::vector<std::optional<double>> temperatures;
    std::vector<std::optional<std::string>> efforts;
    bool throw_observer = false;
    bool throw_settlement = false;
    bool emit_output = false;
    std::shared_ptr<graph::CancelToken> cancel_at_final;
    void set(Role role, std::initializer_list<sp::runtime::Result> values) {
        replies[static_cast<size_t>(role)] = values;
    }
    const auto& budgets(Role role) const { return caps[static_cast<size_t>(role)]; }
};
struct Scenario {
    std::shared_ptr<Script> script = std::make_shared<Script>();
    std::shared_ptr<test::LocalProvider> provider;
    std::unique_ptr<graph::GraphEngine> engine;
    graph::RunConfig config;
    graph::RunMetadata metadata;
    explicit Scenario(std::shared_ptr<sp::runtime::Client> client = test::client(),
                      std::function<void(graph::DeepResearchConfig&)> configure = {}) {
        script->set(Role::Brief, {reply("brief text")});
        script->set(Role::Supervisor, {
            reply("", sp::StopKind::ToolUse, {call("c1", "conduct_research", R"({"research_topic":"topic A"})")}),
            reply("", sp::StopKind::ToolUse, {call("c2", "research_complete", "{}")})});
        script->set(Role::Researcher, {reply("raw findings")});
        script->set(Role::Compress, {reply("COMPRESSED-A")});
        script->set(Role::FinalReport, {reply("## report")});
        provider = std::make_shared<test::LocalProvider>([state = script](ProviderRequest request, const auto&, const auto& on_event)
            -> asio::awaitable<sp::runtime::Result> {
            std::lock_guard lock(state->mutex);
            const auto& payload = std::get<sp::chat::Request>(request.payload);
            const auto role = classify(payload);
            const auto index = static_cast<size_t>(role);
            state->caps[index].push_back(payload.max_output_tokens.value_or(0));
            state->last_user[index] = message_text(payload.canonical_messages.back());
            state->deadlines.push_back(request.options.deadline);
            state->models.push_back(payload.model);
            state->temperatures.push_back(payload.temperature);
            state->efforts.push_back(payload.reasoning_effort);
            auto& queue = state->replies[index];
            if (queue.empty()) throw std::logic_error("unscripted research role");
            const auto result = queue.front();
            if (queue.size() > 1) queue.pop_front();
            if (role == Role::FinalReport) {
                if (state->cancel_at_final) state->cancel_at_final->cancel();
                if (state->throw_observer && on_event) {
                    // Local providers must retain the produced outcome when
                    // delivery fails; the SDK bridge performs its own drain.
                    try { on_event(sp::Stop{test::completion(result).stop}); }
                    catch (...) { throw ProviderObserverError(result, std::current_exception()); }
                }
                if (state->throw_settlement)
                    throw ProviderBudgetSettlementError(result,
                        std::make_exception_ptr(std::runtime_error("token context settlement failure")));
                if (state->emit_output && on_event)
                    on_event(sp::PartBegin{sp::LocalId{1}, sp::LocalId{1}, sp::PartKind::Thinking});
            }
            co_return result;
        }, "research-script", std::move(client));
        graph::DeepResearchConfig settings;
        settings.model = "test-model";
        if (configure) configure(settings);
        engine = graph::create_deep_research_graph(provider, {}, settings);
        config.thread_id = "research-thread";
        config.input = {{"user_query", "q"}};
        config.max_steps = 40;
        config.usage = std::make_shared<UsageAccumulator>();
        config.provider_outcomes = std::make_shared<graph::ProviderOutcomes>();
        metadata.run_id = "research-run";
        metadata.owner_scope = "research-owner";
        metadata.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
    graph::RunResult run() { return engine->run(config, metadata); }
};
std::string report_of(const graph::RunResult& result) {
    return result.output.at("channels").at("final_report").at("value").get<std::string>();
}
}

TEST(DeepResearchTruncation, FinalReportRetriesEmptyTruncationWithLargerBudget) {
    Scenario scenario;
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens), reply("## Real report")});
    const auto result = scenario.run();
    EXPECT_EQ(report_of(result), "## Real report");
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096, 8192}));
    ASSERT_EQ(result.provider_outcomes.size(), 7u);
    EXPECT_EQ(result.usage.total->value, 35u);
    EXPECT_EQ(result.usage.cache_read->value, 7u);
    EXPECT_EQ(result.usage.reasoning->value, 14u);
    for (const auto& deadline : scenario.script->deadlines) EXPECT_EQ(deadline, scenario.metadata.deadline);
}

TEST(DeepResearchTruncation, FinalReportEmptyAfterRetriesCannotRestartNodeOrPublishReport) {
    Scenario scenario;
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens)});
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096, 8192, 16384}));
    EXPECT_EQ(scenario.config.provider_outcomes->values.size(), 8u);
    EXPECT_EQ(scenario.config.usage->snapshot().total->value, 40u);
}

TEST(DeepResearchTruncation, PartialTruncatedReportIsFlaggedWithoutMutatingOwnedOutcome) {
    Scenario scenario;
    scenario.script->set(Role::FinalReport, {reply("## Partial", sp::StopKind::MaxTokens)});
    const auto result = scenario.run();
    const auto report = report_of(result);
    EXPECT_EQ(report.rfind("## Partial", 0), 0u);
    EXPECT_NE(report.find("Incomplete"), std::string::npos);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096}));
    EXPECT_EQ(outcome_text(*result.provider_outcomes.back()), "## Partial");
}

TEST(DeepResearchTruncation, CompressedFindingsRecoveryFeedsFinalReport) {
    Scenario scenario;
    scenario.script->set(Role::Compress, {reply("", sp::StopKind::MaxTokens), reply("COMPRESSED-A")});
    EXPECT_EQ(report_of(scenario.run()), "## report");
    EXPECT_EQ(scenario.script->budgets(Role::Compress), (std::vector<std::uint64_t>{2048, 4096}));
    EXPECT_NE(scenario.script->last_user[static_cast<size_t>(Role::FinalReport)].find("COMPRESSED-A"), std::string::npos);
}

TEST(DeepResearchTruncation, ExhaustedCompressionLeavesDiagnosticNotFabricatedOutcome) {
    Scenario scenario;
    scenario.script->set(Role::Compress, {reply("", sp::StopKind::MaxTokens)});
    const auto result = scenario.run();
    EXPECT_EQ(scenario.script->budgets(Role::Compress), (std::vector<std::uint64_t>{2048, 4096, 8192}));
    EXPECT_NE(scenario.script->last_user[static_cast<size_t>(Role::FinalReport)].find("compression produced no visible findings"), std::string::npos);
    for (const auto& outcome : result.provider_outcomes)
        EXPECT_EQ(outcome_text(*outcome).find("compression produced no visible findings"), std::string::npos);
}

TEST(DeepResearchTruncation, SupervisorAndResearcherRecoverOnlyEmptyCompletedTruncation) {
    Scenario scenario;
    scenario.script->replies[static_cast<size_t>(Role::Supervisor)].push_front(reply("", sp::StopKind::MaxTokens));
    scenario.script->set(Role::Researcher, {reply("", sp::StopKind::MaxTokens), reply("raw recovered findings")});
    EXPECT_EQ(report_of(scenario.run()), "## report");
    EXPECT_EQ(scenario.script->budgets(Role::Supervisor), (std::vector<std::uint64_t>{2048, 4096, 2048}));
    EXPECT_EQ(scenario.script->budgets(Role::Researcher), (std::vector<std::uint64_t>{2048, 4096}));
    EXPECT_NE(scenario.script->last_user[static_cast<size_t>(Role::Compress)].find("raw recovered findings"), std::string::npos);
}

TEST(DeepResearchTruncation, TypedFailureDoesNotRedispatch) {
    Scenario scenario;
    sp::PartialCompletion partial;
    partial.usage = test::usage(2, std::nullopt, std::nullopt, sp::UsageStage::Partial);
    scenario.script->set(Role::FinalReport, {test::failure(sp::ErrorKind::DeadlineExceeded, partial)});
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096}));
    EXPECT_TRUE(std::holds_alternative<sp::Failure>(*scenario.config.provider_outcomes->values.back()));
}

TEST(DeepResearchTruncation, ObserverFailureDoesNotRedispatchAndRetainsUsage) {
    Scenario scenario;
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens)});
    scenario.script->throw_observer = true;
    scenario.config.on_provider_event = [](const sp::Event&) {
        throw std::runtime_error("token context observer failure");
    };
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096}));
    const auto report = scenario.config.usage->snapshot();
    ASSERT_TRUE(report.total);
    EXPECT_EQ(report.total->value, 30u);
    EXPECT_EQ(scenario.config.provider_outcomes->values.size(), 6u);
}

TEST(DeepResearchTruncation, StreamedSemanticOutputDoesNotRedispatch) {
    Scenario scenario;
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens)});
    scenario.script->emit_output = true;
    int delivered = 0;
    scenario.config.on_provider_event = [&](const sp::Event&) { ++delivered; };
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096}));
}

TEST(DeepResearchTruncation, CancellationBetweenBudgetAttemptsDoesNotDispatchAgain) {
    Scenario scenario;
    scenario.config.cancel_token = std::make_shared<graph::CancelToken>();
    scenario.script->cancel_at_final = scenario.config.cancel_token;
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens)});
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096}));
}

#ifdef NEOGRAPH_TESTS_HAVE_PROGRAM_SQLITE
TEST(DeepResearchTruncation, DurableBrokerReplaysEachDistinctBudgetSlotWithoutRedispatch) {
    Scenario scenario;
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens), reply("## durable report")});
    const auto path = neograph::graph::test_support::unique_temp_path("neograph-research-slots-", ".sqlite");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove(path.string() + "-wal", error);
            std::filesystem::remove(path.string() + "-shm", error);
        }
    } cleanup{path};
    using Journal = program::SQLiteProgramProviderCallJournal;
    const program::ProgramCoreProviderCallContext context{
        "research-owner", "research-version", "research-run", "root", 1};
    const std::string binding = "sha256:" + std::string(64, 'a');
    {
        Journal journal(path.string());
        graph::RunResources resources;
        resources.provider_call_broker = journal.bind(context, binding).broker;
        const auto result = async::run_sync(scenario.engine->run_async(scenario.config, scenario.metadata, resources));
        EXPECT_EQ(report_of(result), "## durable report");
        EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096, 8192}));
    }
    Journal reopened(path.string());
    graph::RunResources resources;
    resources.provider_call_broker = reopened.bind(context, binding).broker;
    // A fresh graph starts at the same deterministic task slots. Request-bound
    // replay must return both outcomes, not conflict or run either cap again.
    graph::DeepResearchConfig settings;
    settings.model = "test-model";
    auto replay_engine = graph::create_deep_research_graph(scenario.provider, {}, settings);
    const auto result = async::run_sync(replay_engine->run_async(scenario.config, scenario.metadata, resources));
    EXPECT_EQ(report_of(result), "## durable report");
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096, 8192}));
    EXPECT_EQ(scenario.script->budgets(Role::Researcher), (std::vector<std::uint64_t>{2048}));
}
#endif

TEST(DeepResearchTruncation, IncreasedCapCannotRenewOriginalSharedBank) {
    Scenario scenario(test::bounded_client("test-model", 1000));
    scenario.config.model_token_budget = 7500;
    scenario.config.budget_exhausted = std::make_shared<std::atomic_bool>(false);
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens), reply("not admitted")});
    const auto original_bank = scenario.config.usage;
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(scenario.config.usage, original_bank);
    EXPECT_TRUE(scenario.config.budget_exhausted->load());
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096}));
    ASSERT_FALSE(scenario.config.provider_outcomes->values.empty());
    const auto* failure = std::get_if<sp::Failure>(scenario.config.provider_outcomes->values.back().get());
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->error.kind, sp::ErrorKind::QuotaExhausted);
    const auto report = original_bank->snapshot();
    ASSERT_TRUE(report.total);
    EXPECT_EQ(report.total->value, 30u);
}

TEST(DeepResearchTruncation, SettlementFailureRetainsOutcomeWithoutFreshEffect) {
    Scenario scenario;
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens)});
    scenario.script->throw_settlement = true;
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096}));
    EXPECT_EQ(scenario.config.usage->snapshot().total->value, 30u);
    EXPECT_EQ(scenario.config.provider_outcomes->values.size(), 6u);
}

TEST(DeepResearchTruncation, EmptyEndTurnCannotPublishReportOrEnterCapLadder) {
    Scenario scenario;
    scenario.script->set(Role::FinalReport, {reply("")});
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096}));
}

TEST(DeepResearchTruncation, ToolCallWithMaxTokensIsNotAnEmptyCompletionRetry) {
    Scenario scenario;
    scenario.script->set(Role::Supervisor, {
        reply("", sp::StopKind::MaxTokens, {call("c1", "conduct_research", R"({"research_topic":"topic A"})")}),
        reply("", sp::StopKind::ToolUse, {call("c2", "research_complete", "{}")})});
    EXPECT_EQ(report_of(scenario.run()), "## report");
    EXPECT_EQ(scenario.script->budgets(Role::Supervisor), (std::vector<std::uint64_t>{2048, 2048}));
}

TEST(DeepResearchTruncation, ImplicitProviderDeadlineIsPinnedAcrossBudgetAttempts) {
    Scenario scenario;
    scenario.metadata.deadline.reset();
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens), reply("## report")});
    EXPECT_EQ(report_of(scenario.run()), "## report");
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096, 8192}));
    const auto& deadlines = scenario.script->deadlines;
    ASSERT_EQ(deadlines.size(), 7u);
    ASSERT_TRUE(deadlines[5]);
    EXPECT_EQ(deadlines[5], deadlines[6]);
}

TEST(DeepResearchSettings, ModelIsRequired) {
    Scenario scenario;
    EXPECT_THROW(graph::create_deep_research_graph(scenario.provider, {}, {}), std::invalid_argument);
}

TEST(DeepResearchSettings, StepsSendOnlyTheCallersControls) {
    Scenario plain;
    EXPECT_EQ(report_of(plain.run()), "## report");
    ASSERT_FALSE(plain.script->temperatures.empty());
    for (const auto& temperature : plain.script->temperatures) EXPECT_FALSE(temperature);
    for (const auto& model : plain.script->models) EXPECT_EQ(model, "test-model");

    Scenario tuned(test::client(), [](graph::DeepResearchConfig& settings) {
        settings.model = "other-model";
        settings.controls.reasoning_effort = "low";
        settings.controls.max_output_tokens = 7;  // ignored: the step caps apply
        settings.brief_max_output_tokens = 300;
        settings.supervisor_max_output_tokens = 1000;
        settings.researcher_max_output_tokens = 1100;
        settings.compress_max_output_tokens = 1200;
        settings.report_max_output_tokens = 1300;
    });
    EXPECT_EQ(report_of(tuned.run()), "## report");
    for (const auto& model : tuned.script->models) EXPECT_EQ(model, "other-model");
    for (const auto& effort : tuned.script->efforts) EXPECT_EQ(effort, std::optional<std::string>("low"));
    for (const auto& temperature : tuned.script->temperatures) EXPECT_FALSE(temperature);
    EXPECT_EQ(tuned.script->budgets(Role::Brief), (std::vector<std::uint64_t>{300}));
    EXPECT_EQ(tuned.script->budgets(Role::Supervisor), (std::vector<std::uint64_t>{1000, 1000}));
    EXPECT_EQ(tuned.script->budgets(Role::Researcher), (std::vector<std::uint64_t>{1100}));
    EXPECT_EQ(tuned.script->budgets(Role::Compress), (std::vector<std::uint64_t>{1200}));
    EXPECT_EQ(tuned.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{1300}));
}

TEST(DeepResearchSettings, OutputBudgetBoundsTheEmptyAnswerRetry) {
    Scenario scenario(test::client(), [](graph::DeepResearchConfig& settings) { settings.max_output_budget = 6000; });
    scenario.script->set(Role::FinalReport, {reply("", sp::StopKind::MaxTokens)});
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_EQ(scenario.script->budgets(Role::FinalReport), (std::vector<std::uint64_t>{4096, 6000}));
}

TEST(DeepResearchSettings, BriefFailureFailsTheRun) {
    Scenario scenario(test::client(), [](graph::DeepResearchConfig& settings) { settings.retry.max_retries = 0; });
    scenario.script->set(Role::Brief, {test::failure(sp::ErrorKind::InvalidRequest)});
    EXPECT_THROW(scenario.run(), std::exception);
    EXPECT_TRUE(scenario.script->budgets(Role::Supervisor).empty());
}
