// End-to-end scenarios of plan intake and plan repair, with a scripted planner and no network.
//
// A Program asks a planner node for a flat step plan, a registered node checks it with
// intake_step_plan and feeds the violations back, and the accepted plan is expanded and run
// by ExpandTaskGraph over child Programs that compute deterministic values.

#include "program_expansion_nodes.h"

#include "fixtures/unique_temp_dir.h"

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_transition_store.h>
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
#include <neograph/graph/sqlite_checkpoint.h>
#endif

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::program;
using namespace neograph::program_test;
using namespace neograph::program_test::expansion;

const std::string kPrompt =
    "Plan the work with the templates calc.add/v1 (inputs a and b; output sum) and "
    "text.upper/v1 (input text; output upper). Answer with one step-plan/v1 JSON object.";

std::filesystem::path fixture_dir() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "plan_intake";
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    EXPECT_TRUE(input.is_open()) << "cannot open " << path;
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

// The planner graph of the repair pattern: plan -> intake -> (plan again), bounded by the
// attempt count of the intake node and, as a backstop, by max_visits of the planner node.
json planner_definition(std::uint32_t max_attempts, const char* feedback = "replace",
                        bool failure_exit = false) {
    json routes{{"ok", "__end__"}, {"retry", "plan"}, {"give_up", failure_exit ? "report" : "__end__"}};
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels",
         json{{"messages", json{{"reducer", "runtime-overwrite"},
                                {"initial", json::array({json{{"role", "user"}, {"content", kPrompt}}})}}},
              {"proposal", json{{"reducer", "runtime-overwrite"}, {"initial", nullptr}}},
              {"repair", json{{"reducer", "runtime-overwrite"}, {"initial", json::object()}}}}},
        {"nodes",
         json{{"plan", json{{"type", "test.scripted_planner"}}},
              {"intake", json{{"type", std::string(STEP_PLAN_INTAKE_NODE)},
                              {"max_attempts", max_attempts},
                              {"feedback", feedback}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "plan"}},
                               json{{"from", "plan"}, {"to", "intake"}}})},
        {"conditional_edges",
         json::array({json{{"from", "intake"},
                           {"condition", std::string(STEP_PLAN_ROUTE_CONDITION)},
                           {"routes", routes}}})},
        {"node_retry_policies", json{{"plan", json{{"max_visits", max_attempts}}}}}};
    if (failure_exit) {
        definition["nodes"]["report"] = json{{"type", std::string(STEP_PLAN_REPORT_NODE)}};
        definition["failure_exits"]   = json{{"report", json{{"code", "plan.repair_exhausted"}}}};
    }
    return definition;
}

enum class Tail { PlannerOnly, Expand, ExpandOrFallback };

json plan_program(json planner, Tail tail, const RunOptions& options) {
    auto document = program_document("exp-echo");
    document["program_schema_version"]                     = PROGRAM_SCHEMA_VERSION_V4;
    document["declared_budget_requirements"][3]["maximum"] = options.concurrency;
    document["declared_budget_requirements"][4]["maximum"] = 32;
    if (tail != Tail::PlannerOnly) {
        document["declared_budget_requirements"][6]["minimum"] = 1;
        document["declared_budget_requirements"][6]["maximum"] = 1;
        document["declared_budget_requirements"][7]["minimum"] = options.max_depth;
        document["declared_budget_requirements"][7]["maximum"] = options.max_depth;
        document["declared_budget_requirements"][8]["minimum"] = options.max_tasks;
        document["declared_budget_requirements"][8]["maximum"] = options.max_tasks;
    }
    json expand{{"op", "expand_task_graph"},
                {"proposal_source", json{{"artifact", "input"}, {"field", "/channels/proposal/value"}}},
                {"max_tasks", options.max_tasks},
                {"max_edges", options.max_tasks * 2},
                {"max_depth", options.max_depth},
                {"max_dynamic_compiles", 1},
                {"max_total_children", options.max_tasks},
                {"max_concurrency", options.concurrency},
                {"failure_policy", options.failure_policy}};
    json root;
    if (tail == Tail::PlannerOnly) {
        root = json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(planner)}};
    } else {
        json last = expand;
        if (tail == Tail::ExpandOrFallback) {
            last = json{{"op", "branch"},
                        {"condition", json{{"path", "/channels/repair/value/status"}, {"equals", "ok"}}},
                        {"then", expand},
                        {"else", json{{"op", "return"}, {"value", json{{"fallback", true}}}}}};
        }
        root = json{{"op", "sequence"},
                    {"name", "main"},
                    {"definition", std::move(planner)},
                    {"children", json::array({json{{"op", "call_core"}}, std::move(last)})}};
    }
    document["root"] = std::move(root);
    return document;
}

