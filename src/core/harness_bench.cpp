#include <neograph/graph/harness_bench.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/state.h>
#include <neograph/graph/node.h>
#include <neograph/graph/loader.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace neograph::graph {

// ── feedback mode name ──────────────────────────────────────────────

const char* feedback_mode_name(FeedbackMode mode) {
    switch (mode) {
        case FeedbackMode::FULL_DIAGNOSTIC: return "A_full_diagnostic";
        case FeedbackMode::FAIL_ONLY:        return "B_fail_only";
        case FeedbackMode::RUNTIME_SYMPTOMS: return "C_runtime_symptoms";
    }
    return "unknown";
}

// ── parse_task ────────────────────────────────────────────────────────

HarnessTask parse_task(const json& j) {
    HarnessTask t;
    t.name = j.value("name", "unnamed");
    t.description = j.value("description", "");
    t.natural_spec = j.value("natural_spec", "");

    if (j.contains("expected_properties")) {
        const auto& e = j["expected_properties"];
        t.min_nodes = e.value("min_nodes", 0);
        t.max_nodes = e.value("max_nodes", 100);
        t.has_conditional_edges = e.value("has_conditional_edges", false);
        t.has_barrier = e.value("has_barrier", false);
        t.has_interrupt = e.value("has_interrupt", false);
        t.has_tool_calls = e.value("has_tool_calls", false);
        t.has_template_vars = e.value("has_template_vars", false);
    }
    return t;
}

// ── format_feedback ──────────────────────────────────────────────────

Feedback format_feedback(const json& core,
                         const std::string& compile_err,
                         const ValidationReport& validation,
                         FeedbackMode mode,
                         int attempted_routes) {
    Feedback fb;
    fb.converged = compile_err.empty() && !validation.has_errors();

    switch (mode) {
    case FeedbackMode::FULL_DIAGNOSTIC: {
        // Collect full diagnostic text: compile errors + validation.
        std::string prompt;
        if (!compile_err.empty()) {
            prompt += "Compiler error:\n" + compile_err + "\n";
        }
        if (validation.diagnostics.size() > 0) {
            prompt += "Validator diagnostics (E3–E11):\n";
            for (const auto& d : validation.diagnostics) {
                prompt += "  [" + d.code + "] " + d.severity + ": "
                          + d.message + "\n";
                if (d.witness.is_object()) {
                    prompt += "    witness: " + d.witness.dump(2) + "\n";
                }
            }
        }
        if (fb.converged) {
            prompt = "Harness compiles and validates successfully.\n";
        }
        prompt += "\nPlease fix the harness and try again.";
        fb.prompt = prompt;
        fb.estimated_tokens = static_cast<int>(prompt.size() / 4);
        break;
    }

    case FeedbackMode::FAIL_ONLY: {
        // No detail — just signal success/failure.
        if (fb.converged) {
            fb.prompt = "Harness compiles and validates successfully.\n";
        } else {
            std::string prompt;
            prompt = "Compilation failed. Please fix the errors and try again.\n";
            fb.prompt = prompt;
        }
        fb.estimated_tokens = static_cast<int>(fb.prompt.size() / 4);
        break;
    }

    case FeedbackMode::RUNTIME_SYMPTOMS: {
        // Simulate runtime symptoms: the graph compiled but produced
        // wrong output. Describe behavioural observations.
        if (fb.converged) {
            fb.prompt = "Harness executes but produces incorrect output.\n";
            if (attempted_routes >= 0) {
                fb.prompt += "Expected " + std::to_string(attempted_routes)
                          + " conditional routes, but some appear to be "
                          + "silently dropped — the output doesn't match "
                          + "the routing pattern.\n";
            }
            fb.prompt += "The graph runs without errors but the result "
                         "channel has unexpected values. "
                         "Please review the topology for silent drops "
                         "or miswired edges.\n";
        } else {
            std::string prompt;
            prompt = "Compilation failed. Please fix the errors and try again.\n";
            fb.prompt = prompt;
        }
        fb.estimated_tokens = static_cast<int>(fb.prompt.size() / 4);
        break;
    }
    }
    return fb;
}

// ── ConvergenceMetrics::to_json ──────────────────────────────────────

json ConvergenceMetrics::to_json() const {
    json j = json::object();
    j["task_name"] = task_name;
    j["feedback_mode"] = feedback_mode_name(mode);
    j["turns"] = turns;
    j["total_estimated_tokens"] = total_estimated_tokens;
    j["total_errors"] = total_errors;
    j["converged"] = converged;
    j["final_warnings"] = final_warnings;
    j["meets_spec"] = meets_spec;
    return j;
}

// ── Simulation helpers ───────────────────────────────────────────────

