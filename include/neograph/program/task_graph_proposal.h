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
    std::string                            template_id;
    std::string                            content_identity;
    std::vector<std::string>               input_fields;
    std::vector<TaskGraphArtifactContract> output_artifacts;
    TaskGraphBudget                        budget_ceiling;
    std::string                            child_binding;
    std::string                            executable_identity;
    std::string                            kind;
    std::vector<std::string>               capabilities;
    std::vector<std::string>               effects;

    bool operator==(const TaskGraphTemplateContract&) const = default;
};

/** Finite structural and resource ceilings enforced before a proposal is usable. */
struct TaskGraphProposalLimits {
    std::uint64_t    max_tasks = 0;
    std::uint64_t    max_edges = 0;
    std::uint64_t    max_depth = 0;
    TaskGraphBudget  per_task_budget_ceiling;
    TaskGraphBudget  total_budget_ceiling;
    /**
     * Lowest proposal schema version this host accepts. Schema 1 lets a task
     * leave a budget dimension unset; the launched child then receives the
     * link's whole ceiling for it while the aggregate counts it as 0 (#412).
     * A host that takes proposals from a planner sets this to 2, which
     * requires every budget dimension to be explicit.
     */
    std::uint32_t    min_schema_version = 1;
    /// Schema 2: the most attempts one task may declare. 1 means no retries.
    std::uint32_t    max_attempts = 1;
    /// Schema 2: bytes of canonical JSON one literal input may take. 0 refuses literals.
    std::uint32_t    max_literal_bytes = 0;
    /// Schema 2: bytes of canonical JSON all the literals of a proposal may take together.
    std::uint32_t    max_total_literal_bytes = 0;

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

/**
 * A JSON value assigned to one input field of a template (schema 2). It is data, like a
 * binding: the field must be a declared input of the template and receives no other value.
 */
struct TaskGraphInputLiteral {
    TaskGraphInputTarget to;
    json                 value;

    bool operator==(const TaskGraphInputLiteral&) const = default;
};

/** One task vertex in a finite TaskGraphProposal DAG. */
struct TaskGraphTask {
    std::string                        id;
    std::string                        template_id;
    std::vector<TaskGraphInputBinding> input_bindings;
    std::vector<std::string>           depends_on;
    TaskGraphBudget                    budget;
    /// Schema 2; sorted by target field.
    std::vector<TaskGraphInputLiteral> literals;
    /// Schema 2: attempts the task may take, 1 to TaskGraphProposalLimits::max_attempts. Each
    /// attempt has the whole `budget`, so the aggregate counts it once per attempt.
    std::uint32_t                      max_attempts = 1;

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
 * Deeply owned canonical TaskGraphProposal.
 *
 * Equivalent JSON key, task, dependency, and binding order normalizes to the
 * same canonical bytes and identity. The identity deliberately excludes
 * source-map provenance and trusted policy/template details; those remain
 * inputs to later compilation and admission receipts.
 *
 * Schema 1 is the original document. Schema 2 requires every task to state a
 * non-zero amount for each budget dimension in which its template has a
 * non-zero ceiling (`P_PROPOSAL_BUDGET_ZERO`), so that the aggregate counts
 * what each launched task receives, and adds two optional task members:
 * `literals` (JSON values for declared input fields) and `max_attempts`. A
 * schema-1 document keeps its canonical bytes and identity; a document that
 * uses a schema-2 member while declaring schema 1 is `P_PROPOSAL_SCHEMA_VERSION`.
 */
class NEOGRAPH_PROGRAM_API TaskGraphProposal final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION   = 1;
    static constexpr std::uint32_t SCHEMA_VERSION_2 = 2;

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