std::string plan_text(std::initializer_list<std::string> steps) {
    std::string text = R"({"schema":"step-plan/v1","steps":[)";
    bool        first = true;
    for (const auto& step : steps) {
        if (!first) text += ",";
        first = false;
        text += step;
    }
    return text + "]}";
}

// Two independent sums, a sum of both, and a literal text after it.
const std::string kGoodPlan = plan_text(
    {R"({"id":"s1","use":"calc.add/v1","params":{"a":2,"b":3}})",
     R"({"id":"s2","use":"calc.add/v1","params":{"a":10,"b":20}})",
     R"({"id":"s3","use":"calc.add/v1","params":{"a":{"from_step":"s1"},"b":{"from_step":"s2"}}})",
     R"({"id":"s4","use":"text.upper/v1","params":{"text":"done"},"after":["s3"]})"});

// An unknown template, a reference to a step that comes later, and a key outside the format.
const std::string kBadPlan = plan_text(
    {R"({"id":"s1","use":"calc.ad/v1","params":{"a":2,"b":3}})",
     R"({"id":"s2","use":"calc.add/v1","params":{"a":{"from_step":"s3"},"b":1},"note":"x"})",
     R"({"id":"s3","use":"calc.add/v1","params":{"a":1,"b":1}})"});

void script(std::vector<std::string> replies, std::vector<std::uint64_t> tokens = {}) {
    std::lock_guard lock(planner_mutex);
    planner_replies = std::move(replies);
    planner_tokens  = std::move(tokens);
}

json planner_messages(std::size_t call) {
    std::lock_guard lock(planner_mutex);
    return call < planner_seen.size() ? planner_seen[call] : json();
}

RunOptions parallel_options() {
    RunOptions options;
    options.concurrency    = 2;
    options.max_tasks      = 4;
    options.max_operations = 32;
    return options;
}

double number_at(const json& output, std::size_t index) {
    return output["tasks"][index]["output"]["channels"]["value"]["value"].get<double>();
}

}  // namespace

TEST(PlanIntakeScenario, S1_AValidFlatPlanBecomesAParallelTaskGraph) {
    ExpansionHarness h;
    h.reset_observations();
    script({kGoodPlan});
    const auto options = parallel_options();
    const auto run = h.run_program(plan_program(planner_definition(3), Tail::Expand, options),
                                   json::object(), options);

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    EXPECT_EQ(planner_calls.load(), 1);
    ASSERT_TRUE(run.record.has_value());
    // The lowered proposal is the one the golden says (derived independently of the library).
    EXPECT_EQ(run.record->fragment.proposal_hash(),
              "sha256:26ab24c9157d6011c3ee047516f8c700f3075492d617dba03dceddd83f59ea25");
    // The two independent sums are launched in one chunk; the sum of both and the text follow.
    EXPECT_EQ(run.events, (Events{"s1:active:1", "s2:active:1", "s1:completed:1", "s2:completed:1",
                                  "s3:active:1", "s3:completed:1", "s4:active:1", "s4:completed:1",
                                  "terminal"}));
    EXPECT_EQ(number_at(run.result.output(), 0), 5);
    EXPECT_EQ(number_at(run.result.output(), 1), 30);
    EXPECT_EQ(number_at(run.result.output(), 2), 35);  // bound from both producers
    EXPECT_EQ(run.result.output()["tasks"][3]["output"]["channels"]["value"]["value"], "DONE");
    EXPECT_EQ(run.record->fragment.schema_version(), 2U);
}

