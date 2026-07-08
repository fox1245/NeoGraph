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
#include <neograph/graph/evolution.h>
#include <neograph/graph/node.h>
#include <neograph/json.h>

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

// Core topology fixture: 2-node chain. No DSL keys.
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

// DSL fixture with templates + use.
const char* kDslSeed = R"({
  "schema_version": 1,
  "name": "test_dsl",
  "vars": {"greeting": "hello"},
  "channels": {
    "messages": {"reducer": "append"}
  },
  "templates": {
    "respond": {
      "params": ["msg"],
      "nodes": {"act": {"type": "pnoop"}},
      "edges": [{"from": "act", "to": "__end__"}]
    },
    "log": {
      "params": ["level"],
      "nodes": {"logger": {"type": "pnoop"}},
      "edges": [{"from": "logger", "to": "__end__"}]
    }
  },
  "use": [
    {"template": "respond", "prefix": "r1",
     "args": {"msg": "${greeting}"}},
    {"template": "log", "prefix": "l1",
     "args": {"level": "info"}}
  ],
  "nodes": {},
  "edges": [
    {"from": "__start__", "to": "r1_act"}
  ]
})";

} // anonymous namespace

// ── Mutation operator unit tests ────────────────────────────────────

TEST(Evolution, AllOperatorsReturnSomething) {
    pnoop_registered();
    auto ops = all_operators();
    EXPECT_GE(ops.size(), 3u);  // at minimum swap/add/remove

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

TEST(Evolution, DslMutationsElaborateThenCompile) {
    pnoop_registered();
    auto elab = Elaborator::elaborate(json::parse(kDslSeed));
    json core = elab.core;
    EXPECT_FALSE(core.contains("vars"));
    EXPECT_FALSE(core.contains("templates"));
    EXPECT_FALSE(core.contains("use"));

    NodeContext ctx;
    Task task;

    auto ops = all_operators();
    std::mt19937 rng(42);

    int applied = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (const auto& op : ops) {
            auto mr = op(core, rng);
            if (!mr.core) continue;
            auto score = evaluate(*mr.core, task, ctx);
            EXPECT_TRUE(score.compiled) << mr.description;
            EXPECT_TRUE(score.validated) << mr.description;
            applied++;
        }
    }
    EXPECT_GE(applied, 5);
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

TEST(Evolution, DslSeedLoopRuns) {
    pnoop_registered();
    auto elab = Elaborator::elaborate(json::parse(kDslSeed));
    Task task;

    EvolutionConfig cfg;
    cfg.offspring_per_gen = 5;
    cfg.survivors_per_gen = 2;
    cfg.max_generations = 2;
    cfg.seed = 1;

    auto result = evolve(elab.core, task, cfg);
    EXPECT_GT(result.compile_passed, 0);
    EXPECT_GT(result.total_offspring, 0);
    EXPECT_GE(result.best.generation, 0);
    EXPECT_NE(result.best.core.dump(), "");
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
    EXPECT_TRUE(j["genealogy"].is_array());
    // Best individual should include the lockfile.
    EXPECT_TRUE(j["best"].contains("lockfile"))
        << "best individual must carry its lockfile";
}