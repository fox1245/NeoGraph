// NeoGraph Program quickstart.
//
// A Program wraps a Core graph in a lifecycle with durable identities, a budget and
// admission. This example compiles a ProgramSource with one `call_core` operation, admits it
// and runs it through ProgramRuntime. The same GraphEngine remains the only node executor;
// Program owns the envelope around it. The registry, admission profile, policy, catalog and
// runtime are built by examples/program_support.h; this file keeps only the Program itself.
//
// The Program is written as C++ builder JSON. Example 65 writes the same thing in JavaScript.
//
// Usage: ./example_program_quickstart     prints: 1
//
// smoke-expect-line: 1

#include "program_support.h"

#include <iostream>
#include <stdexcept>

namespace {

using namespace examples::program;

// One Core graph: a single node that adds one to the `counter` channel.
json core_definition() {
    return {
        {"schema_version", 1},
        {"name", "program-quickstart-core"},
        {"channels", {{"counter", {{"reducer", "overwrite"}, {"initial", 0}}}}},
        {"nodes", {{"increment", {{"type", "quickstart.increment"}}}}},
        {"edges",
         json::array({{{"from", "__start__"}, {"to", "increment"}},
                      {{"from", "increment"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()},
    };
}

// The Program: one `call_core` operation plus the budget it declares it needs.
json program_document() {
    return {
        {"program_schema_version", 1},
        {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"root", {{"op", "call_core"},
                  {"name", "program-quickstart-core"},
                  {"definition", core_definition()}}},
        {"declared_budget_requirements",
         json::array({
             {{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
             {{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 0}},
             {{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 0}},
             {{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
             {{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
             {{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
             {{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
             {{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
             {{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
         })},
    };
}

}  // namespace

int main() {
    // 1. The executables a Program may name. A node declares the channels it reads and writes.
    Registry registry;
    registry.node(
        "quickstart.increment",
        [](const std::string&, const json&, const NodeInput& in) {
            const auto current = in.state.get("counter");
            const auto value = current.is_number_integer() ? current.get<int>() : 0;
            return NodeOutput{{ChannelWrite{"counter", value + 1}}};
        },
        json{{"reads", json::array({"counter"})},
             {"writes", json::array({"counter"})},
             {"exports", json::array({"counter"})}});
    Host host(std::move(registry).build());

    // 2. Compile and admit the Program, then run it with a budget inside the declared bounds.
    const auto version = host.compile_admit(
        ProgramSource::from_cpp_builder("example:program-quickstart", 1, program_document()));
    const auto result = host.run(version, json::object(), RunBudget{10000, 0, 0, 1, 1, 20, 0, 0, 0});
    if (result.status() != ProgramTerminalStatus::Completed)
        throw std::runtime_error("Program quickstart did not complete");

    std::cout << result.output()["channels"]["counter"]["value"] << '\n';
    return 0;
}
