/**
 * @file graph/validator.h
 * @brief Static semantic analysis over declarative or compiled topology.
 *
 * GraphValidator is the pass layer between parsing (GraphCompiler, M1:
 * every key consumed) and execution (GraphEngine): it checks what the
 * *graph* means, not what the JSON says — dangling references,
 * unreachable nodes, barriers that can never fire, incomplete conditional
 * routes, and channel effect violations.
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

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neograph::graph {

class GraphRegistry;

/// One static-analysis finding, with a machine-readable witness
/// (counterexample) for tooling to highlight.
struct Diagnostic {
    std::string code;          ///< "E3".."E16" (topology validation catalog)
    std::string severity;      ///< "error" | "warning"
    std::string path;          ///< Legacy JSON-ish display path
    std::string message;       ///< human-readable, self-contained
    json        witness;       ///< machine-readable counterexample
    std::string json_pointer;  ///< RFC 6901 pointer; empty means document root
};

struct NEOGRAPH_API ValidationReport {
    std::vector<Diagnostic> diagnostics;

    bool                           has_errors() const;
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
    const TopologySpec&     topology() const noexcept { return topology_; }
    const ValidationReport& report() const noexcept { return report_; }
    TopologySpec            release() && { return std::move(topology_); }

private:
    friend class GraphValidator;

    ValidatedTopology(TopologySpec topology, ValidationReport report)
        : topology_(std::move(topology)), report_(std::move(report)) {}

    TopologySpec     topology_;
    ValidationReport report_;
};

/**
 * @brief Rule selection for GraphValidator. Defaults apply the current rules.
 */
struct ValidationOptions {
    /**
     * Apply only the rules Program bundles have sealed since bundle format v1.
     * E7/E8/E11 then reason over every declared edge and route, an effects
     * object counts as declared even when it lists neither reads nor writes,
     * and E13-E15 are not reported. Program compilation seals warnings from
     * this rule set so a newer compiler never changes a bundle id; Program
     * admission links with it so sealed bundles keep materializing.
     */
    bool original_rules = false;
    /// Report every cycle in the routed graph as an E16 error, for hosts that
    /// need a DAG (plans, checklists). Loops are otherwise valid.
    bool require_acyclic = false;
};

class NEOGRAPH_API GraphValidator {
public:
    /// @brief Validate declarative topology before runtime node creation.
    static ValidationReport validate(const TopologySpec& topology);

    /// @brief Validate declarative topology with a local registry overlay.
    static ValidationReport validate(const TopologySpec& topology, const GraphRegistry& registry);

    /// @brief Validate topology references without process-global fallback.
    static ValidationReport validate_local(const TopologySpec&  topology,
                                           const GraphRegistry& registry);

    /// @brief Validate with explicit rule selection.
    static ValidationReport validate(const TopologySpec&      topology,
                                     const GraphRegistry&     registry,
                                     const ValidationOptions& options);

    /// @brief Local-registry validation with explicit rule selection.
    static ValidationReport validate_local(const TopologySpec&      topology,
                                           const GraphRegistry&     registry,
                                           const ValidationOptions& options);

    /**
     * @brief Validate and wrap a topology, throwing when any error is found.
     */
    static ValidatedTopology require_valid(TopologySpec topology);

    /// @brief Validate and wrap using a local registry overlay.
    static ValidatedTopology require_valid(TopologySpec topology, const GraphRegistry& registry);

    /// @brief Validate and wrap without process-global fallback.
    static ValidatedTopology require_valid_local(TopologySpec         topology,
                                                 const GraphRegistry& registry);

    /**
     * @brief Run all static checks against a compiled graph.
     *
     * Checks (catalog codes from the topology validation research doc):
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
     *        closed conditions must map every declared label (dead route
     *        keys / uncovered labels are errors). The reserved `default`
     *        route does not replace closed-label coverage. Open conditions
     *        warn on uncovered known labels unless an explicit `default`
     *        handles them. An EMPTY route map is always an error. Conditions
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
     *        write-only unless declared as externally consumed through
     *        an effect contract's optional `exports`, or read-only
     *        without an initial value (warnings). An effect contract
     *        that lists neither reads nor writes counts as undeclared.
     *  - E12 local registry membership: validate_local reports missing
     *        reducers, node types, and conditions as errors instead of throwing.
     *  - E13 a static edge out of a node that also has a conditional edge;
     *        the node routes only through its conditional edge, so the
     *        static edge never fires.
     *  - E14 a second conditional edge from the same node; only a node's
     *        first conditional edge routes.
     *        E13 and E14 are errors for strict documents (schema_version
     *        >= 1) and warnings for legacy documents.
     *  - E15 a barrier member with a route that leads somewhere other than
     *        the join or __end__ (warning): when that route is taken the
     *        join can no longer fire and the run ends BarrierStalled.
     *  - E16 any cycle in the routed graph (error), reported only with
     *        ValidationOptions::require_acyclic.
     *
     * E7, E8 and E11 follow the routes the scheduler takes: a node with a
     * conditional edge contributes only its first conditional edge's routes.
     * ValidationOptions::original_rules selects the rule set Program bundles
     * were first sealed under (no E13-E15, every declared edge counted).
     *
     * Pure function of the CompiledGraph + registries; does not
     * execute anything.
     */
    static ValidationReport validate(const CompiledGraph& cg);

    /// @brief Validate using a local-first registry overlay.
    static ValidationReport validate(const CompiledGraph& cg, const GraphRegistry& registry);

    /// @brief Validate a compiled graph without process-global fallback.
    static ValidationReport validate_local(const CompiledGraph& cg, const GraphRegistry& registry);

    /// @brief Validate a compiled graph with explicit rule selection.
    static ValidationReport validate(const CompiledGraph&     cg,
                                     const GraphRegistry&     registry,
                                     const ValidationOptions& options);

    /**
     * @brief The routed successor table the current rules reason over (X2).
     *
     * A node with a conditional edge maps to the targets of its first one (the
     * only edge the scheduler consults); any other node maps to the targets of
     * its static edges. "__start__" maps to the entry nodes and "__end__"
     * appears as a target. Nodes without successors are absent.
     */
    static std::map<std::string, std::set<std::string>> routed_successors(
        const TopologySpec& topology);
};

}  // namespace neograph::graph
