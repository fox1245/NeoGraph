/**
 * @file program/task_graph_fragment.h
 * @brief Deterministic lowering and durable publication contracts for dynamic task DAGs.
 *
 * TaskGraphProposal is untrusted planner data.  The compiler in this file turns
 * it into a typed, content-addressed fragment after applying the trusted
 * template allow-list, authority intersection, and immutable budget ceilings.
 * A fragment is still not executable until an admission/runtime layer publishes
 * it through TaskGraphFragmentStore.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/admission.h>
#include <neograph/program/task_graph_proposal.h>

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** Immutable trusted facts for one template selected by a proposal. */
struct NEOGRAPH_PROGRAM_API TaskGraphTemplateReceipt {
    std::string              template_id;
    std::string              content_identity;
    std::string              child_binding;
    std::string              executable_identity;
    std::string              kind;
    std::vector<std::string> capabilities;
    std::vector<std::string> effects;

    bool operator==(const TaskGraphTemplateReceipt&) const = default;
};

/** A stable operation identity and all typed edges for one dynamic task. */
struct NEOGRAPH_PROGRAM_API CompiledTaskGraphTask {
    std::string                        task_id;
    std::string                        operation_id;
    std::string                        template_id;
    std::string                        template_identity;
    std::string                        child_binding;
    std::vector<TaskGraphInputBinding> input_bindings;
    std::vector<std::string>           depends_on;
    TaskGraphBudget                    requested_budget;
    TaskGraphBudget                    reserved_budget;
    std::vector<std::string>           capabilities;
    std::vector<std::string>           effects;

    bool operator==(const CompiledTaskGraphTask&) const = default;
};

/**
 * Immutable compiler output.  It is intentionally data-only: no callback,
 * endpoint, credential, provider, or raw Program JSON is carried as executable
 * state.
 */
class NEOGRAPH_PROGRAM_API CompiledTaskGraphFragment final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    CompiledTaskGraphFragment(const CompiledTaskGraphFragment&)            = default;
    CompiledTaskGraphFragment& operator=(const CompiledTaskGraphFragment&) = default;
    CompiledTaskGraphFragment(CompiledTaskGraphFragment&&) noexcept        = default;
    CompiledTaskGraphFragment& operator=(CompiledTaskGraphFragment&&) noexcept = default;
    ~CompiledTaskGraphFragment();

    static CompiledTaskGraphFragment parse(std::string_view stored_bytes);

    std::uint32_t schema_version() const noexcept;
    const std::string& fragment_id() const noexcept;
    const std::string& owner_scope() const noexcept;
    const std::string& parent_run_id() const noexcept;
    const std::string& expansion_operation_id() const noexcept;
    const std::string& parent_program_version_id() const noexcept;
    std::uint32_t child_depth() const noexcept;
    const std::string& proposal_hash() const noexcept;
    const std::vector<CompiledTaskGraphTask>& tasks() const noexcept;
    const TaskGraphJoinPolicy& join() const noexcept;
    const TaskGraphBudget& aggregate_budget() const noexcept;
    const std::vector<TaskGraphTemplateReceipt>& template_receipts() const noexcept;
    const std::vector<std::string>& capability_effect_closure() const noexcept;
    const std::vector<SourceMapEntry>& source_map() const noexcept;

    /** Canonical typed fragment identity input and durable publication payload. */
    json to_json() const;
    const std::string& serialize_canonical() const noexcept;
    bool operator==(const CompiledTaskGraphFragment& other) const noexcept;

private:
    struct Impl;
    explicit CompiledTaskGraphFragment(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;

    friend class TaskGraphFragmentCompiler;
};

/** Durable state of one published task. */
enum class TaskGraphTaskState : std::uint8_t {
    Pending,
    Active,
    Completed,
    Failed,
    Cancelled,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(TaskGraphTaskState state) noexcept;
NEOGRAPH_PROGRAM_API TaskGraphTaskState task_graph_task_state_from_string(std::string_view value);

/** Typed task result; artifact values are data, never authority. */
struct NEOGRAPH_PROGRAM_API TaskGraphTaskRecord {
    std::string         task_id;
    std::string         operation_id;
    TaskGraphTaskState  state = TaskGraphTaskState::Pending;
    std::uint32_t       attempt = 0;
    std::optional<json> output;
    std::optional<json> failure;