namespace {

// S2: the first plan is rejected, the second is accepted. The planner is asked exactly twice
// and its tokens are charged once each.
void expect_repair_turn(ExpansionHarness& h) {
    h.reset_observations();
    script({kBadPlan, kGoodPlan}, {120, 80});
    const auto options = parallel_options();
    const auto run = h.run_program(plan_program(planner_definition(3), Tail::Expand, options),
                                   json::object(), options);

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    EXPECT_EQ(planner_calls.load(), 2);
    EXPECT_EQ(run.result.usage().model_tokens, 200U);

    // What the planner saw on the second call: the prompt, its rejected plan, the violations.
    const auto seen = planner_messages(1);
    ASSERT_TRUE(seen.is_array());
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_EQ(seen[0]["role"], "user");
    EXPECT_EQ(seen[0]["content"], kPrompt);
    EXPECT_EQ(seen[1]["role"], "assistant");
    EXPECT_EQ(seen[1]["content"], kBadPlan);
    EXPECT_EQ(seen[2]["role"], "user");
    const auto expected = read_text(fixture_dir() / "s2_feedback.txt");
    EXPECT_EQ(seen[2]["content"].get<std::string>(), expected);
    // The text is what the library renders for the plan's violations.
    const auto violations = intake_step_plan(kBadPlan, plan_repair_options().intake).violations;
    EXPECT_EQ(format_violations_for_model(violations, 2048), expected);

    ASSERT_TRUE(run.record.has_value());
    EXPECT_EQ(run.record->fragment.proposal_hash(),
              "sha256:26ab24c9157d6011c3ee047516f8c700f3075492d617dba03dceddd83f59ea25");
    EXPECT_TRUE(run.record->terminal);
}

}  // namespace

TEST(PlanIntakeScenario, S2_ARejectedPlanIsRepairedOnTheNextTurn) {
    ExpansionHarness h;
    expect_repair_turn(h);
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE) && defined(NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT)
TEST(PlanIntakeScenario, S2_TheRepairTurnIsSentOnceAndChargedOnceOnTheSqliteHost) {
    neograph::graph::test_support::UniqueTempDir directory("plan-intake");
    ExpansionHarness h;
    h.fixture.journal     = std::make_shared<SQLiteProgramTransitionStore>(directory.file("program.db"));
    h.fixture.checkpoints = std::make_shared<neograph::graph::SqliteCheckpointStore>(
        directory.file("checkpoints.db"));
    h.fixture.runtime = h.fixture.make_runtime();
    expect_repair_turn(h);
}
#endif

TEST(PlanIntakeScenario, TheRepairReportCountsTheTurnsAndTheViolationCodes) {
    ExpansionHarness h;
    h.reset_observations();
    script({kBadPlan, kGoodPlan});
    const auto options = parallel_options();
    auto planner_only = options;
    planner_only.expands = false;
    const auto run = h.run_program(plan_program(planner_definition(3), Tail::PlannerOnly, options),
                                   json::object(), planner_only);

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    const auto report = run.result.output()["channels"]["repair"]["value"];
    EXPECT_EQ(report["status"], "ok");
    EXPECT_EQ(report["attempts"], 2);
    EXPECT_EQ(report["max_attempts"], 3);
    EXPECT_EQ(report["last_violations"].size(), 0U);
    EXPECT_EQ(report["violation_code_histogram"],
              (json{{"STEP_REF_FORWARD", 1}, {"STEP_UNKNOWN_FIELD", 1}, {"STEP_USE_UNKNOWN", 1}}));
    ASSERT_EQ(report["plan_digests"].size(), 2U);
    EXPECT_NE(report["plan_digests"][0], report["plan_digests"][1]);
    EXPECT_EQ(report["envelope"], (json::array({"strict", "strict"})));
    EXPECT_TRUE(run.result.output()["channels"]["proposal"]["value"].is_object());
}

