// Dispatch-only ProgramPlan benchmark.
//
// This intentionally walks typed dispatch descriptors without constructing a
// GraphEngine, provider, journal, or runtime. It measures immutable plan lookup
// and nested operation-reference scheduling separately from Core execution.
//
// Usage: bench_program_dispatch [iterations]

#include <neograph/program/plan.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::program;

json benchmark_plan() {
    const auto return_node = [](std::string id) {
        return json{{"id", std::move(id)},
                    {"op", "return"},
                    {"source_pointer", "/benchmark/return"},
                    {"value", true}};
    };
    json operations = json::array(
        {json{{"id", "root"},
              {"op", "sequence"},
              {"source_pointer", "/benchmark/root"},
              {"children", json::array({"call", "branch", "loop", "retry", "parallel",
                                         "race", "quorum", "map", "spawn", "await", "emit",
                                         "checkpoint", "cancel", "ret"})}},
         json{{"id", "call"},
              {"op", "call_core"},
              {"source_pointer", "/benchmark/call"},
              {"core", "main"}},
         json{{"id", "branch"},
              {"op", "branch"},
              {"source_pointer", "/benchmark/branch"},
              {"condition", json{{"path", "/route"}, {"exists", true}}},
              {"then", "ret"},
              {"else", "ret"}},
         json{{"id", "loop"},
              {"op", "loop"},
              {"source_pointer", "/benchmark/loop"},
              {"condition", json{{"path", "/continue"}, {"exists", true}}},
              {"body", "ret"},
              {"max_iterations", std::uint64_t{1}}},
         json{{"id", "retry"},
              {"op", "retry"},
              {"source_pointer", "/benchmark/retry"},
              {"body", "ret"},
              {"max_attempts", std::uint64_t{1}}},
         json{{"id", "parallel"},
              {"op", "parallel"},
              {"source_pointer", "/benchmark/parallel"},
              {"branches", json::array({"ret", "ret"})}},
         json{{"id", "race"},
              {"op", "race"},
              {"source_pointer", "/benchmark/race"},
              {"branches", json::array({"ret", "ret"})}},
         json{{"id", "quorum"},
              {"op", "quorum"},
              {"source_pointer", "/benchmark/quorum"},
              {"branches", json::array({"ret", "ret"})},
              {"min_success", std::uint64_t{1}}},
         json{{"id", "map"},
              {"op", "map"},
              {"source_pointer", "/benchmark/map"},
              {"items", json::array({1})},
              {"body", "ret"}},
         json{{"id", "spawn"},
              {"op", "spawn"},
              {"source_pointer", "/benchmark/spawn"},
              {"child_binding", "demo-child"}},
         json{{"id", "await"},
              {"op", "await"},
              {"source_pointer", "/benchmark/await"},
              {"body", "ret"},
              {"timeout_ms", std::uint64_t{1}}},
         json{{"id", "emit"},
              {"op", "emit"},
              {"source_pointer", "/benchmark/emit"},
              {"value", true}},
         json{{"id", "checkpoint"},
              {"op", "checkpoint"},
              {"source_pointer", "/benchmark/checkpoint"},
              {"body", "ret"}},
         json{{"id", "cancel"},
              {"op", "cancel"},
              {"source_pointer", "/benchmark/cancel"},
              {"scope", "run"},
              {"reason", "benchmark"}},
         return_node("ret")});
    return json{{"root", "root"}, {"operations", std::move(operations)}};
}

void enqueue_references(const ProgramPlanDispatchDescriptor& dispatch,
                        std::vector<std::string_view>&       work) {
    for (const auto& child : dispatch.children) work.push_back(child);
    if (dispatch.then_id) work.push_back(*dispatch.then_id);
    if (dispatch.else_id) work.push_back(*dispatch.else_id);
    if (dispatch.body) work.push_back(*dispatch.body);
    for (const auto& branch : dispatch.branches) work.push_back(branch);
}

}  // namespace

int main(int argc, char** argv) {
    const auto iterations = argc > 1 ? std::stoull(argv[1]) : 100000ULL;
    const auto plan       = ProgramPlan::from_json(benchmark_plan());
    std::vector<std::string_view> work;
    work.reserve(plan.nodes().size());

    std::uint64_t visited = 0;
    const auto   started  = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        work.clear();
        work.push_back(plan.root_id());
        while (!work.empty()) {
            const auto id = work.back();
            work.pop_back();
            const auto* node = plan.find(id);
            if (!node) return 2;
            const auto& dispatch = node->dispatch();
            visited += static_cast<std::uint8_t>(dispatch.operation) +
                       dispatch.source_pointer.size();
            enqueue_references(dispatch, work);
        }
    }
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    const auto per_walk = iterations == 0 ? 0.0
                                          : static_cast<double>(elapsed_ns) /
                                                static_cast<double>(iterations);
    std::cout << "dispatch_only iterations=" << iterations << " visited=" << visited
              << " total_ns=" << elapsed_ns << " ns_per_walk=" << per_walk << '\n';
    return visited == 0 ? 3 : 0;
}
