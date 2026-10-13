// #443: a run that goes over a budget ends with a failure record that names the
// budget and says how far the run went over it. The ending of a run whose plan
// completed is decided from its usage and its grant, so each budget path is
// checked here without a clock or a provider; the runtime tests next to the
// code that produces the usage (wall time, model tokens, money, Core steps)
// check the same records end to end.

#include "budget_overrun.h"

#include <neograph/graph/types.h>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using neograph::json;
using neograph::program::ProgramTerminalStatus;
using neograph::program::ProgramUsage;
using neograph::program::RunBudget;
using neograph::program::detail::budget_witness;
using neograph::program::detail::BudgetOverrun;
using neograph::program::detail::complete_budget_record;
using neograph::program::detail::completed_run_overrun;
using neograph::program::detail::wall_time_failure;

RunBudget grant() {
    RunBudget budget;
    budget.wall_time_ms        = 1'000;
    budget.model_tokens        = 500;
    budget.monetary_microunits = 2'000;
    budget.max_core_steps      = 8;
    return budget;
}

// A run that used exactly what it was granted.
ProgramUsage at_the_grant() {
    ProgramUsage usage;
    usage.wall_time_ms        = 1'000;
    usage.model_tokens        = 500;
    usage.monetary_microunits = 2'000;
    usage.core_steps          = 8;
    return usage;
}

std::optional<BudgetOverrun> overrun(const ProgramUsage& usage,
                                     bool                money_exceeded     = false,
                                     bool                check_model_tokens = true) {
    return completed_run_overrun(grant(), usage, check_model_tokens, money_exceeded);
}

}  // namespace

TEST(ProgramBudgetOverrun, TheWitnessSaysGrantedUsedAndOverBy) {
    EXPECT_EQ(budget_witness(10, 14), (json{{"granted", 10}, {"used", 14}, {"over_by", 4}}));
    EXPECT_EQ(budget_witness(10, 10), (json{{"granted", 10}, {"used", 10}, {"over_by", 0}}));
    // A run stopped before it used its grant is not over it.
    EXPECT_EQ(budget_witness(10, 6), (json{{"granted", 10}, {"used", 6}, {"over_by", 0}}));
}

TEST(ProgramBudgetOverrun, ARunWithinEveryBudgetIsNotOverAny) {
    EXPECT_FALSE(overrun(ProgramUsage{}));
    // Using the whole grant is within it; the grant is the amount allowed.
    EXPECT_FALSE(overrun(at_the_grant()));
}

TEST(ProgramBudgetOverrun, WallTimeEndsTimedOutWithTheDeadlinesCode) {
    auto usage         = at_the_grant();
    usage.wall_time_ms = 1'037;
    const auto ending  = overrun(usage);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->status, ProgramTerminalStatus::TimedOut);
    EXPECT_EQ(ending->failure.code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(ending->failure.message, "Program wall-time budget (1000 ms) expired");
    EXPECT_EQ(ending->failure.witness,
              (json{{"wall_time_ms", {{"granted", 1'000}, {"used", 1'037}, {"over_by", 37}}}}));
    // The record is the one the deadline path builds, so the two cannot drift.
    EXPECT_EQ(ending->failure, wall_time_failure(1'000, 1'037));
}

TEST(ProgramBudgetOverrun, ModelTokensEndBudgetExhaustedWithTheClassifiersCode) {
    auto usage         = at_the_grant();
    usage.model_tokens = 620;
    const auto ending  = overrun(usage);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->status, ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(ending->failure.code,
              neograph::graph::node_failure_code(
                  neograph::graph::NodeFailureClass::BudgetExhausted));
    EXPECT_EQ(ending->failure.code, "NG.BudgetExhausted");
    EXPECT_EQ(ending->failure.message,
              "Model-token budget exceeded by the last reply: 620 tokens used of 500");
    EXPECT_EQ(ending->failure.witness,
              (json{{"model_tokens", {{"granted", 500}, {"used", 620}, {"over_by", 120}}}}));
}

