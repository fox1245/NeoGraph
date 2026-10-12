// NeoGraph Example 68: pause, resume and fork a Program run
//
// The C++ twin of Python examples 34 and 35.
//
// Part 1. `interruptBefore("b")` pauses a run before node b. The result is Interrupted and its
// `interrupt()` carries the pending call that ProgramRuntime::resume answers. The node receives
// the answer as `in.ctx.resume_value`. The resumed result lists only the Core nodes that ran
// after the pause.
//
// Part 2. A run pauses while `check` waits for approval, next to a `draft` node that has already
// finished (it stands for an expensive call). ProgramRuntime::fork starts a second run from that
// exact checkpoint with another answer. The finished node is replayed from the checkpoint in
// both runs: its body ran once in total. The fork's budget is carved out of what the paused
// run has left, so it gets a share, not all.
//
// Needs -DNEOGRAPH_BUILD_PROGRAM=ON and -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON.
// Usage: ./example_program_pause_resume_fork
//
// smoke-expect: paused: interrupted before b, trace ["a"]
// smoke-expect: resumed: completed, trace ["b","c"]
// smoke-expect: paused with draft ran 1 time
// smoke-expect: fork answers approved: completed, verdict "approved"
// smoke-expect: original answers rejected: completed, verdict "rejected"
// smoke-expect: draft body ran: 1
// smoke-forbid: [neograph] lint

#include "program_support.h"

#include <atomic>
#include <iostream>
#include <stdexcept>

using namespace examples::program;

namespace {

constexpr const char* kPipeline = R"js(
export function define() {
  return ng.graph("pipeline")
    .channel("trace", {reducer: "append", initial: []})
    .channel("answer", {reducer: "overwrite", initial: null})
    .node("a", {type: "steps.run"})
    .node("b", {type: "steps.run", takes_answer: true})
    .node("c", {type: "steps.run"})
    .entry("a").edge("a", "b").edge("b", "c").exit("c")
    .interruptBefore("b");
}
export function* main(input) {
  const reply = yield ng.callCore("pipeline", input, "pipeline:main");
  return {answer: reply.answer, trace: reply.trace};
}
)js";

constexpr const char* kReview = R"js(
export function define() {
  return ng.graph("review")
    .channel("trace", {reducer: "append", initial: []})
    .channel("verdict", {reducer: "overwrite", initial: null})
    .node("start", {type: "steps.run"})
    .node("draft", {type: "review.draft"})
    .node("check", {type: "review.check"})
    .entry("start").edge("start", "draft").edge("start", "check")
    .exit("draft").exit("check");
}
export function* main(input) {
  const reply = yield ng.callCore("review", input, "review:main");
  return {verdict: reply.verdict};
}
)js";

std::atomic<int> draft_runs{0};

}  // namespace

int main() {
    Registry registry;
    registry
        .node("steps.run",
              [](const std::string& name, const json& config, const NodeInput& in) {
                  NodeOutput out{{ChannelWrite{"trace", json::array({name})}}};
                  if (config.value("takes_answer", false))
                      out.writes.push_back({"answer", in.ctx.resume_value.value_or(json())});
                  return out;
              })
        .node("review.draft",
              [](const std::string& name, const json&, const NodeInput&) {
                  ++draft_runs;                      // the "expensive" call
                  return NodeOutput{{ChannelWrite{"trace", json::array({name})}}};
              })
        .node("review.check",
              [](const std::string&, const json&, const NodeInput& in) -> NodeOutput {
                  if (!in.ctx.resume_value)          // first time: ask and pause
                      throw NodeInterrupt("needs approval", json{{"question", "publish the draft?"}});
                  return NodeOutput{{ChannelWrite{"verdict", *in.ctx.resume_value}}};
              });
    HostOptions options;
    options.worker_count = 2;                         // draft and check run in one super-step
    Host host(std::move(registry).build(), options);
    const RunBudget run_budget = budget();

    // Part 1: pause and resume.
    const auto pipeline = host.compile_admit_javascript("pipeline.js", kPipeline, run_budget);
    const auto paused = host.run(pipeline, json::object(), run_budget);
    std::cout << "paused: " << to_string(paused.status()) << " before " << paused.interrupt()->core_node
              << ", trace " << json(paused.execution_trace()).dump() << '\n';
    const auto resumed = host.resume(paused, "ship it");
    std::cout << "resumed: " << to_string(resumed.status()) << ", trace "
              << json(resumed.execution_trace()).dump() << '\n';
    std::cout << "main() returned: " << resumed.output().dump() << '\n';

    // Part 2: fork a paused run next to a finished node.
    const auto review = host.compile_admit_javascript("review.js", kReview, run_budget);
    const auto waiting = host.run(review, json::object(), run_budget);
    std::cout << "paused with draft ran " << draft_runs.load() << " time\n";

    RunBudget share = budget(4'000);
    share.max_concurrency = 2;
    share.max_program_operations = 8;
    share.max_core_steps = 20;
    const auto forked = host.fork(waiting, "approved", review, share);
    std::cout << "fork answers approved: " << to_string(forked.status()) << ", verdict "
              << forked.output()["verdict"] << '\n';
    const auto original = host.resume(waiting, "rejected");
    std::cout << "original answers rejected: " << to_string(original.status()) << ", verdict "
              << original.output()["verdict"] << '\n';
    std::cout << "draft body ran: " << draft_runs.load() << '\n';

    const bool ok = paused.execution_trace() == std::vector<std::string>{"a"} &&
                    resumed.execution_trace() == std::vector<std::string>{"b", "c"} &&
                    resumed.output() == json{{"answer", "ship it"}, {"trace", {"a", "b", "c"}}} &&
                    forked.run_id() != waiting.run_id() && forked.output()["verdict"] == "approved" &&
                    original.output()["verdict"] == "rejected" && draft_runs.load() == 1;
    if (!ok) throw std::runtime_error("pause, resume and fork did not behave as described");
    return 0;
}