    bool operator==(const TaskGraphTaskRecord&) const = default;
};

struct NEOGRAPH_PROGRAM_API TaskGraphFragmentRecord {
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    CompiledTaskGraphFragment fragment;
    std::vector<TaskGraphTaskRecord> tasks;
    std::uint64_t revision = 0;
    bool published = false;
    bool terminal = false;

    static TaskGraphFragmentRecord parse(std::string_view stored_bytes);
    json to_json() const;
    std::string serialize_canonical() const;

    bool operator==(const TaskGraphFragmentRecord&) const = default;
};

/** Result of a compare-and-publish/update operation. */
enum class TaskGraphPublishResult : std::uint8_t {
    Published,
    AlreadyPresent,
    Conflict,
    Missing,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(TaskGraphPublishResult result) noexcept;

/** Host-owned durable store boundary for compiled fragments and task state. */
class NEOGRAPH_PROGRAM_API TaskGraphFragmentStore {
public:
    virtual ~TaskGraphFragmentStore() = default;

    virtual TaskGraphPublishResult publish(const TaskGraphFragmentRecord& record) = 0;
    virtual std::optional<TaskGraphFragmentRecord> load(
        std::string_view fragment_id) const = 0;
    virtual TaskGraphPublishResult compare_update(
        std::string_view fragment_id,
        std::uint64_t expected_revision,
        const TaskGraphFragmentRecord& record) = 0;
};

/** Trusted, host-owned inputs to deterministic proposal lowering. */
struct NEOGRAPH_PROGRAM_API TaskGraphFragmentCompileOptions {
    std::string                 owner_scope;
    std::string                 parent_run_id;
    std::string                 expansion_operation_id;
    std::string                 parent_program_version_id;
    std::uint32_t               child_depth = 0;
    BudgetLimits                remaining_budget;
    TaskGraphProposalLimits     limits;
    std::vector<TaskGraphTemplateContract> template_allowlist;
    std::vector<std::string>    parent_capabilities;
    std::vector<std::string>    parent_effects;
    std::vector<std::string>    host_capabilities;
    std::vector<std::string>    host_effects;
};

/** Host-owned policy facts used to admit one dynamic task-graph expansion. */
struct NEOGRAPH_PROGRAM_API TaskGraphExpansionPolicy {
    TaskGraphProposalLimits             limits;
    std::vector<TaskGraphTemplateContract> template_allowlist;
    std::vector<std::string>            host_capabilities;
    std::vector<std::string>            host_effects;

    bool operator==(const TaskGraphExpansionPolicy&) const = default;
};

using TaskGraphExpansionPolicyResolver =
    std::function<std::optional<TaskGraphExpansionPolicy>(
        std::string_view owner_scope,
        std::string_view parent_program_version_id,
        std::string_view expansion_operation_id)>;

/** Pure compiler from untrusted proposal data to an immutable typed fragment. */
class NEOGRAPH_PROGRAM_API TaskGraphFragmentCompiler final {
public:
    static CompiledTaskGraphFragment compile(
        const TaskGraphProposal& proposal,
        const TaskGraphFragmentCompileOptions& options);

    static CompiledTaskGraphFragment compile(
        const json& proposal,
        TaskGraphProposalOptions proposal_options,
        const TaskGraphFragmentCompileOptions& options);
};

/** Small deterministic store used by tests and embedded hosts. */
class NEOGRAPH_PROGRAM_API InMemoryTaskGraphFragmentStore final : public TaskGraphFragmentStore {
public:
    InMemoryTaskGraphFragmentStore();
    InMemoryTaskGraphFragmentStore(InMemoryTaskGraphFragmentStore&&) noexcept;
    InMemoryTaskGraphFragmentStore& operator=(InMemoryTaskGraphFragmentStore&&) noexcept;
    InMemoryTaskGraphFragmentStore(const InMemoryTaskGraphFragmentStore&) = delete;
    InMemoryTaskGraphFragmentStore& operator=(const InMemoryTaskGraphFragmentStore&) = delete;
    ~InMemoryTaskGraphFragmentStore() override;

    TaskGraphPublishResult publish(const TaskGraphFragmentRecord& record) override;
    std::optional<TaskGraphFragmentRecord> load(std::string_view fragment_id) const override;
    TaskGraphPublishResult compare_update(
        std::string_view fragment_id, std::uint64_t expected_revision,
        const TaskGraphFragmentRecord& record) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::program
