// Convergence benchmark tests (issue #81).
//
// Verifies:
//   1. format_feedback produces correct output for all three modes
//   2. The simulation runs without crashing
//   3. Metrics are reported correctly

#include <gtest/gtest.h>
#include <neograph/graph/harness_bench.h>
#include <neograph/graph/node.h>
#include <neograph/json.h>

#include <random>

using namespace neograph::graph;
using namespace neograph;

namespace {

struct PnoopNode : GraphNode {
    std::string n;
    explicit PnoopNode(std::string name) : n(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        co_return NodeOutput{};
    }
    std::string get_name() const override { return n; }
};

std::unique_ptr<GraphNode> make_pnoop(const std::string& name,
                                       const json&, const NodeContext&) {
    return std::make_unique<PnoopNode>(name);
}

bool once = [] {
    NodeFactory::instance().register_type("pnoop", make_pnoop,
                                          json::object(), json::object());
    return true;
}();

const char* kValidHarness = R"({
  "schema_version": 1,
  "name": "valid",
  "channels": {"x": {"reducer": "overwrite"}},
  "nodes": {"a": {"type": "pnoop"}, "b": {"type": "pnoop"}},
  "edges": [{"from": "__start__", "to": "a"}, {"from": "a", "to": "b"}]
})";

const char* kInvalidHarness = R"({
  "schema_version": 1,
  "name": "invalid",
  "channels": {"x": {"reducer": "overwrite"}},
  "nodes": {"a": {"type": "pnoop"}},
  "edges": [{"from": "__start__", "to": "ghost"}]
})";

} // anonymous namespace

// ── parse_task ───────────────────────────────────────────────────────

TEST(HarnessBench, ParseTask) {
    json j = json::parse(R"({
        "name": "test_task",
        "description": "A test",
        "natural_spec": "Do X then Y",
        "expected_properties": {
            "min_nodes": 3,
            "has_conditional_edges": true,
            "has_barrier": true
        }
    })");

    auto t = parse_task(j);
    EXPECT_EQ(t.name, "test_task");
    EXPECT_EQ(t.description, "A test");
    EXPECT_EQ(t.natural_spec, "Do X then Y");
    EXPECT_EQ(t.min_nodes, 3);
    EXPECT_TRUE(t.has_conditional_edges);
    EXPECT_TRUE(t.has_barrier);
    EXPECT_FALSE(t.has_interrupt);
}

// ── format_feedback ──────────────────────────────────────────────────

TEST(HarnessBench, FeedbackFullDiagnosticConverged) {
    json core = json::parse(kValidHarness);
    auto fb = format_feedback(core, "", ValidationReport{}, 
                              FeedbackMode::FULL_DIAGNOSTIC);
    EXPECT_TRUE(fb.converged);
    EXPECT_GT(fb.estimated_tokens, 0);
    EXPECT_NE(fb.prompt.find("validates successfully"), std::string::npos);
}

TEST(HarnessBench, FeedbackFullDiagnosticCompileError) {
    json core = json::parse(kInvalidHarness);
    auto fb = format_feedback(core, "strict topology validation failed: "
                              "edges[0] references unknown node 'ghost'",
                              ValidationReport{},
                              FeedbackMode::FULL_DIAGNOSTIC);
    EXPECT_FALSE(fb.converged);
    EXPECT_NE(fb.prompt.find("Compiler error"), std::string::npos);
}

TEST(HarnessBench, FeedbackFailOnlyBaseline) {
    json core = json::parse(kInvalidHarness);
    auto fb = format_feedback(core, "compile failed", ValidationReport{},
                              FeedbackMode::FAIL_ONLY);
    EXPECT_FALSE(fb.converged);
    EXPECT_EQ(fb.prompt.find("Compiler error"), std::string::npos);
    EXPECT_EQ(fb.prompt.find("witness"), std::string::npos);
    EXPECT_NE(fb.prompt.find("Compilation failed"), std::string::npos);
}

