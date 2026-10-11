/**
 * @file failure_routing.h
 * @brief Internal: the #364 failure routing runtime shared by the engine and the executor.
 *
 * A node whose final failure is routable and that has a failure edge does not
 * fail the run. Its invocation yields a *handled* NodeResult instead: one
 * record written to the edge's error_channel (when it has one) plus a marker
 * write on the reserved channel kFailureRouteChannel that carries the same
 * record. The marker is never applied to state; it is how every path (static
 * nodes, fan-out branches, Sends, replayed pending writes) tells the engine
 * that the invocation routes along its failure edge. Because the marker is an
 * ordinary channel write, the pending write that records a handled invocation
 * needs no store schema change, and resume replays the route without running
 * the node again.
 *
 * Internal header: not installed, no compatibility promise.
 */
#pragma once

#include <neograph/graph/compiler.h>
#include <neograph/graph/types.h>

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::graph::detail {

/// The reserved channel of the marker write. Strict (schema_version >= 1)
/// definitions cannot declare an empty channel name, and only they can have
/// failure edges, so the marker never collides with a real channel.
inline constexpr std::string_view kFailureRouteChannel = "";

/// Free text in a failure record is cut to this many bytes.
inline constexpr std::size_t kFailureTextMaxBytes = 512;

/// The runtime policy of one node that has a sealed per-node retry policy, a
/// failure edge, or both.
struct NodeFailurePlan {
    std::set<std::string>      non_retryable;
    std::optional<int>         max_visits;
    bool                       error_text = false;
    /// The failure edge's target; empty without a failure edge.
    std::string                target;
    std::optional<std::string> error_channel;
};

/// Everything immutable about failure routing in one linked engine.
struct FailureRouting {
    std::map<std::string, NodeFailurePlan> nodes;
    /// Failure-exit node -> its code (NG.Aborted when it declares none).
    std::map<std::string, std::string> exits;
    /// Failure target -> the nodes with max_visits whose failure edge leads
    /// there; their visit counters reset when the target completes.
    std::map<std::string, std::vector<std::string>> visit_resets;
    /// Failure edge source -> target, for the scheduler and stall witnesses.
    std::map<std::string, std::string> targets;
    FailureTextRedactor redactor;

    const NodeFailurePlan* plan(const std::string& node) const {
        const auto found = nodes.find(node);
        return found == nodes.end() ? nullptr : &found->second;
    }
    /// True when some node has a failure edge, i.e. a marker can occur.
    bool routes() const noexcept { return !targets.empty(); }
    /// True when some node declares max_visits, i.e. a run keeps counters.
    bool counts_visits() const noexcept { return visit_limits; }

    bool visit_limits = false;

    static FailureRouting build(const std::map<std::string, NodeRetryPolicySpec>& policies,
                                const std::vector<FailureEdge>&                   edges,
                                const std::map<std::string, FailureExit>&         exits,
                                FailureTextRedactor                               redactor);
};

/**
 * Mutable per-run state: the visit counters of nodes that declare max_visits.
 *
 * Every checkpoint stores the counters as they were before the checkpoint's
 * resume step ran, as `_neograph.node_visits` in its metadata (only nonzero
 * counters of declaring nodes; absent when none), so a resumed step counts
 * its activations exactly once. The engine snapshots the counters at the
 * start of every step for the checkpoints saved during it (a NodeInterrupt).
 */
class FailureRunState {
public:
    /// One activation of `node`. False, without counting, when the node is
    /// past its max_visits: it does not run and takes NG.VisitLimit.
    bool activate(const FailureRouting& routing, const std::string& node);
    /// `target` completed: reset the counters of the nodes routed to it.
    void target_completed(const FailureRouting& routing, const std::string& target);
    /// The current counters as metadata, null when there are none.
    json visits() const;
    /// Snapshot the current counters for checkpoints saved during a step.
    void mark_step_start();
    json step_start_visits() const;
    /// Restore the counters from a checkpoint's metadata.
    void restore(const json& metadata);

private:
    static json encode(const std::map<std::string, int>& visits);

    mutable std::mutex         mutex_;
    std::map<std::string, int> visits_;
    std::map<std::string, int> step_start_;
};

/// The record a handled invocation carries in its marker write, or null.
const json* failure_route_record(const NodeResult& result) noexcept;

/// The writes of `writes` that apply to state: every write but the marker.
/// Returns `writes` itself when it holds no marker.
const std::vector<ChannelWrite>& state_writes(const std::vector<ChannelWrite>& writes,
                                              std::vector<ChannelWrite>&       storage);

/// The structured failure record (#364): node, kind, code, attempts,
/// retryable, step, routed_to, effects, and detail.untrusted_text only when
/// `text` is nonempty.
json failure_record_json(const NodeFailure& failure, int step, const std::string& routed_to,
                         const std::string& text);

}  // namespace neograph::graph::detail