TEST(PlanIntakeScenario, S3_ThreeRejectedPlansExhaustTheLoopAndTheParentFallsBack) {
    ExpansionHarness h;
    h.reset_observations();
    script({kBadPlan, kBadPlan, kBadPlan}, {50, 50, 50});
    const auto options = parallel_options();
    const auto run = h.run_program(
        plan_program(planner_definition(3), Tail::ExpandOrFallback, options), json::object(), options);

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    EXPECT_EQ(run.result.output(), (json{{"fallback", true}}));
    EXPECT_EQ(planner_calls.load(), 3);
    EXPECT_EQ(run.result.usage().model_tokens, 150U);  // three planner turns and nothing else
    EXPECT_FALSE(run.record.has_value());              // no fragment was published
    EXPECT_TRUE(run.events.empty());
    EXPECT_TRUE(run.child_budgets.empty());            // no task ran
}

TEST(PlanIntakeScenario, S3_TheReportSaysExhaustedAndCountsEveryCode) {
    ExpansionHarness h;
    h.reset_observations();
    script({kBadPlan, kBadPlan, kBadPlan});
    const auto options = parallel_options();
    auto planner_only = options;
    planner_only.expands = false;
    const auto run = h.run_program(plan_program(planner_definition(3), Tail::PlannerOnly, options),
                                   json::object(), planner_only);

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed);
    const auto report = run.result.output()["channels"]["repair"]["value"];
    EXPECT_EQ(report["status"], "exhausted");
    EXPECT_EQ(report["attempts"], 3);
    EXPECT_EQ(report["violation_code_histogram"],
              (json{{"STEP_REF_FORWARD", 3}, {"STEP_UNKNOWN_FIELD", 3}, {"STEP_USE_UNKNOWN", 3}}));
    EXPECT_EQ(report["last_violations"].size(), 3U);
    EXPECT_EQ(report["last_violations"][0]["code"], "STEP_USE_UNKNOWN");
    EXPECT_TRUE(run.result.output()["channels"]["proposal"]["value"].is_null());
}

TEST(PlanIntakeScenario, S3_AFailureExitEndsTheRunWithTheRepairCode) {
    ExpansionHarness h;
    h.reset_observations();
    script({kBadPlan, kBadPlan});
    const auto options = parallel_options();
    auto planner_only = options;
    planner_only.expands = false;
    const auto run = h.run_program(
        plan_program(planner_definition(2, "replace", true), Tail::PlannerOnly, options), json::object(),
        planner_only);

    EXPECT_EQ(run.result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(run.result.failure().has_value());
    EXPECT_EQ(run.result.failure()->code, "plan.repair_exhausted");
    EXPECT_EQ(planner_calls.load(), 2);
    EXPECT_EQ(run.result.output()["channels"]["repair"]["value"]["status"], "exhausted");
}

TEST(PlanIntakeScenario, TheFeedbackModeDecidesHowMuchOfTheConversationTheNextTurnSees) {
    for (const char* mode : {"replace", "append"}) {
        SCOPED_TRACE(mode);
        ExpansionHarness h;
        h.reset_observations();
        script({kBadPlan, kBadPlan, kGoodPlan});
        const auto options = parallel_options();
        auto planner_only = options;
        planner_only.expands = false;
        const auto run = h.run_program(
            plan_program(planner_definition(3, mode), Tail::PlannerOnly, options), json::object(),
            planner_only);
        ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed);
        EXPECT_EQ(planner_calls.load(), 3);
        EXPECT_EQ(planner_messages(1).size(), 3U);
        // Replace drops the first rejected plan and its violations; append keeps them.
        EXPECT_EQ(planner_messages(2).size(), std::string_view(mode) == "replace" ? 3U : 5U);
    }
}

TEST(PlanIntakeScenario, TheSealedGraphCannotAskForMoreAttemptsThanTheHostAllows) {
    ExpansionHarness h;
    const auto options = parallel_options();
    EXPECT_ANY_THROW((void)h.fixture.admit_document(
        plan_program(planner_definition(9), Tail::PlannerOnly, options)));
    EXPECT_ANY_THROW((void)h.fixture.admit_document(
        plan_program(planner_definition(3, "weird"), Tail::PlannerOnly, options)));
}

