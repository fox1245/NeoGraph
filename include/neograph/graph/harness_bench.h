/**
 * @file graph/harness_bench.h
 * @brief Harness convergence benchmark — A/B/C feedback modes (issue #81).
 *
 * Measures the real-world value of compiler diagnostics by comparing three
 * feedback regimes when an LLM iteratively corrects a generated harness:
 *
 *   A. Full diagnostic:  strict compiler error text + witness JSON + validator
 *                         warnings. Every error carries a source coordinate and
 *                         a machine-readable counterexample.
 *   B. Fail-only:        "compile failed, try again" — no detail. Baseline.
 *   C. Runtime symptoms: compile succeeds, graph runs, outputs are wrong.
 *                         Simulates the v0.1.x silent-drop era.
 *
 * The framework does NOT call an LLM — it provides the feedback formatting,
 * metrics collection, and convergence measurement. Run with an external script
 * that feeds LLM-generated harnesses through compile() -> feedback -> iterate.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/elaborator.h>

#include <string>
#include <vector>

namespace neograph::graph {

// ── Feedback mode ────────────────────────────────────────────────────

enum class NEOGRAPH_API FeedbackMode : uint8_t {
    FULL_DIAGNOSTIC = 0,  ///< A: full error + witness (agent-optimized)
    FAIL_ONLY       = 1,  ///< B: "failed" only (baseline)
    RUNTIME_SYMPTOMS= 2,  ///< C: runtime symptom simulation
};

/// Human-readable name for each feedback mode.
NEOGRAPH_API const char* feedback_mode_name(FeedbackMode mode);

// ── Harness task ─────────────────────────────────────────────────────

/// A benchmark task: natural language spec converted into a harness.
/// The expected_properties describe what a correct harness should look like.
struct NEOGRAPH_API HarnessTask {
    std::string name;
    std::string description;
    std::string natural_spec;

    /// Expected structural properties (used for convergence detection).
    int min_nodes = 0;
    int max_nodes = 100;
    bool has_conditional_edges = false;
    bool has_barrier = false;
    bool has_interrupt = false;
    bool has_tool_calls = false;
    bool has_template_vars = false;
};

/// Parse a HarnessTask from a JSON fixture file.
NEOGRAPH_API HarnessTask parse_task(const json& j);

// ── Feedback ─────────────────────────────────────────────────────────

/// One round of feedback from the compiler to the LLM.
struct NEOGRAPH_API Feedback {
    /// The prompt to send to the LLM (varies by FeedbackMode).
    std::string prompt;
    /// Token count estimate (characters / ~4 for this framework).
    int estimated_tokens = 0;
    /// Whether the harness compiled+validated successfully.
    bool converged = false;
};

/// Format feedback for a given mode from a compile+validate result.
/// @param core         The harness topology JSON submitted by the LLM.
/// @param compile_err  Empty if compile succeeded, else the exception text.
/// @param validation   Validation report (may be empty if compile failed).
/// @param mode         Which feedback regime to format.
/// @param attempted_routes  For C (runtime symptoms): number of routes that
///                          silently dropped or misrouted. -1 = unknown.
NEOGRAPH_API Feedback format_feedback(
    const json& core,
    const std::string& compile_err,
    const ValidationReport& validation,
    FeedbackMode mode,
    int attempted_routes = -1);

// ── Convergence metrics ──────────────────────────────────────────────

/// Metrics for one convergence attempt.
struct NEOGRAPH_API ConvergenceMetrics {
    /// Task that was being solved.
    std::string task_name;
    /// Which feedback mode was used.
    FeedbackMode mode = FeedbackMode::FULL_DIAGNOSTIC;
    /// How many LLM turns were needed to converge (or max).
    int turns = 0;
    /// Estimated total tokens consumed across all turns.
    int total_estimated_tokens = 0;
    /// Number of compile+validate errors across all turns.
    int total_errors = 0;
    /// Whether the final harness converged (compile+validate passed).
    bool converged = false;
    /// Final validator warning count.
    int final_warnings = 0;
    /// Whether the final harness meets all expected properties.
    bool meets_spec = false;

    json to_json() const;
};

// ── Benchmark runner (simulation) ────────────────────────────────────

/// Run a simulated convergence benchmark against a single task.
///
/// This does NOT call an LLM. Instead, it generates candidate harnesses
/// by applying random mutations to the seed_harness, then measures how
/// many "turns" each feedback mode takes to converge. This gives a
/// lower-bound estimate of diagnostic value without LLM costs.
///
/// For real LLM experiments, the external script should call
/// format_feedback() per turn and record metrics.
NEOGRAPH_API ConvergenceMetrics run_simulation(
    const HarnessTask& task,
    const json& seed_harness,
    FeedbackMode mode,
    int max_turns = 20,
    unsigned int seed = 42);

/// Generate a convergence report as JSON.
NEOGRAPH_API json generate_report(
    const std::vector<ConvergenceMetrics>& results);

} // namespace neograph::graph