namespace {

/// Check if a harness core JSON meets the expected properties.
bool meets_expected_properties(const json& core, const HarnessTask& task) {
    if (!core.is_object()) return false;

    int n_nodes = 0;
    if (core.contains("nodes") && core["nodes"].is_object()) {
        for (auto it = core["nodes"].begin(); it != core["nodes"].end(); ++it)
            n_nodes++;
    }
    if (n_nodes < task.min_nodes || n_nodes > task.max_nodes)
        return false;

    if (task.has_conditional_edges) {
        if (!core.contains("conditional_edges") ||
            !core["conditional_edges"].is_array() ||
            core["conditional_edges"].empty())
            return false;
    }

    if (task.has_barrier) {
        bool has_barrier = false;
        if (core.contains("nodes") && core["nodes"].is_object()) {
            for (auto it = core["nodes"].begin(); it != core["nodes"].end(); ++it) {
                if (it.value().contains("barrier")) { has_barrier = true; break; }
            }
        }
        if (!has_barrier) return false;
    }

    // All checks passed.
    return true;
}

} // anonymous namespace

// ── run_simulation ────────────────────────────────────────────────────

ConvergenceMetrics run_simulation(const HarnessTask& task,
                                   const json& seed_harness,
                                   FeedbackMode mode,
                                   int max_turns,
                                   unsigned int seed) {
    ConvergenceMetrics m;
    m.task_name = task.name;
    m.mode = mode;
    m.converged = false;

    std::mt19937 rng(seed);
    NodeContext ctx;
    json current = json::parse(seed_harness.dump());

    // Evolve-harness mutation operators from evolution.h (if available)
    // or use a simple random JSON mutator for the simulation.
    for (int turn = 0; turn < max_turns; ++turn) {
        m.turns = turn + 1;

        // Compile gate.
        std::string compile_err;
        try {
            auto cg = GraphCompiler::compile(current, ctx);
        } catch (const std::exception& e) {
            compile_err = e.what();
            m.total_errors++;
        }

        // Validate gate (only if compile passed).
        ValidationReport vr;
        if (compile_err.empty()) {
            try {
                auto cg = GraphCompiler::compile(current, ctx);
                vr = GraphValidator::validate(cg);
            } catch (const std::exception&) {
                // Should not happen if compile passed, but be safe.
                compile_err = "unexpected compile failure";
                m.total_errors++;
            }
        }
        if (vr.has_errors()) m.total_errors++;

        // Check convergence.
        bool compile_ok = compile_err.empty();
        bool validate_ok = !vr.has_errors();
        bool spec_ok = compile_ok && validate_ok &&
                       meets_expected_properties(current, task);

        m.converged = compile_ok && validate_ok;

        if (spec_ok) {
            m.converged = true;
            m.meets_spec = true;
            m.final_warnings = static_cast<int>(vr.warnings().size());
            break;
        }

        // Generate feedback to simulate next iteration.
        auto fb = format_feedback(current, compile_err, vr, mode);
        m.total_estimated_tokens += fb.estimated_tokens;

        // For simulation: randomly perturb the harness (simulating LLM fix).
        // In a real experiment, an external script would call an LLM here
        // with fb.prompt as the instruction.
        // Since we don't have an LLM, we just record the feedback and
        // mark as not converged.
        //
        // The simulation "fix" is: if compile failed, try simpler topology.
        if (!compile_ok && m.turns % 2 == 0) {
            // Halve the nodes on even turns when compile fails.
            if (current.contains("nodes") && current["nodes"].is_object()) {
                json new_nodes = json::object();
                int kept = 0;
                for (auto it = current["nodes"].begin();
                     it != current["nodes"].end() && kept < task.max_nodes / 2;
                     ++it, ++kept) {
                    new_nodes[it.key()] = it.value();
                }
                current["nodes"] = std::move(new_nodes);
            }
        } else if (compile_ok && !m.converged) {
            // If compile passes but spec doesn't match, add missing features.
            if (task.has_barrier && !meets_expected_properties(current, task)) {
                // Pick a random node and add barrier.
                std::vector<std::string> names;
                for (auto it = current["nodes"].begin();
                     it != current["nodes"].end(); ++it)
                    names.push_back(it.key());
                if (!names.empty() && current.contains("edges") &&
                    current["edges"].is_array()) {
                    std::string target = names[rng() % names.size()];
                    // Find an upstream node.
                    for (auto eit = current["edges"].begin();
                         eit != current["edges"].end(); ++eit) {
                        json e = *eit;
                        if (e.contains("to") && e["to"] == target) {
                            json wait_arr = json::array();
                            wait_arr.push_back(e["from"]);
                            current["nodes"][target]["barrier"]
                                = json::object();
                            current["nodes"][target]["barrier"]["wait_for"]
                                = std::move(wait_arr);
                            break;
                        }
                    }
                }
            }
        }
    }

    return m;
}

// ── generate_report ──────────────────────────────────────────────────

json generate_report(const std::vector<ConvergenceMetrics>& results) {
    json report = json::object();
    json runs = json::array();

    int total_converged = 0;
    int total_turns = 0;

    for (const auto& r : results) {
        runs.push_back(r.to_json());
        if (r.converged) total_converged++;
        total_turns += r.turns;
    }

    report["runs"] = std::move(runs);
    report["summary"] = json::object();
    report["summary"]["total_runs"] = static_cast<int>(results.size());
    report["summary"]["converged"] = total_converged;
    report["summary"]["avg_turns"] = results.empty()
        ? 0.0
        : static_cast<double>(total_turns) / results.size();

    return report;
}

} // namespace neograph::graph