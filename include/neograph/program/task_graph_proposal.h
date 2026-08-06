/**
 * @file program/task_graph_proposal.h
 * @brief Immutable, non-executable TaskGraphProposal-v1 values.
 *
 * A TaskGraphProposal is planner-produced data. Parsing validates only the
 * finite task DAG and its declared template/data/budget bounds; it neither
 * resolves executable targets nor admits, publishes, or dispatches work.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/source.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** Requested resource budget for one proposed task or a proposal ceiling. */
struct TaskGraphBudget {
    std::uint64_t wall_time_ms         = 0;
    std::uint64_t model_tokens         = 0;
    std::uint64_t monetary_microunits  = 0;
    std::uint64_t output_bytes         = 0;

    bool operator==(const TaskGraphBudget&) const = default;
};

/** Exact field contract for one named artifact emitted by a sealed template. */
struct TaskGraphArtifactContract {
    std::string              artifact;
    std::vector<std::string> fields;

    bool operator==(const TaskGraphArtifactContract&) const = default;
};

/**
 * Immutable-template facts supplied by the trusted source/admission layer.
 *
 * `template_id` is the only template selector that proposal JSON may use.
 * `content_identity` binds that selector to an exact sealed template version;
 * no provider, tool, endpoint, credential, or executable target is present in
 * the proposal contract.
 */
struct TaskGraphTemplateContract {
    std::string                          template_id;
    std::string                          content_identity;
    std::vector<std::string>             input_fields;
    std::vector<TaskGraphArtifactContract> output_artifacts;
    TaskGraphBudget                      budget_ceiling;

    bool operator==(const TaskGraphTemplateContract&) const = default;
};

/** Finite structural and resource ceilings enforced before a proposal is usable. */
struct TaskGraphProposalLimits {
    std::uint64_t    max_tasks = 0;
    std::uint64_t    max_edges = 0;
    std::uint64_t    max_depth = 0;
    TaskGraphBudget  per_task_budget_ceiling;
    TaskGraphBudget  total_budget_ceiling;

    bool operator==(const TaskGraphProposalLimits&) const = default;
};

/** Trusted parsing context for one proposal artifact.
 *
 * `source_map` addresses the submitted JSON. On success, TaskGraphProposal
 * remaps it onto canonical `to_json()` coordinates, including reordered
 * tasks, dependencies, and bindings.
 */
struct TaskGraphProposalOptions {
    std::string                            source_id;
    std::vector<SourceMapEntry>            source_map;
    TaskGraphProposalLimits                limits;
    std::vector<TaskGraphTemplateContract> template_allowlist;
};

/** Reference to one declared producer field. */
struct TaskGraphArtifactReference {
    std::string task_id;
    std::string artifact;
    std::string field;

    bool operator==(const TaskGraphArtifactReference&) const = default;
};

/** Target field accepted by the consuming sealed template. */
struct TaskGraphInputTarget {
    std::string field;

    bool operator==(const TaskGraphInputTarget&) const = default;
};

/** Typed dataflow edge; it is data selection, never expression evaluation. */
struct TaskGraphInputBinding {
    TaskGraphArtifactReference from;
    TaskGraphInputTarget       to;

    bool operator==(const TaskGraphInputBinding&) const = default;
};

/** One task vertex in a finite TaskGraphProposal DAG. */
struct TaskGraphTask {
    std::string                        id;
    std::string                        template_id;
    std::vector<TaskGraphInputBinding> input_bindings;
    std::vector<std::string>           depends_on;
    TaskGraphBudget                    budget;

    bool operator==(const TaskGraphTask&) const = default;
};

enum class TaskGraphJoinKind : std::uint8_t { All };

NEOGRAPH_PROGRAM_API std::string_view to_string(TaskGraphJoinKind kind) noexcept;
NEOGRAPH_PROGRAM_API TaskGraphJoinKind task_graph_join_kind_from_string(std::string_view value);

/** v1 intentionally supports only all-of joins; additional policies need their own contract. */
struct TaskGraphJoinPolicy {
    TaskGraphJoinKind kind = TaskGraphJoinKind::All;

    bool operator==(const TaskGraphJoinPolicy&) const = default;
};

/** Typed structural rejection with stable Program diagnostics and coordinates. */
class NEOGRAPH_PROGRAM_API TaskGraphProposalError final : public std::runtime_error {
public:
    explicit TaskGraphProposalError(std::vector<Diagnostic> diagnostics);
    const std::vector<Diagnostic>& diagnostics() const noexcept;

private:
    std::vector<Diagnostic> diagnostics_;
};

/**
 * Deeply owned canonical TaskGraphProposal-v1.
 *
 * Equivalent JSON key, task, dependency, and binding order normalizes to the
 * same canonical bytes and identity. The identity deliberately excludes
 * source-map provenance and trusted policy/template details; those remain
 * inputs to later compilation and admission receipts.
 */
class NEOGRAPH_PROGRAM_API TaskGraphProposal final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    TaskGraphProposal(const TaskGraphProposal&)            = default;
    TaskGraphProposal& operator=(const TaskGraphProposal&) = default;
    TaskGraphProposal(TaskGraphProposal&&) noexcept        = default;
    TaskGraphProposal& operator=(TaskGraphProposal&&) noexcept = default;
    ~TaskGraphProposal();

    static TaskGraphProposal parse(const json& proposal, TaskGraphProposalOptions options);

    std::uint32_t                           schema_version() const noexcept;
    const std::string&                      source_id() const noexcept;
    const std::vector<SourceMapEntry>&      source_map() const noexcept;
    const std::vector<TaskGraphTask>&       tasks() const noexcept;
    const TaskGraphJoinPolicy&              join() const noexcept;
    const std::string&                      id() const noexcept;
    const std::string&                      proposal_hash() const noexcept;
    json                                    to_json() const;
    const std::string&                      serialize_canonical() const noexcept;

private:
    struct Impl;
    explicit TaskGraphProposal(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
