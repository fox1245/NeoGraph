// NeoGraph Example 58: Harness convergence benchmark (issue #81).
// Runs the convergence simulation across all three feedback modes (A/B/C)
// and generates a comparison report.
//
//   ./example_harness_bench --smoke
//   ./example_harness_bench task1.json [task2.json ...]

#include <neograph/graph/harness_bench.h>
#include <neograph/graph/node.h>
#include <neograph/json.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

using namespace neograph::graph;
using namespace neograph;

namespace {

const char* kSmokeTask = R"({
  "name": "smoke_convergence",
  "description": "Minimal convergence smoke test",
  "natural_spec": "A 2-node chain graph that compiles and validates.",
  "expected_properties": {"min_nodes": 2, "max_nodes": 5}
})";

const char* kSmokeSeed = R"({
  "schema_version": 1,
  "name": "smoke_seed",
  "channels": {"x": {"reducer": "overwrite"}},
  "nodes": {"a": {"type": "pnoop"}, "b": {"type": "pnoop"}},
  "edges": [{"from": "__start__", "to": "a"}, {"from": "a", "to": "b"}]
})";

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

} // anonymous namespace

int main(int argc, char** argv) {
    NodeFactory::instance().register_type("pnoop", make_pnoop,
                                          json::object(), json::object());

    std::vector<HarnessTask> tasks;
    json seed = json::parse(kSmokeSeed);

    if (argc == 2 && std::string(argv[1]) == "--smoke") {
        tasks.push_back(parse_task(json::parse(kSmokeTask)));
    } else if (argc >= 2) {
        for (int i = 1; i < argc; ++i) {
            std::ifstream in(argv[i]);
            if (!in.is_open()) {
                std::cerr << "Cannot open " << argv[i] << "\n";
                return 1;
            }
            std::stringstream buf; buf << in.rdbuf();
            tasks.push_back(parse_task(json::parse(buf.str())));
        }
    } else {
        std::cerr << "Usage: " << argv[0] << " [--smoke | task1.json ...]\n";
        return 1;
    }

    std::vector<FeedbackMode> modes = {
        FeedbackMode::FULL_DIAGNOSTIC,
        FeedbackMode::FAIL_ONLY,
        FeedbackMode::RUNTIME_SYMPTOMS,
    };

    std::vector<ConvergenceMetrics> all_results;

    for (const auto& task : tasks) {
        std::cout << "Task: " << task.name << "\n";
        for (auto mode : modes) {
            ConvergenceMetrics m = run_simulation(task, seed, mode, 10, 42);
            all_results.push_back(m);
            std::cout << "  " << feedback_mode_name(mode)
                      << ": turns=" << m.turns
                      << " converged=" << (m.converged ? "yes" : "no")
                      << " errors=" << m.total_errors
                      << " tokens=" << m.total_estimated_tokens
                      << "\n";
        }
    }

    std::cout << "\n" << generate_report(all_results).dump(2) << "\n";
    return 0;
}
