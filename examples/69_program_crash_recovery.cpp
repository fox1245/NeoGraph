// NeoGraph Example 69: crash recovery of a Program run
//
// The C++ twin of Python example 36. A Host built on SQLite files keeps versions, run records
// and checkpoints on disk. Process A runs a Program to a question, is answered, charges an
// effect and then dies without ending the run (std::_Exit). Process B opens the same
// directory with the same registry and calls ProgramRuntime::reconnect(owner, run_id): the
// run continues from its last durable checkpoint and finishes on its second attempt.
//
// The run restarts from the checkpoint the interrupt left, so the node that already charged
// runs again: recovery is at-least-once for node bodies. The Python example keeps a model call
// exactly-once with the provider-call journal. For any other effect in a node, make it
// idempotent: here `charge` checks a ledger for its order id before it applies the effect.
// Child budgets (max_child_depth) are what let a run be recovered from its checkpoint.
//
// The example starts itself twice with std::system, so it needs a Linux or macOS shell.
// Needs -DNEOGRAPH_BUILD_PROGRAM=ON, -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON and SQLite.
// Usage: ./example_program_crash_recovery
//
// smoke-os: linux, darwin
// smoke-expect: process A: paused, answered, ran charge 1 time, then died
// smoke-expect: process B: completed on attempt 2, trace ["ask","charge","finish"]
// smoke-expect: charge ran 2 times; the ledger holds 1 charge
// smoke-forbid: [neograph] lint

#include "program_support_sqlite.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace examples::program;
namespace fs = std::filesystem;

namespace {

constexpr const char* kProgram = R"js(
export function define() {
  return ng.graph("order")
    .channel("trace", {reducer: "append", initial: []})
    .node("ask", {type: "order.step"})
    .node("charge", {type: "order.charge"})
    .node("finish", {type: "order.finish"})
    .entry("ask").edge("ask", "charge").edge("charge", "finish").exit("finish")
    .interruptBefore("ask");
}
)js";

fs::path g_directory;       // the shared directory: SQLite files, run id, ledger
bool g_die = false;         // process A only: die inside `finish`

int lines_of(const char* file) {
    std::ifstream log(g_directory / file);
    int lines = 0;
    for (std::string line; std::getline(log, line);) ++lines;
    return lines;
}

NodeOutput traced(const std::string& name) {
    return NodeOutput{{ChannelWrite{"trace", json::array({name})}}};
}

// Child budgets are what let a run be recovered from its checkpoint after a crash.
RunBudget run_budget() {
    RunBudget value = budget();
    value.max_child_depth = 1;
    value.max_total_children = 1;
    return value;
}

Host open_host() {
    Registry registry;
    registry
        .node("order.step", [](const std::string& name, const json&, const NodeInput&) { return traced(name); })
        .node("order.charge",
              [](const std::string& name, const json&, const NodeInput&) {
                  std::ofstream(g_directory / "runs.log", std::ios::app) << "ran\n";
                  if (lines_of("ledger.log") == 0)       // idempotent: apply the charge for order-17 once
                      std::ofstream(g_directory / "ledger.log", std::ios::app) << "order-17\n";
                  return traced(name);
              })
        .node("order.finish", [](const std::string& name, const json&, const NodeInput&) {
            if (g_die) std::_Exit(9);                  // the process dies before the run ends
            return traced(name);
        });
    HostOptions options;
    options.ceiling.max_child_depth = 1;
    options.ceiling.max_total_children = 1;
    return Host(std::move(registry).build(), options, sqlite_stores(g_directory));
}

int process_a() {
    auto host = open_host();
    const auto version = host.compile_admit_javascript("order.js", kProgram, run_budget());
    const auto paused = host.run(version, json::object(), run_budget());
    std::ofstream(g_directory / "run_id.txt") << paused.run_id();
    g_die = true;
    host.resume(paused, json::object());               // never returns: `finish` exits the process
    return 1;
}

int process_b() {
    auto host = open_host();
    std::string run_id;
    std::ifstream(g_directory / "run_id.txt") >> run_id;
    const auto result = host.runtime().reconnect(host.owner(), run_id).wait();
    std::cout << "process B: " << to_string(result.status()) << " on attempt " << result.attempt()
              << ", trace " << json(result.execution_trace()).dump() << std::endl;
    return result.status() == ProgramTerminalStatus::Completed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3) {                                   // a child: <phase> <directory>
        g_directory = argv[2];
        return std::string(argv[1]) == "first" ? process_a() : process_b();
    }
    g_directory = fs::temp_directory_path() / ("ng-program-recovery-" + std::to_string(std::random_device{}()));
    fs::create_directories(g_directory);
    const std::string self = "\"" + fs::absolute(argv[0]).string() + "\" ";
    const std::string where = " \"" + g_directory.string() + "\"";

    const int died = std::system((self + "first" + where).c_str());
    std::cout << "process A: paused, answered, ran charge " << lines_of("runs.log") << " time, then died" << std::endl;
    const int recovered = std::system((self + "recover" + where).c_str());
    const int ran = lines_of("runs.log"), charged = lines_of("ledger.log");
    std::cout << "charge ran " << ran << " times; the ledger holds " << charged << " charge" << std::endl;

    fs::remove_all(g_directory);
    if (died == 0 || recovered != 0 || ran != 2 || charged != 1)
        throw std::runtime_error("crash recovery did not behave as described");
    return 0;
}
