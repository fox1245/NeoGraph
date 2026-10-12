// NeoGraph Example 65: a JavaScript Program run from C++
//
// A Program can be written as JavaScript with two exports. define() returns the one graph the
// Program may run; its nodes are C++ node types the host registered. main() is a generator
// that drives the graph: `yield ng.callCore(name, input, site)` runs it and returns its
// output, so main() can call the same graph more than once and decide from the result.
// Example 63 builds the same envelope from C++ JSON; here the control flow is JavaScript.
//
// Needs -DNEOGRAPH_BUILD_PROGRAM=ON and -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON.
// Usage: ./example_program_javascript
//
// smoke-expect: status: completed
// smoke-expect: first call: 42
// smoke-expect: second call: 84
// smoke-expect: trace: ["twice","twice"]
// smoke-forbid: [neograph] lint

#include "program_support.h"

#include <iostream>
#include <stdexcept>

using namespace examples::program;

namespace {

constexpr const char* kProgram = R"js(
export function define() {
  return ng.graph("double")
    .channel("value", {reducer: "overwrite", initial: 0})
    .node("twice", {type: "demo.double"})
    .entry("twice")
    .exit("twice");
}
export function* main(input) {
  const first  = yield ng.callCore("double", {value: input.value}, "double:first");
  const second = yield ng.callCore("double", {value: first.value}, "double:second");
  return {first: first.value, second: second.value};
}
)js";

}  // namespace

int main() {
    Registry registry;
    registry.node("demo.double", [](const std::string&, const json&, const NodeInput& in) {
        return NodeOutput{{ChannelWrite{"value", in.state.get("value").get<int>() * 2}}};
    });
    Host host(std::move(registry).build());

    // compile_admit seals the budget into the version; run() passes the same one.
    const RunBudget run_budget = budget();
    const auto version = host.compile_admit_javascript("double.js", kProgram, run_budget);
    const auto result = host.run(version, {{"value", 21}}, run_budget);

    std::cout << "status: " << to_string(result.status()) << '\n';
    std::cout << "first call: " << result.output()["first"] << '\n';
    std::cout << "second call: " << result.output()["second"] << '\n';
    std::cout << "trace: " << json(result.execution_trace()).dump() << '\n';

    if (result.status() != ProgramTerminalStatus::Completed || result.output()["second"] != 84 ||
        result.execution_trace().size() != 2)
        throw std::runtime_error("the JavaScript Program did not double twice");
    return 0;
}
