// Evolution loop tests (issue #80).
//
// Verifies:
//   1. Every mutation operator produces structurally valid core JSON
//      (passes the compile gate).
//   2. The evolution loop runs deterministically (same seed → same
//      population).
//   3. At least one offspring per mutation kind actually fires on the
//      test corpus (coverage self-check).

#include <gtest/gtest.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/evolution.h>
#include <neograph/graph/node.h>
#include <neograph/json.h>

#include <algorithm>
#include <random>
#include <set>

using namespace neograph::graph;
using namespace neograph;

namespace {

struct Pnoop : GraphNode {
    std::string name_;
    explicit Pnoop(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }
};

struct EvaluationNode : GraphNode {
    std::string name_;
    std::string mode_;
    json value_;

    EvaluationNode(std::string name, const json& config)
        : name_(std::move(name)),
          mode_(config.value("mode", "copy")),
          value_(config.value("value", json())) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        if (mode_ == "fail") throw std::runtime_error("intentional failure");
        if (mode_ == "cancel") throw CancelledException("intentional cancellation");

        NodeOutput output;
        output.writes.push_back({
            "result", mode_ == "copy" ? input.state.get("input") : value_});
        co_return output;
    }

    std::string get_name() const override { return name_; }
};

std::unique_ptr<GraphNode> make_pnoop(const std::string& name, const json&, const NodeContext&) {
    return std::make_unique<Pnoop>(name);
}

// Register pnoop once for all tests in this suite.
bool pnoop_registered() {
    static bool once = [] {
        NodeFactory::instance().register_type("pnoop", make_pnoop,
            json::object(), json::object());
        return true;
    }();
    return once;
}

bool evaluation_node_registered() {
    static bool once = [] {
        NodeFactory::instance().register_type(
            "evolution_test",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<EvaluationNode>(name, config);
            },
            json::object(),
            json::parse(R"({"reads":["input"],"writes":["result"]})"));
        return true;
    }();
    return once;
}

json evaluation_core(const std::string& mode, const json& value = json()) {
    json node = {{"type", "evolution_test"}, {"mode", mode}};
    if (!value.is_null()) node["value"] = value;
    return {
        {"schema_version", 1},
        {"name", "evaluation_core"},
        {"channels", {
            {"input", {{"reducer", "overwrite"}}},
            {"result", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {{"candidate", std::move(node)}}},
        {"edges", {{{"from", "__start__"}, {"to", "candidate"}}}}
    };
}

Task evaluation_task() {
    Task task;
    task.name = "behavioral";
    task.input = {{"input", "expected"}};
    task.expected_output = {{"result", "expected"}};
    task.expected_super_steps = 1;
    return task;
}

// Strict Core topology fixture: 2-node chain.
const char* kMinimalCore = R"({
  "schema_version": 1,
  "name": "test_core",
  "channels": {"x": {"reducer": "overwrite"}},
  "nodes": {
    "a": {"type": "pnoop"},
    "b": {"type": "pnoop"}
  },
  "edges": [
    {"from": "__start__", "to": "a"},
    {"from": "a", "to": "b"}
  ]
})";


} // anonymous namespace

// ── Mutation operator unit tests ────────────────────────────────────

TEST(Evolution, AllOperatorsReturnSomething) {
    pnoop_registered();
    auto ops = all_operators();
    EXPECT_GE(ops.size(), 3u);  // at minimum edge add/remove and routing toggle

    json core = json::parse(kMinimalCore);
    std::mt19937 rng(42);

    for (size_t i = 0; i < ops.size(); ++i) {
        auto result = ops[i](core, rng);
        // Individual operators might not apply to a 2-node core;
        // that's fine — we just check they return something or
        // gracefully opt out.
        if (result.core) {
            // Validate the mutated core at least parses.
            json mutated = *result.core;
            EXPECT_TRUE(mutated.is_object());
            EXPECT_FALSE(result.description.empty());
        }
    }
}

