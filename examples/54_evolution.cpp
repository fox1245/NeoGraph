// NeoGraph Example 54: Harness evolution loop (issue #80).
//
//   ./example_evolution seed.json task.json > lineage.json
//   ./example_evolution --smoke
//
// The lineage document contains every individual across all generations,
// compile-gate statistics, and the best individual's core + sourcemap.

#include <neograph/graph/evolution.h>
#include <neograph/graph/node.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace neograph::graph;
using namespace neograph;

namespace {

const char* kSmokeSeed = R"({
  "schema_version": 1,
  "name": "smoke_seed",
  "channels": {"messages": {"reducer": "append"}},
  "nodes": {
    "n0": {"type": "pnoop"},
    "n1": {"type": "pnoop"}
  },
  "edges": [
    {"from": "__start__", "to": "n0"},
    {"from": "n0", "to": "n1"}
  ]
})";

const char* kSmokeTask = R"({
  "name": "trivial",
  "input": {"messages": []},
  "expected_output": {"messages": []},
  "expected_super_steps": 2
})";

struct SmokePnoop : GraphNode {
    std::string name_;
    explicit SmokePnoop(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }
};

std::unique_ptr<GraphNode> make_smoke_pnoop(const std::string& name, const json&, const NodeContext&) {
    return std::make_unique<SmokePnoop>(name);
}

} // anonymous namespace

int main(int argc, char** argv) {
    json seed_doc, task_doc;

    if (argc == 2 && std::string(argv[1]) == "--smoke") {
        seed_doc = json::parse(kSmokeSeed);
        task_doc = json::parse(kSmokeTask);
        NodeFactory::instance().register_type(
            "pnoop", make_smoke_pnoop,
            json::object(), json::object());
    } else if (argc == 3) {
        auto read_file = [](const char* path) {
            std::ifstream in(path);
            if (!in.is_open()) {
                std::cerr << "cannot open " << path << "\n";
                std::exit(2);
            }
            std::stringstream buf;
            buf << in.rdbuf();
            return buf.str();
        };
        seed_doc = json::parse(read_file(argv[1]));
        task_doc = json::parse(read_file(argv[2]));
    } else {
        std::cerr << "usage: " << argv[0] << " seed.json task.json\n"
                  << "   or: " << argv[0] << " --smoke\n";
        return 2;
    }

    Task task;
    task.name = task_doc.value("name", "unnamed");
    task.input = task_doc.value("input", json::object());
    task.expected_output = task_doc.value("expected_output", json::object());
    task.expected_super_steps = task_doc.value("expected_super_steps", 0);

    auto elab = Elaborator::elaborate(seed_doc);

    EvolutionConfig cfg;
    cfg.offspring_per_gen = 10;
    cfg.survivors_per_gen = 3;
    cfg.max_generations = 3;
    cfg.seed = 42;

    auto result = evolve(elab.core, task, cfg);
    auto output = to_json(result);
    output["task"] = task.name;
    output["seed_name"] = seed_doc.value("name", "");

    std::cout << output.dump(2) << "\n";
    return result.compile_passed > 0 ? 0 : 1;
}