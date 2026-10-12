// NeoGraph Example 66: failure routing in a Program
//
// The C++ twin of Python example 32. TEST throws a NodeFailureError with an author code
// (`app.test_failed`). `retryPolicy("TEST", {...})` retries it twice and then, instead of failing
// the Program, routes the failure to ASK_USER, which has `interruptBefore`: the run pauses with a
// structured failure record. The host answers through ProgramRuntime::resume. "retry" runs
// IMPLEMENT and TEST again and the Program completes; "abort" runs ABORT, a failure exit, so
// the Program ends Failed with the declared code and never reports Completed.
// RunBudget::max_node_retries caps retries across the whole run.
//
// Needs -DNEOGRAPH_BUILD_PROGRAM=ON and -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON.
// Usage: ./example_program_failure_routing
//
// smoke-expect: paused at ASK_USER, reason: failure_route
// smoke-expect: record: TEST app.test_failed, exhausted after 3 attempts
// smoke-expect: node retries: 2 of 4
// smoke-expect: operator retries: completed, trace ["ASK_USER","IMPLEMENT","TEST"]
// smoke-expect: operator aborts: failed plan.user_abort at ABORT
// smoke-forbid: [neograph] lint

#include "program_support.h"

#include <iostream>
#include <stdexcept>

using namespace examples::program;

namespace {

constexpr const char* kProgram = R"js(
export function define() {
  return ng.graph("fix")
    .channel("trace", {reducer: "append", initial: []})
    .channel("failures", {reducer: "append", initial: []})
    .channel("decision", {reducer: "overwrite", initial: ""})
    .channel("fixed", {reducer: "overwrite", initial: false})
    .node("IMPLEMENT", {type: "fix.implement"})
    .node("TEST", {type: "fix.test"})
    .node("ASK_USER", {type: "fix.ask_user"})
    .node("ABORT", {type: "fix.implement"})
    .entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST")
    .conditionalEdge("ASK_USER", "fix.decision", {retry: "IMPLEMENT", abort: "ABORT"})
    .retryPolicy("TEST", {max_retries: 2, initial_delay_ms: 10, backoff_multiplier: 1,
                          max_delay_ms: 10, on_failure: "ASK_USER", error_channel: "failures"})
    .interruptBefore("ASK_USER")
    .failureExit("ABORT", {code: "plan.user_abort"});
}
)js";

NodeOutput trace(const std::string& name) {
    return NodeOutput{{ChannelWrite{"trace", json::array({name})}}};
}

}  // namespace

int main() {
    Registry registry;
    registry
        .node("fix.implement",
              [](const std::string& name, const json&, const NodeInput&) { return trace(name); })
        .node("fix.test",
              [](const std::string& name, const json&, const NodeInput& in) {
                  // The failure code is what a record and a retry policy see; the text is not.
                  if (!in.state.get("fixed").get<bool>())
                      throw NodeFailureError("app.test_failed", "2 tests failed");
                  return trace(name);
              })
        .node("fix.ask_user",
              [](const std::string& name, const json&, const NodeInput& in) {
                  const json answer = in.ctx.resume_value.value_or(json::object());
                  auto out = trace(name);
                  out.writes.push_back({"decision", answer.value("decision", "abort")});
                  out.writes.push_back({"fixed", answer.value("fix", false)});
                  return out;
              })
        .condition("fix.decision", {"retry", "abort"},
                   [](const GraphState& state) { return state.get("decision").get<std::string>(); });
    Host host(std::move(registry).build());

    RunBudget run_budget = budget();
    run_budget.max_node_retries = 4;
    const auto version = host.compile_admit_javascript("fix.js", kProgram, run_budget);

    const auto paused = host.run(version, json::object(), run_budget);
    const auto value = paused.interrupt()->pending_input->core_interrupt_value();
    const auto& record = value["failure"];
    std::cout << "paused at " << paused.interrupt()->core_node << ", reason: " << value["reason"].get<std::string>() << '\n';
    std::cout << "record: " << record["node"].get<std::string>() << ' ' << record["code"].get<std::string>() << ", "
              << record["kind"].get<std::string>() << " after " << record["attempts"] << " attempts\n";
    std::cout << "node retries: " << paused.usage().node_retries << " of " << *run_budget.max_node_retries << '\n';

    const auto retried = host.resume(paused, {{"decision", "retry"}, {"fix", true}});
    std::cout << "operator retries: " << to_string(retried.status()) << ", trace "
              << json(retried.execution_trace()).dump() << '\n';

    // A second run of the same version; this time the operator gives up.
    const auto paused_again = host.run(version, json::object(), run_budget);
    const auto aborted = host.resume(paused_again, {{"decision", "abort"}});
    std::cout << "operator aborts: " << to_string(aborted.status()) << ' ' << aborted.failure()->code
              << " at " << aborted.failure()->core_node << '\n';

    const bool ok = paused.status() == ProgramTerminalStatus::Interrupted && !paused.failure() &&
                    record["attempts"] == 3 && record["routed_to"] == "ASK_USER" &&
                    retried.status() == ProgramTerminalStatus::Completed &&
                    aborted.status() == ProgramTerminalStatus::Failed &&
                    aborted.failure()->code == "plan.user_abort" &&
                    aborted.failure()->core_node == "ABORT";
    if (!ok) throw std::runtime_error("failure routing did not behave as described");
    return 0;
}
