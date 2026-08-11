// Dispatch-only ProgramPlan benchmark.
//
// This intentionally walks typed dispatch descriptors without constructing a
// GraphEngine, provider, journal, or runtime. It measures immutable plan lookup
// and nested operation-reference scheduling separately from Core execution.
//
// Usage: bench_program_dispatch [iterations] [warmup] [samples]

#include <neograph/program/plan.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
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
                        std::vector<std::string_view>& work) {
    for (const auto& child : dispatch.children) work.push_back(child);
    if (dispatch.then_id) work.push_back(*dispatch.then_id);
    if (dispatch.else_id) work.push_back(*dispatch.else_id);
    if (dispatch.body) work.push_back(*dispatch.body);
    for (const auto& branch : dispatch.branches) work.push_back(branch);
}

std::uint64_t walk_plan(const ProgramPlan& plan,
                        std::vector<std::string_view>& work) {
    std::uint64_t visited = 0;
    work.clear();
    work.push_back(plan.root_id());
    while (!work.empty()) {
        const auto id = work.back();
        work.pop_back();
        const auto* node = plan.find(id);
        if (!node) throw std::runtime_error("benchmark plan reference missing");
        const auto& dispatch = node->dispatch();
        visited += static_cast<std::uint8_t>(dispatch.operation) +
                   dispatch.source_pointer.size();
        enqueue_references(dispatch, work);
    }
    return visited;
}

std::size_t parse_positive(std::string_view text, const char* name) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        throw std::invalid_argument(
            std::string(name) + " must be a positive integer");
    }
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 4) {
            throw std::invalid_argument(
                "usage: bench_program_dispatch [iterations] [warmup] [samples]");
        }
        const auto iterations = argc > 1
            ? parse_positive(argv[1], "iterations") : 100000ULL;
        const auto warmup = argc > 2
            ? parse_positive(argv[2], "warmup") : 10ULL;
        const auto samples = argc > 3
            ? parse_positive(argv[3], "samples") : 5ULL;
        const auto plan = ProgramPlan::from_json(benchmark_plan());
        std::vector<std::string_view> work;
        work.reserve(plan.nodes().size());

        std::uint64_t warmup_visited = 0;
        for (std::uint64_t sample = 0; sample < warmup; ++sample)
            warmup_visited += walk_plan(plan, work);

        std::vector<std::int64_t> timings;
        timings.reserve(samples);
        std::uint64_t visited = warmup_visited;
        for (std::uint64_t sample = 0; sample < samples; ++sample) {
            const auto started = std::chrono::steady_clock::now();
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration)
                visited += walk_plan(plan, work);
            timings.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        }
        std::sort(timings.begin(), timings.end());
        const auto median_ns = timings[timings.size() / 2];
        const double per_walk = static_cast<double>(median_ns) /
                                static_cast<double>(iterations);
        std::cout << "config\titerations\t" << iterations << '\n'
                  << "config\twarmup\t" << warmup << '\n'
                  << "config\tsamples\t" << samples << '\n'
                  << "runtime\tos\tLinux\n"
                  << "runtime\tcompiler\t" << __VERSION__ << '\n'
#ifdef NEOGRAPH_BENCH_BUILD_TYPE
                  << "runtime\tbuild_type\t" << NEOGRAPH_BENCH_BUILD_TYPE << '\n'
#else
                  << "runtime\tbuild_type\tunspecified\n"
#endif
#if defined(NDEBUG)
                  << "runtime\toptimization\toptimized\n"
#else
                  << "runtime\toptimization\tunoptimized\n"
#endif
                  << "header\tworkload\titerations\tmedian_ns\tns_per_walk\n"
                  << "result\tdispatch_only\t" << iterations << '\t'
                  << median_ns << '\t' << per_walk << '\n'
                  << "metric\tvisited\t" << visited << '\n';
        return visited == 0 ? 3 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
