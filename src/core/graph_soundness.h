/**
 * @file graph_soundness.h
 * @brief Internal: the opt-in soundness check of a topology's control skeleton (X3).
 *
 * The check explores every run the declared routes allow, with super-steps as
 * the scheduler takes them. A state is the ready set, the signals each join
 * holds (any_of slots included) and, for nodes that declare max_visits, their
 * visit counts. Every route choice is a nondeterministic label: a node with a
 * conditional edge may take any of its routes, and a node with a failure edge
 * (#364) may instead fail and take that edge. A failure exit ends the run.
 *
 * Findings are warnings with a witness trace, the super-steps and route
 * choices that lead there:
 *  - S1 option to complete: from some reachable point no run completes; every
 *       continuation stalls or fails (a point from which runs only loop is S4);
 *  - S2 proper completion: a run ends while a join still holds signals, either
 *       barrier-stalled or by an explicit end that discards them;
 *  - S3 a join that never fires on any run;
 *  - S4 a set of states the run can enter but never leave (a loop with no exit);
 *  - S5 two nodes that write one overwrite channel can be ready in the same
 *       super-step (E5 reports direct fan-out siblings; S5 does not repeat them).
 * A SOUNDNESS summary says how far the check got: a note when it covered every
 * run, a warning when it did not.
 *
 * The check trusts effect contracts: a node type that may return Command.goto
 * or Send declares it with `"control": ["goto"]` or `["send"]`. Runs after such
 * a node becomes ready are not analyzable and are not explored; S1 and S4 are
 * then reported only where every continuation was explored, and S3 not at all.
 * Past the state cap the check stops and reports the graph as not checked;
 * only S2 and S5 findings reached before the cap are kept.
 *
 * Internal header: not installed and without a compatibility promise. Callers
 * outside NeoGraph opt in through ValidationOptions::soundness_check.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/validator.h>

#include <cstddef>
#include <string>
#include <vector>

namespace neograph::graph {

class GraphRegistry;

struct SoundnessOptions {
    /// States the check visits at most. It also tries at most 64 times as many
    /// route combinations. Past either bound the graph is reported not checked.
    std::size_t state_cap = 100000;
    /// Read effect contracts and condition labels from the registry alone,
    /// without the process-global fallback (as GraphValidator::validate_local).
    bool local_only = false;
};

enum class SoundnessStatus {
    Checked,     ///< Every run was explored.
    Partial,     ///< Runs after a node that declares Command.goto or Send were not.
    NotChecked,  ///< The state cap was reached.
};

/// The ways explored runs end. On a partial or capped check, the ones found.
struct SoundnessOutcomes {
    bool completed              = false;  ///< nothing ready and no join holds signals
    bool completed_over_pending = false;  ///< an explicit end discards waiting signals
    bool stalled                = false;  ///< nothing ready, a join holds signals
    bool failure_exit           = false;  ///< a failure exit completed
    bool failed                 = false;  ///< a visit limit with no failure edge
    bool cyclic                 = false;  ///< some run can repeat a state (a step limit)
};

struct SoundnessReport {
    SoundnessStatus status = SoundnessStatus::Checked;
    std::size_t     states      = 0;
    std::size_t     transitions = 0;
    std::size_t     state_cap   = 0;
    SoundnessOutcomes outcomes;
    /// Nodes whose types declare Command.goto or Send and that some run reaches.
    std::vector<std::string> not_analyzable;
    /// S1-S5 warnings, in code order.
    std::vector<Diagnostic> findings;
    /// The SOUNDNESS summary: a note when checked, otherwise a warning.
    Diagnostic summary;
};

/**
 * Explore the control skeleton of a topology that has no validator errors.
 * Pure: it reads the registry's effect contracts and condition specs only.
 */
NEOGRAPH_API SoundnessReport check_soundness(const TopologySpec&     topology,
                                             const GraphRegistry&    registry,
                                             const SoundnessOptions& options = {});

}  // namespace neograph::graph