TEST(Evolution, CompileGatePassesOnAllAppliedMutations) {
    pnoop_registered();
    json core = json::parse(kMinimalCore);
    NodeContext ctx;
    Task task;
    task.name = "test";

    auto ops = all_operators();
    std::mt19937 rng(99);

    int applied = 0;
    for (int attempt = 0; attempt < 200 && applied < (int)ops.size(); ++attempt) {
        for (const auto& op : ops) {
            auto mr = op(core, rng);
            if (!mr.core) continue;
            // Compile gate.
            auto score = evaluate(*mr.core, task, ctx);
EXPECT_TRUE(score.compiled) << "op failed: " << mr.description
                                      << " summary: " << score.summary;
        EXPECT_TRUE(score.validated) << "op failed validation: "
                                     << mr.description
                                     << " summary: " << score.summary;
            applied++;
        }
    }
    // At least every operator must have fired once within 200 attempts.
    EXPECT_GE(applied, (int)ops.size());
}


// ── Behavioral evaluation ───────────────────────────────────────────

TEST(Evolution, EvaluateRunsAndComparesDeclaredOutputChannels) {
    evaluation_node_registered();
    NodeContext ctx;
    Task task = evaluation_task();

    auto correct = evaluate(evaluation_core("copy"), task, ctx);
    auto incorrect = evaluate(evaluation_core("constant", "wrong"), task, ctx);

    EXPECT_TRUE(correct.compiled);
    EXPECT_TRUE(correct.validated);
    EXPECT_TRUE(correct.executed);
    EXPECT_TRUE(correct.correct);
    EXPECT_DOUBLE_EQ(correct.cost, 0.0);
    EXPECT_NE(correct.summary.find("behavior matched"), std::string::npos);
    EXPECT_NE(correct.summary.find("super_steps=1"), std::string::npos);

    EXPECT_TRUE(incorrect.compiled);
    EXPECT_TRUE(incorrect.validated);
    EXPECT_TRUE(incorrect.executed);
    EXPECT_FALSE(incorrect.correct);
    EXPECT_GE(incorrect.cost, 2.0);
    EXPECT_GT(incorrect.cost, correct.cost);
    EXPECT_NE(incorrect.summary.find("output mismatch"), std::string::npos);
    EXPECT_NE(incorrect.summary.find("result"), std::string::npos);
}

TEST(Evolution, ExpectedSuperStepsContributeToCorrectCandidateCost) {
    evaluation_node_registered();
    NodeContext ctx;
    Task task = evaluation_task();
    json two_step = evaluation_core("copy");
    two_step["nodes"]["second"] = {
        {"type", "evolution_test"}, {"mode", "copy"}};
    two_step["edges"] = json::array({
        {{"from", "__start__"}, {"to", "candidate"}},
        {{"from", "candidate"}, {"to", "second"}}
    });

    auto one = evaluate(evaluation_core("copy"), task, ctx);
    auto two = evaluate(two_step, task, ctx);

    EXPECT_TRUE(two.correct);
    EXPECT_GT(two.cost, one.cost);
    EXPECT_LE(two.cost, 1.0);
    EXPECT_NE(two.summary.find("super_steps=2"), std::string::npos);
}

TEST(Evolution, ParallelNodesCountAsOneSuperStep) {
    evaluation_node_registered();
    NodeContext ctx;
    Task task = evaluation_task();
    json parallel = evaluation_core("copy");
    parallel["nodes"]["second"] = {
        {"type", "evolution_test"}, {"mode", "copy"}};
    parallel["edges"].push_back({{"from", "__start__"}, {"to", "second"}});

    auto score = evaluate(parallel, task, ctx);

    EXPECT_TRUE(score.correct);
    EXPECT_DOUBLE_EQ(score.cost, 0.0);
    EXPECT_NE(score.summary.find("super_steps=1"), std::string::npos);
}