TEST(HarnessBench, FeedbackRuntimeSymptoms) {
    json core = json::parse(kValidHarness);
    auto fb = format_feedback(core, "", ValidationReport{},
                              FeedbackMode::RUNTIME_SYMPTOMS, 3);
    EXPECT_TRUE(fb.converged);
    EXPECT_NE(fb.prompt.find("incorrect output"), std::string::npos);
    EXPECT_NE(fb.prompt.find("silently dropped"), std::string::npos);
}

TEST(HarnessBench, FeedbackRuntimeSymptomsCompileError) {
    json core = json::parse(kInvalidHarness);
    auto fb = format_feedback(core, "compile failed", ValidationReport{},
                              FeedbackMode::RUNTIME_SYMPTOMS);
    EXPECT_FALSE(fb.converged);
    EXPECT_NE(fb.prompt.find("Compilation failed"), std::string::npos);
}

// ── Metrics ──────────────────────────────────────────────────────────

TEST(HarnessBench, MetricsToJson) {
    ConvergenceMetrics m;
    m.task_name = "test";
    m.mode = FeedbackMode::FULL_DIAGNOSTIC;
    m.turns = 5;
    m.total_estimated_tokens = 1000;
    m.total_errors = 3;
    m.converged = true;
    m.final_warnings = 1;
    m.meets_spec = true;

    auto j = m.to_json();
    EXPECT_EQ(j["task_name"].get<std::string>(), "test");
    EXPECT_EQ(j["feedback_mode"].get<std::string>(), "A_full_diagnostic");
    EXPECT_EQ(j["turns"].get<int>(), 5);
    EXPECT_EQ(j["total_errors"].get<int>(), 3);
    EXPECT_TRUE(j["converged"].get<bool>());
    EXPECT_TRUE(j["meets_spec"].get<bool>());
}

// ── Simulation ───────────────────────────────────────────────────────

TEST(HarnessBench, SimulationRuns) {
    auto task = parse_task(json::parse(R"({
        "name": "sim_test",
        "natural_spec": "A 2-node chain.",
        "expected_properties": {"min_nodes": 2, "max_nodes": 5}
    })"));

    json seed = json::parse(kValidHarness);
    auto m = run_simulation(task, seed, FeedbackMode::FULL_DIAGNOSTIC, 5, 42);
    EXPECT_GT(m.turns, 0);
    EXPECT_GE(m.total_estimated_tokens, 0);
}

TEST(HarnessBench, AllModesRunWithoutCrash) {
    auto task = parse_task(json::parse(R"({
        "name": "all_modes",
        "natural_spec": "A 2-node chain.",
        "expected_properties": {"min_nodes": 2, "max_nodes": 5}
    })"));

    json seed = json::parse(kValidHarness);

    for (auto mode : {FeedbackMode::FULL_DIAGNOSTIC,
                      FeedbackMode::FAIL_ONLY,
                      FeedbackMode::RUNTIME_SYMPTOMS}) {
        auto m = run_simulation(task, seed, mode, 5, 42);
        EXPECT_GE(m.turns, 1);
        EXPECT_LE(m.turns, 5);
    }
}

// ── Report ───────────────────────────────────────────────────────────

TEST(HarnessBench, GenerateReport) {
    std::vector<ConvergenceMetrics> results;
    ConvergenceMetrics a;
    a.task_name = "t1"; a.mode = FeedbackMode::FULL_DIAGNOSTIC; a.turns = 3; a.converged = true;
    results.push_back(a);
    ConvergenceMetrics b;
    b.task_name = "t1"; b.mode = FeedbackMode::FAIL_ONLY; b.turns = 5; b.converged = false;
    results.push_back(b);

    auto report = generate_report(results);
    EXPECT_TRUE(report.contains("runs"));
    EXPECT_TRUE(report.contains("summary"));
    EXPECT_EQ(report["runs"].size(), 2u);
    EXPECT_EQ(report["summary"]["total_runs"].get<int>(), 2);
    EXPECT_EQ(report["summary"]["converged"].get<int>(), 1);
}