// ---- The host helper that plans before a run ----

TEST(PlanRepairHelper, RepairsAcrossTurnsAndAccountsForTheTokensItSpent) {
    PlanRepairOptions options;
    options.intake       = plan_repair_options().intake;
    options.max_attempts = 3;
    std::vector<std::vector<PlannerMessage>> seen;
    const std::vector<std::string> replies = {kBadPlan, kGoodPlan};
    const auto result = repair_step_plan(options, kPrompt, [&](const std::vector<PlannerMessage>& m) {
        seen.push_back(m);
        return PlannerReply{replies[seen.size() - 1], 100, 20};
    });

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.attempts, 2U);
    EXPECT_EQ(result.prompt_tokens, 200U);
    EXPECT_EQ(result.completion_tokens, 40U);
    ASSERT_TRUE(result.proposal_hash.has_value());
    EXPECT_EQ(*result.proposal_hash,
              "sha256:26ab24c9157d6011c3ee047516f8c700f3075492d617dba03dceddd83f59ea25");
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0], (std::vector<PlannerMessage>{{"user", kPrompt}}));
    ASSERT_EQ(seen[1].size(), 3U);
    EXPECT_EQ(seen[1][1], (PlannerMessage{"assistant", kBadPlan}));
    EXPECT_EQ(seen[1][2].content, read_text(fixture_dir() / "s2_feedback.txt"));
    EXPECT_EQ(result.repair["status"], "ok");
    EXPECT_EQ(result.repair["attempts"], 2);
    EXPECT_TRUE(result.proposal.is_object());
}

TEST(PlanRepairHelper, StopsAtTheAttemptBoundAndReportsWhatWentWrong) {
    PlanRepairOptions options;
    options.intake       = plan_repair_options().intake;
    options.max_attempts = 3;
    int  calls           = 0;
    auto result = repair_step_plan(options, kPrompt, [&](const std::vector<PlannerMessage>&) {
        ++calls;
        return PlannerReply{kBadPlan, 10, 5};
    });
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(result.attempts, 3U);
    EXPECT_TRUE(result.proposal.is_null());
    EXPECT_FALSE(result.proposal_hash.has_value());
    EXPECT_EQ(result.repair["status"], "exhausted");
    EXPECT_EQ(result.repair["violation_code_histogram"]["STEP_USE_UNKNOWN"], 3);
    EXPECT_EQ(result.prompt_tokens, 30U);

    // The conversation ends with the last answer, which no turn is left to correct. In replace
    // mode the earlier turns have been dropped: the prompt, the second plan with its violations,
    // and the third plan.
    ASSERT_EQ(result.messages.size(), 4U);
    EXPECT_EQ(result.messages[0], (PlannerMessage{"user", kPrompt}));
    EXPECT_EQ(result.messages[1], (PlannerMessage{"assistant", kBadPlan}));
    EXPECT_EQ(result.messages[2].role, "user");
    EXPECT_EQ(result.messages[3], (PlannerMessage{"assistant", kBadPlan}));

    // Append keeps every turn: prompt, plan 1, violations 1, plan 2.
    options.feedback     = StepPlanFeedback::Append;
    options.max_attempts = 2;
    result = repair_step_plan(options, kPrompt, [&](const std::vector<PlannerMessage>&) {
        return PlannerReply{kBadPlan, 0, 0};
    });
    EXPECT_EQ(result.messages.size(), 4U);
}

TEST(PlanRepairHelper, AcceptsAFirstPlanWithoutAskingAgain) {
    PlanRepairOptions options;
    options.intake = plan_repair_options().intake;
    int  calls     = 0;
    const auto result = repair_step_plan(options, kPrompt, [&](const std::vector<PlannerMessage>&) {
        ++calls;
        return PlannerReply{"Here you go:\n```json\n" + kGoodPlan + "\n```", 1, 1};
    });
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(result.repair["envelope"], (json::array({"recovered"})));
    EXPECT_THROW((void)repair_step_plan(options, kPrompt, {}), std::invalid_argument);
}