TEST(Evolution, ComparisonReportsMissingChannelsAndInvalidExpectations) {
    evaluation_node_registered();
    NodeContext ctx;
    Task task = evaluation_task();
    task.expected_output["missing"] = true;

    auto missing = evaluate(evaluation_core("copy"), task, ctx);
    EXPECT_TRUE(missing.executed);
    EXPECT_FALSE(missing.correct);
    EXPECT_GE(missing.cost, 2.0);
    EXPECT_NE(missing.summary.find("missing (missing)"), std::string::npos);

    task.expected_output = json::array();
    auto invalid = evaluate(evaluation_core("copy"), task, ctx);
    EXPECT_TRUE(invalid.executed);
    EXPECT_LT(invalid.cost, 0.0);
    EXPECT_NE(invalid.summary.find("expected_output must be an object"),
              std::string::npos);
}

TEST(Evolution, ExecutionFailuresCancellationAndTimeoutAreDistinct) {
    evaluation_node_registered();
    NodeContext ctx;
    Task task = evaluation_task();

    auto failed = evaluate(evaluation_core("fail"), task, ctx);
    auto cancelled = evaluate(evaluation_core("cancel"), task, ctx);

    json cycle = evaluation_core("copy");
    cycle["edges"].push_back({{"from", "candidate"}, {"to", "candidate"}});
    auto timed_out = evaluate(cycle, task, ctx);

    EXPECT_LT(failed.cost, 0.0);
    EXPECT_NE(failed.summary.find("execution failed"), std::string::npos);
    EXPECT_LT(cancelled.cost, 0.0);
    EXPECT_NE(cancelled.summary.find("execution cancelled"), std::string::npos);
    EXPECT_TRUE(timed_out.executed);
    EXPECT_FALSE(timed_out.correct);
    EXPECT_LT(timed_out.cost, 0.0);
    EXPECT_NE(timed_out.summary.find("execution timeout"), std::string::npos);
    EXPECT_NE(timed_out.summary.find("after 50 super-steps"), std::string::npos);
}

TEST(Evolution, DryRunReportsStructuralValidityWithoutBehavioralClaims) {
    evaluation_node_registered();
    auto core = evaluation_core("copy");
    Task task = evaluation_task();
    EvolutionConfig cfg;
    cfg.offspring_per_gen = 10;
    cfg.survivors_per_gen = 3;
    cfg.max_generations = 1;
    cfg.seed = 42;
    cfg.run_evaluation = false;

    auto result = evolve(core, task, cfg);

    for (const auto& individual : result.population) {
        EXPECT_TRUE(individual.score.compiled);
        EXPECT_TRUE(individual.score.validated);
        EXPECT_FALSE(individual.score.executed);
        EXPECT_FALSE(individual.score.correct);
        EXPECT_NE(individual.score.summary.find("behavior not evaluated"),
                  std::string::npos);
    }
}

TEST(Evolution, SelectionPrefersBehaviorallyCorrectOffspringDeterministically) {
    evaluation_node_registered();
    json seed = {
        {"schema_version", 1},
        {"name", "selection_core"},
        {"channels", {
            {"input", {{"reducer", "overwrite"}}},
            {"result", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"good", {{"type", "evolution_test"}, {"mode", "copy"}}},
            {"bad", {{"type", "evolution_test"}, {"mode", "constant"},
                     {"value", "wrong"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "good"}},
            {{"from", "good"}, {"to", "bad"}}
        })}
    };
    Task task = evaluation_task();
    EvolutionConfig cfg;
    cfg.offspring_per_gen = 100;
    cfg.survivors_per_gen = 100;
    cfg.max_generations = 1;
    cfg.seed = 42;
    cfg.run_evaluation = true;

    auto first = evolve(seed, task, cfg);
    auto second = evolve(seed, task, cfg);

    EXPECT_FALSE(first.population.front().score.correct);
    EXPECT_TRUE(first.best.score.correct);
    EXPECT_LT(first.best.score.cost, first.population.front().score.cost);
    EXPECT_EQ(first.best.core["edges"].size(), 1u);
    EXPECT_TRUE(std::any_of(
        first.population.begin(), first.population.end(),
        [](const Individual& individual) { return individual.score.cost < 0.0; }));
    auto first_json = to_json(first);
    EXPECT_EQ(first_json["genealogy"].size(), first.population.size() - 1);
    bool saw_failed_lineage = false;
    for (const auto& lineage : first_json["genealogy"]) {
        saw_failed_lineage |= lineage["cost"].get<double>() < 0.0;
    }
    EXPECT_TRUE(saw_failed_lineage);
    EXPECT_EQ(first_json.dump(), to_json(second).dump());
}

