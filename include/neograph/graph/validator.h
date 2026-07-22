/**
 * @file graph/validator.h
 * @brief Static semantic analysis over declarative or compiled topology.
 *
 * GraphValidator is the pass layer between parsing (GraphCompiler, M1:
 * every key consumed) and execution (GraphEngine): it checks what the
 * *graph* means, not what the JSON says — dangling references,
 * unreachable nodes, barriers that can never fire, conditional routes
 * that fall into the scheduler's arbitrary lexicographically-last
 * fallback, channel effect violations.
 *
 * Severity philosophy (checker soundness over coverage): a diagnostic
 * is an ERROR only when the construct can never be right under the
 * engine's semantics (dangling name, barrier waiting on a node that
 * has no path to signal it, empty route map = UB at dispatch, write to
 * an undeclared channel = guaranteed runtime throw). Anything a
 * dynamic mechanism could legitimize — Command.goto / Send can reach
 * statically-unreachable nodes — is a WARNING, so a correct graph is
 * never rejected (no false errors), while Studio/tooling can still
 * render every warning as lint.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/compiler.h>
#include <string>
#include <utility>
#include <vector>

namespace neograph::graph {

class GraphRegistry;

/// One static-analysis finding, with a machine-readable witness
/// (counterexample) for tooling to highlight.
struct Diagnostic {
    std::string code;       ///< "E3".."E11" (docs/dsl §5 catalog)
    std::string severity;   ///< "error" | "warning"
    std::string path;       ///< JSON-ish path of the offending construct
    std::string message;    ///< human-readable, self-contained
    json        witness;    ///< machine-readable counterexample
};

struct NEOGRAPH_API ValidationReport {
    std::vector<Diagnostic> diagnostics;

    bool has_errors() const;
    std::vector<const Diagnostic*> errors() const;
    std::vector<const Diagnostic*> warnings() const;
    /// Aggregated multi-line text of all diagnostics (throw/log body).
    std::string summary() const;
};

/**
 * @brief Topology proven free of static semantic errors.
 *
 * Construct through GraphValidator::require_valid(). Warnings remain available
 * for tooling, but the wrapped topology is guaranteed to have no error-level
 * diagnostics at validation time.
 */
class NEOGRAPH_API ValidatedTopology {
public:
    const TopologySpec& topology() const noexcept { return topology_; }
    const ValidationReport& report() const noexcept { return report_; }
    TopologySpec release() && { return std::move(topology_); }

private:
    friend class GraphValidator;

    ValidatedTopology(TopologySpec topology, ValidationReport report)
        : topology_(std::move(topology)), report_(std::move(report)) {}

    TopologySpec topology_;
    ValidationReport report_;
};

class NEOGRAPH_API GraphValidator {
public:
    /// @brief Validate declarative topology before runtime node creation.
    static ValidationReport validate(const TopologySpec& topology);

    /// @brief Validate declarative topology with a local registry overlay.
    static ValidationReport validate(const TopologySpec& topology,
                                     const GraphRegistry& registry);

    /**
     * @brief Validate and wrap a topology, throwing when any error is found.
     */
    static ValidatedTopology require_valid(TopologySpec topology);

    /// @brief Validate and wrap using a local registry overlay.
    static ValidatedTopology require_valid(TopologySpec topology,
                                           const GraphRegistry& registry);

    /**
     * @brief Run all static checks against a compiled graph.
     *
     * Checks (catalog codes from the DSL research doc §5):
     *  - E3  dangling references: edge endpoints, conditional-edge
     *        sources and route targets, interrupt names, barrier
     *        wait_for members must name existing nodes (errors).
     *  - E7  reachability from __start__ (warning — Command.goto/Send
     *        can reach nodes the static edge set cannot).
     *  - E8  barrier liveness: every wait_for member must have a
     *        static edge/route into the barrier node, else the AND-join
     *        can never satisfy (error). Self-wait is E3.
     *  - E9  unsynchronized fan-in: >=2 plain in-edges without a
     *        barrier — double-activation risk when arrival supersteps
     *        differ (warning; XOR-merges are legitimate).
     *  - E10 route completeness against declared ConditionSpecs:
     *        closed conditions must map exactly their label set (dead
     *        route keys / uncovered labels are errors — an uncovered
     *        label falls into the scheduler's lexicographically-last
     *        fallback, an arbitrary target). Open conditions warn on
     *        uncovered known labels. An EMPTY route map is always an
     *        error: dispatch would dereference rend() (UB). Conditions
     *        without a declared spec are skipped.
     *  - E11 no path to __end__ (warning), honoring the scheduler's
     *        implicit rule that a node with no outgoing edges routes
     *        to __end__ — so this fires only for genuinely trapped
     *        cycles.
     *  - E4/E5/E6 channel effects — run ONLY when every node's type
     *        declared an effect contract (one unknown type disables
     *        the whole family; soundness over coverage):
     *        E4 write to undeclared channel (error: the engine throws
     *        at runtime on every execution), read of undeclared
     *        channel (warning: silently yields null);
     *        E5 overwrite-reducer channel written by two direct
     *        fan-out siblings (warning: racy last-writer-wins);
     *        E6 dead channel — declared but never read nor written,
     *        read-only without initial value (warnings).
     *
     * Pure function of the CompiledGraph + registries; does not
     * execute anything.
     */
    static ValidationReport validate(const CompiledGraph& cg);

    /// @brief Validate using a local-first registry overlay.
    static ValidationReport validate(const CompiledGraph& cg, const GraphRegistry& registry);
};

} // namespace neograph::graph