TEST(ProgramBudgetOverrun, AGrantOfNoModelTokensAllowsNone) {
    auto budget          = grant();
    budget.model_tokens  = 0;
    auto usage           = at_the_grant();
    usage.model_tokens   = 1;
    const auto ending    = completed_run_overrun(budget, usage, true, false);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->failure.code, "NG.BudgetExhausted");
    EXPECT_EQ(ending->failure.witness,
              (json{{"model_tokens", {{"granted", 0}, {"used", 1}, {"over_by", 1}}}}));
}

TEST(ProgramBudgetOverrun, ARecordedReplayDoesNotSpendModelTokens) {
    auto usage         = at_the_grant();
    usage.model_tokens = 620;
    EXPECT_FALSE(overrun(usage, false, /*check_model_tokens=*/false));
}

TEST(ProgramBudgetOverrun, MoneyEndsBudgetExhaustedWithTheMoneyBudgetCode) {
    auto usage                = at_the_grant();
    usage.monetary_microunits = 2'010;
    const auto ending         = overrun(usage, /*money_exceeded=*/true);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->status, ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(ending->failure.code, "P_MONEY_BUDGET");
    EXPECT_EQ(ending->failure.message, "Monetary budget exceeded: 2010 microunits charged of 2000");
    EXPECT_EQ(ending->failure.witness,
              (json{{"monetary", {{"granted", 2'000}, {"used", 2'010}, {"over_by", 10}}}}));
}

TEST(ProgramBudgetOverrun, TheMonetaryAccountDecidesWhetherMoneyIsExceeded) {
    // The usage alone does not say: a run whose account is within its ceiling
    // is not over it, and one the account reports over it is.
    auto usage                = at_the_grant();
    usage.monetary_microunits = 5'000;
    EXPECT_FALSE(overrun(usage, /*money_exceeded=*/false));
    EXPECT_TRUE(overrun(usage, /*money_exceeded=*/true));
}

TEST(ProgramBudgetOverrun, CoreStepsEndBudgetExhaustedWithTheCoreStepCode) {
    auto usage        = at_the_grant();
    usage.core_steps  = 11;
    const auto ending = overrun(usage);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->status, ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(ending->failure.code, "P_CORE_STEP_BUDGET");
    EXPECT_EQ(ending->failure.message, "Core step budget exceeded: 11 steps run of 8");
    EXPECT_EQ(ending->failure.witness,
              (json{{"core_steps", {{"granted", 8}, {"used", 11}, {"over_by", 3}}}}));
}