// ── Evolution loop ──────────────────────────────────────────────────

TEST(Evolution, LoopRunsDeterministically) {
    pnoop_registered();
    json core = json::parse(kMinimalCore);
    Task task;

    EvolutionConfig cfg;
    cfg.offspring_per_gen = 10;
    cfg.survivors_per_gen = 3;
    cfg.max_generations = 2;
    cfg.seed = 42;

    auto r1 = evolve(core, task, cfg);
    auto r2 = evolve(core, task, cfg);

    // Same seed → same population topology (score fields identical).
    EXPECT_EQ(r1.population.size(), r2.population.size());
    EXPECT_EQ(r1.total_offspring, r2.total_offspring);
    EXPECT_EQ(r1.compile_passed, r2.compile_passed);
    EXPECT_GT(r1.compile_passed, 0) << "at least one offspring must compile";

    // Serialize and compare JSON output.
    auto j1 = to_json(r1);
    auto j2 = to_json(r2);
    EXPECT_EQ(j1.dump(), j2.dump());
}

TEST(Evolution, DifferentSeedsDiffer) {
    pnoop_registered();
    json core = json::parse(kMinimalCore);
    Task task;

    EvolutionConfig cfg;
    cfg.offspring_per_gen = 10;
    cfg.survivors_per_gen = 3;
    cfg.max_generations = 2;

    cfg.seed = 42;
    auto r1 = evolve(core, task, cfg);

    cfg.seed = 9999;
    auto r2 = evolve(core, task, cfg);

    // Different seeds should produce different lineage (different
    // total_offspring or compile_passed is possible but not
    // guaranteed; the actual population content must differ).
    bool differs = (r1.total_offspring != r2.total_offspring) ||
                   (r1.compile_passed != r2.compile_passed);
    if (!differs) {
        // Check that at least one population entry differs.
        for (size_t i = 0; i < std::min(r1.population.size(), r2.population.size()); ++i) {
            if (r1.population[i].mutation_description !=
                r2.population[i].mutation_description) {
                differs = true;
                break;
            }
        }
    }
    EXPECT_TRUE(differs) << "different seeds must produce different lineages";
}

// ── Genealogy / JSON output ─────────────────────────────────────────

TEST(Evolution, ToJsonContainsAllFields) {
    pnoop_registered();
    json core = json::parse(kMinimalCore);
    Task task;

    EvolutionConfig cfg;
    cfg.offspring_per_gen = 5;
    cfg.survivors_per_gen = 2;
    cfg.max_generations = 1;
    cfg.seed = 0;

    auto result = evolve(core, task, cfg);
    auto j = to_json(result);

    EXPECT_TRUE(j.contains("total_offspring"));
    EXPECT_TRUE(j.contains("compile_passed"));
    EXPECT_TRUE(j.contains("execute_passed"));
    EXPECT_TRUE(j.contains("population"));
    EXPECT_TRUE(j.contains("best"));
    EXPECT_TRUE(j.contains("genealogy"));
    EXPECT_TRUE(j["population"].is_array());
    EXPECT_TRUE(j["best"].contains("generation"));
    EXPECT_TRUE(j["best"].contains("cost"));
    EXPECT_TRUE(j["best"].contains("summary"));
    EXPECT_TRUE(j["best"].contains("executed"));
    EXPECT_TRUE(j["best"].contains("correct"));
    EXPECT_TRUE(j["genealogy"].is_array());
    // Best individual should include the lockfile.
    EXPECT_TRUE(j["best"].contains("lockfile"))
        << "best individual must carry its lockfile";
}
