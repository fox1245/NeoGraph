/**
 * @file graph/evolution.h
 * @brief Harness evolution loop — compiler as evolutionary oracle (issue #80).
 *
 * Three layers:
 *   1. **Mutation operators** — DSL-level (M4) topology mutations that
 *      preserve schema validity. Each operator takes a core topology JSON
 *      and a deterministic seed, returns a mutated core, or nullopt when
 *      the mutation cannot be applied to the given graph.
 *   2. **Task + Scorer** — a fixed-input → expected-output contract, with
 *      a scoring function that evaluates a CompiledGraph against it.
 *   3. **Selection loop** — given N mutations per generation, compile-gate
 *      each (zero-cost), run evaluation on survivors, keep top K.
 *
 * ## Genealogy
 *
 * Every evolutionary run produces a diffable lockfile (the core topology
 * JSON) + a source map (from M4 elaboration). Together they form a
 * lineage that can be git-committed, diffed, and rolled back. The source
 * map ties every generated construct back to the DSL coordinate that
 * produced it.
 *
 * ## Design constraints
 *
 * - **Non-Turing-complete mutation space**: raw JSON mutation is forbidden.
 *   All operators work on M4 DSL concepts (subgraph, template, use,
 *   conditional_edges, barrier toggle). This guarantees a high ratio of
 *   structurally valid offspring.
 * - **Deterministic**: same seed → same mutation sequence. Crucial for
 *   bisection and A/B comparison.
 * - **Zero-cost gate first**: every mutation passes through
 *   GraphCompiler + GraphValidator before evaluation. Invalid offspring
 *   are rejected without execution, and the rejection rate is itself a
 *   metric (high rejection = mutation operator is producing junk).
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/elaborator.h>

#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace neograph::graph {

// =========================================================================
// 1. Mutation operators
// =========================================================================

/// Result of applying a single mutation.
struct NEOGRAPH_API MutationResult {
    /// The mutated core topology JSON. Present iff the mutation was
    /// structurally applicable to the input graph.
    std::optional<json> core;
    /// Human-readable description of what this mutation did (for logging
    /// and genealogy).
    std::string description;
};

/// Mutation operator: deterministic function (input core + rng) → result.
/// The operator reads from `rng` for any stochastic choices; giving the
/// same rng state to the same input produces the same output.
using MutationOp = std::function<MutationResult(const json& core, std::mt19937& rng)>;

/// Catalog of built-in mutation operators, returned by all_operators().
enum class MutationKind : uint8_t {
    /// Swap one template instantiation for another (same interface).
    /// Picks a `use` entry and changes its `template` reference.
    SWAP_TEMPLATE = 0,
    /// Add a new `use` instantiation of an existing template.
    ADD_USE,
    /// Remove one `use` instantiation (leaving at least one node).
    REMOVE_USE,
    /// Tweak a template parameter (`args` value) within the JSON type.
    TUNE_PARAM,
    /// Add or remove a conditional_edges entry on a node.
    TOGGLE_CONDITIONAL_EDGE,
    /// Toggle a barrier on/off for a fan-in node (add or remove the
    /// barrier spec).
    TOGGLE_BARRIER,
    /// Add a new edge from a node to another reachable node.
    ADD_EDGE,
    /// Remove an existing plain edge.
    REMOVE_EDGE,
};

/// Return a vector of one operator per MutationKind. Each operator is a
/// self-contained MutationOp that reads from the provided rng.
NEOGRAPH_API std::vector<MutationOp> all_operators();

// =========================================================================
// 2. Task + Scorer
// =========================================================================

/// A task is a fixed-input → expected-output contract that a compiled
/// graph should satisfy. The task is entirely offline — it does NOT
/// call real LLMs. Nodes in the graph are expected to be deterministic
/// stubs (like M3's PbtNoopNode) for the evaluation to be reproducible.
struct NEOGRAPH_API Task {
    /// Name of the task (for logging/reporting).
    std::string name;
    /// The graph definition (core topology JSON) that solves this task.
    /// This is the *ground truth* — mutations are relative to this or
    /// to a derivative.
    json reference_core;
    /// Input state to feed into the graph.
    json input;
    /// Expected output state (channel values after execution).
    json expected_output;
    /// Expected number of super-steps (approx).
    int expected_super_steps = 0;
};

/// Score of a single graph run against a task.
struct NEOGRAPH_API Score {
    /// 0.0 = perfect, higher = worse. Negative means invalid/unrunnable.
    double cost = -1.0;
    /// Human-readable summary.
    std::string summary;
    /// Whether this graph is functionally correct (output matches expected).
    bool correct = false;
    /// Compiler gate metrics.
    bool compiled = false;
    bool validated = false;
    bool executed = false;
};

/// Scorer function: compile-gate + run + compare against task.
/// Returns a Score.
NEOGRAPH_API Score evaluate(const json& core, const Task& task,
                            const NodeContext& ctx);

// =========================================================================
// 3. Generation / Selection loop
// =========================================================================

/// Parameters for one evolution generation.
struct NEOGRAPH_API EvolutionConfig {
    /// Number of offspring to produce per generation.
    int offspring_per_gen = 20;
    /// Keep top K survivors after evaluation.
    int survivors_per_gen = 5;
    /// Maximum generations to run.
    int max_generations = 10;
    /// Seed for deterministic reproducibility.
    uint64_t seed = 42;
    /// When true, runs the evaluation (requires GraphEngine). When false,
    /// only the compile gate is applied (dry-run for operator testing).
    bool run_evaluation = false;
};

/// One individual in the population.
struct NEOGRAPH_API Individual {
    /// Core topology JSON (the "lockfile").
    json core;
    /// Source map from elaboration, if available.
    json sourcemap;
    /// Generation this individual was born in.
    int generation = 0;
    /// Parent index within the previous generation (-1 = seed).
    int parent_index = -1;
    /// Mutation that produced this individual.
    std::string mutation_description;
    /// Score from the most recent evaluation. Negative = not evaluated.
    Score score;
};

/// Result of one evolution run.
struct NEOGRAPH_API EvolutionResult {
    /// All individuals across all generations (in birth order).
    std::vector<Individual> population;
    /// Best individual (lowest cost).
    Individual best;
    /// Compile-gate statistics.
    int total_offspring = 0;
    int compile_passed = 0;
    int execute_passed = 0;
};

/// Run the evolution loop.
NEOGRAPH_API EvolutionResult evolve(
    const json& seed_core,
    const Task& task,
    const EvolutionConfig& config = EvolutionConfig{});

/// Serialize an EvolutionResult as a JSON lineage document.
/// Contains the full genealogy, compile-gate stats, and best individual.
NEOGRAPH_API json to_json(const EvolutionResult& result);

} // namespace neograph::graph