TEST(ProgramBudgetOverrun, EveryRecordNamesTheRunsRootOperation) {
    std::vector<ProgramUsage> overs(4, at_the_grant());
    overs[0].wall_time_ms        = 1'001;
    overs[1].model_tokens        = 501;
    overs[2].monetary_microunits = 2'001;
    overs[3].core_steps          = 9;
    for (const auto& usage : overs) {
        const auto ending = overrun(usage, usage.monetary_microunits > 2'000);
        ASSERT_TRUE(ending);
        EXPECT_EQ(ending->failure.operation_id, "root");
        EXPECT_TRUE(ending->failure.witness.is_object());
        EXPECT_EQ(ending->failure.witness.size(), 1U);
    }
}

// Several budgets at once: the witness holds every budget that was exceeded,
// and the status and the code are those of the first of them in the order of
// the RunBudget fields: wall time, model tokens, money, Core steps.
TEST(ProgramBudgetOverrun, SeveralBudgetsAreAllInTheWitnessAndTheFirstGivesTheCode) {
    ProgramUsage usage;
    usage.wall_time_ms        = 1'200;
    usage.model_tokens        = 900;
    usage.monetary_microunits = 2'500;
    usage.core_steps          = 12;
    const json wall{{"granted", 1'000}, {"used", 1'200}, {"over_by", 200}};
    const json tokens{{"granted", 500}, {"used", 900}, {"over_by", 400}};
    const json money{{"granted", 2'000}, {"used", 2'500}, {"over_by", 500}};
    const json steps{{"granted", 8}, {"used", 12}, {"over_by", 4}};

    auto ending = overrun(usage, true);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->status, ProgramTerminalStatus::TimedOut);
    EXPECT_EQ(ending->failure.code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(ending->failure.witness,
              (json{{"wall_time_ms", wall}, {"model_tokens", tokens}, {"monetary", money},
                    {"core_steps", steps}}));

    usage.wall_time_ms = 1'000;
    ending             = overrun(usage, true);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->status, ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(ending->failure.code, "NG.BudgetExhausted");
    EXPECT_EQ(ending->failure.witness,
              (json{{"model_tokens", tokens}, {"monetary", money}, {"core_steps", steps}}));

    usage.model_tokens = 500;
    ending             = overrun(usage, true);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->status, ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(ending->failure.code, "P_MONEY_BUDGET");
    EXPECT_EQ(ending->failure.witness, (json{{"monetary", money}, {"core_steps", steps}}));

    usage.monetary_microunits = 2'000;
    ending                    = overrun(usage, false);
    ASSERT_TRUE(ending);
    EXPECT_EQ(ending->status, ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(ending->failure.code, "P_CORE_STEP_BUDGET");
    EXPECT_EQ(ending->failure.witness, (json{{"core_steps", steps}}));
}

// A run that a budget stopped on the way has its record completed at the end:
// the Core-step record says how the run stands against its grant, and a budget
// that the run is also over is added without touching what the record has.
TEST(ProgramBudgetOverrun, ARecordFromTheWayGainsTheCoreStepEntryAndTheOtherExceededBudgets) {
    const auto stopped = [](std::string code) {
        return neograph::program::ProgramFailure{std::move(code), "stopped", "root", "", 0,
                                                 json::object()};
    };
    auto usage       = at_the_grant();
    usage.core_steps = 8;  // stopped at the grant: used all of it, over by nothing

    auto step_limit = stopped("P_CORE_STEP_BUDGET");
    complete_budget_record(step_limit, grant(), usage, true, false);
    EXPECT_EQ(step_limit.witness,
              (json{{"core_steps", {{"granted", 8}, {"used", 8}, {"over_by", 0}}}}));

    // A timeout that was not built by the deadline path (a native capability
    // that ran past it) gets the entry, and one that has it keeps it.
    usage.wall_time_ms = 1'004;
    auto native        = stopped("P_RUNTIME_TIMEOUT");
    complete_budget_record(native, grant(), usage, true, false);
    EXPECT_EQ(native.witness,
              (json{{"wall_time_ms", {{"granted", 1'000}, {"used", 1'004}, {"over_by", 4}}}}));
    usage.wall_time_ms = 1'000;

    // The deadline's own entry, and the exceeded tokens and money next to it.
    usage.model_tokens        = 520;
    usage.monetary_microunits = 2'100;
    auto deadline             = stopped("P_RUNTIME_TIMEOUT");
    deadline.witness =
        json{{"wall_time_ms", {{"granted", 1'000}, {"used", 1'003}, {"over_by", 3}}}};
    complete_budget_record(deadline, grant(), usage, true, true);
    EXPECT_EQ(deadline.witness,
              (json{{"wall_time_ms", {{"granted", 1'000}, {"used", 1'003}, {"over_by", 3}}},
                    {"model_tokens", {{"granted", 500}, {"used", 520}, {"over_by", 20}}},
                    {"monetary", {{"granted", 2'000}, {"used", 2'100}, {"over_by", 100}}}}));

    // A refused priced call keeps its own `monetary` evidence.
    auto refused     = stopped("P_MONEY_BUDGET");
    refused.witness  = json{{"monetary", {{"ceiling", 2'000}, {"used", 1'900}}}};
    complete_budget_record(refused, grant(), usage, true, true);
    EXPECT_EQ(refused.witness["monetary"], (json{{"ceiling", 2'000}, {"used", 1'900}}));

    // Another failure is not a budget record and is left alone.
    auto other = stopped("P_RUNTIME_CORE_FAILURE");
    complete_budget_record(other, grant(), usage, true, true);
    EXPECT_EQ(other.witness, json::object());
}

TEST(ProgramBudgetOverrun, TheDeadlinePathAddsTheWitnessToWhatItKnows) {
    const auto failure = wall_time_failure(400, 812, json{{"cause_message", "node was cancelled"}});
    EXPECT_EQ(failure.code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(failure.witness,
              (json{{"cause_message", "node was cancelled"},
                    {"wall_time_ms", {{"granted", 400}, {"used", 812}, {"over_by", 412}}}}));
}
