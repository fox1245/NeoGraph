// NeoGraph Example 67: budgets of a Program
//
// A Program is admitted together with the budget it will run under, and every run must pass
// exactly that RunBudget. Three limits show up in this example, each at a different moment:
//   * The host's ceiling (BudgetLimits) is checked at admission: a Program that asks for more
//     is refused with P_ADMIT_POLICY and never becomes a version.
//   * The admitted budget is checked at start: a run with any other value is refused with
//     P_START_BUDGET before it begins.
//   * The budget is spent while the run goes: a loop that never finishes stops when it has used
//     its max_core_steps, and the Program ends BudgetExhausted with P_CORE_STEP_BUDGET.
// (The Python example 33 spends a model-token budget the same way. Binding a provider to the
// built-in llm_call node is a Python host feature today.)
//
// Needs -DNEOGRAPH_BUILD_PROGRAM=ON and -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON.
// Usage: ./example_program_budgets
//
// smoke-expect: admission above the ceiling: P_ADMIT_POLICY
// smoke-expect: status: budget_exhausted
// smoke-expect: core steps used: 7 of 7
// smoke-expect: another budget at start: P_START_BUDGET
// smoke-forbid: [neograph] lint

#include "program_support.h"

#include <iostream>
#include <stdexcept>

using namespace examples::program;

namespace {

// think -> act -> think -> ... : the condition never lets the loop end.
constexpr const char* kProgram = R"js(
export function define() {
  return ng.graph("agent_loop")
    .channel("trace", {reducer: "append", initial: []})
    .node("think", {type: "loop.step"})
    .node("act", {type: "loop.step"})
    .entry("think").edge("think", "act")
    .conditionalEdge("act", "loop.forever", {again: "think", done: "__end__"});
}
)js";

}  // namespace

int main() {
    Registry registry;
    registry
        .node("loop.step",
              [](const std::string& name, const json&, const NodeInput&) {
                  return NodeOutput{{ChannelWrite{"trace", json::array({name})}}};
              })
        .condition("loop.forever", {"again", "done"},
                   [](const GraphState&) { return std::string("again"); });

    HostOptions options;
    options.ceiling = default_ceiling();
    options.ceiling.max_core_steps = 100;        // no run of this host may be granted more
    Host host(std::move(registry).build(), options);

    // 1. Admission: asking for 500 steps exceeds the ceiling of 100.
    RunBudget greedy = budget();
    greedy.max_core_steps = 500;
    std::string refusal;
    try {
        host.compile_admit_javascript("loop.js", kProgram, greedy);
    } catch (const ProgramAdmissionError& error) {
        refusal = error.diagnostics().front().code;
    }
    std::cout << "admission above the ceiling: " << refusal << '\n';

    // 2. A run that spends its whole budget.
    RunBudget run_budget = budget();
    run_budget.max_core_steps = 7;
    const auto version = host.compile_admit_javascript("loop.js", kProgram, run_budget);
    const auto result = host.run(version, json::object(), run_budget);
    std::cout << "status: " << to_string(result.status()) << ' ' << result.failure()->code << '\n';
    std::cout << "core steps used: " << result.usage().core_steps << " of " << run_budget.max_core_steps << '\n';
    std::cout << "remaining core steps: " << result.remaining_budget().max_core_steps << '\n';

    // 3. The same version under another budget never starts.
    RunBudget other = run_budget;
    other.max_core_steps = 50;
    std::string refused_at_start;
    try {
        host.run(version, json::object(), other);
    } catch (const ProgramDiagnosticError& error) {
        refused_at_start = error.diagnostic().code;
    }
    std::cout << "another budget at start: " << refused_at_start << '\n';

    const bool ok = refusal == "P_ADMIT_POLICY" && refused_at_start == "P_START_BUDGET" &&
                    result.status() == ProgramTerminalStatus::BudgetExhausted &&
                    result.failure()->code == "P_CORE_STEP_BUDGET" &&
                    result.usage().core_steps == 7 && result.remaining_budget().max_core_steps == 0 &&
                    result.execution_trace().size() == 7;
    if (!ok) throw std::runtime_error("budgets did not behave as described");
    return 0;
}
