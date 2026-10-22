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
#include <neograph/program/store_call.h>
#include <neograph/program/admission.h>
#include <neograph/program/task_graph_proposal.h>

#include <functional>
#include <asio/awaitable.hpp>

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
    std::vector<std::string> input_fields;
    std::vector<TaskGraphArtifactContract> output_artifacts;

    bool operator==(const TaskGraphTemplateReceipt&) const = default;
};

/** A stable operation identity and all typed edges for one dynamic task. */
struct NEOGRAPH_PROGRAM_API CompiledTaskGraphTask {
    std::string                           task_id;
    std::string                           operation_id;
    std::string                           template_id;
    std::string                           template_identity;
    std::string                           child_binding;
    std::vector<TaskGraphInputBinding>    input_bindings;
    std::vector<std::string>              depends_on;
    TaskGraphBudget                       requested_budget;
    TaskGraphBudget                       reserved_budget;
    std::vector<std::string>              capabilities;
    std::vector<std::string>              effects;
    std::vector<std::string>              input_fields;
    std::vector<TaskGraphArtifactContract> output_artifacts;
    /// Schema-2 fragments: JSON values assigned to input fields of each attempt's child input,
    /// after the bindings; sorted by target field.
    std::vector<TaskGraphInputLiteral>    input_literals;
    /// Schema-2 fragments: attempts the task may take. `reserved_budget` is `requested_budget`
    /// once per attempt; every attempt's child starts with the requested budget.
    std::uint32_t                         max_attempts = 1;

    bool operator==(const CompiledTaskGraphTask&) const = default;
};

/**
 * Immutable compiler output.  It is intentionally data-only: no callback,
 * endpoint, credential, provider, or raw Program JSON is carried as executable
 * state.
 */
class NEOGRAPH_PROGRAM_API CompiledTaskGraphFragment final {
public:
    /// Fragment of a schema-1 proposal. Its bytes and identity are unchanged.
    static constexpr std::uint32_t SCHEMA_VERSION = 1;
    /// Fragment of a schema-2 proposal. Every reserved budget dimension is exact:
    /// a task is launched with, and held to, what the aggregate counted (#412).
    static constexpr std::uint32_t SCHEMA_VERSION_2 = 2;

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
    /** Stable identity for this bounded dispatch attempt. */
    std::string attempt_identity;
    /** Durable child lineage, populated before the child result is joined. */
    std::optional<std::string> child_run_id;

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

/**
 * Host-owned durable store boundary for compiled fragments and task state. Every operation is awaitable (#556); a store
 * whose backend is synchronous completes it at once (SyncTaskGraphFragmentStore). The arguments of a call, string views
 * included, must stay alive until the awaitable completes: await it in the full expression that makes the call.
 */
class NEOGRAPH_PROGRAM_API TaskGraphFragmentStore {
public:
    virtual ~TaskGraphFragmentStore() = default;

    [[nodiscard]] virtual asio::awaitable<TaskGraphPublishResult> publish(const TaskGraphFragmentRecord& record) = 0;
    [[nodiscard]] virtual asio::awaitable<std::optional<TaskGraphFragmentRecord>> load(
        std::string_view fragment_id) const = 0;
    /**
     * Locate the published expansion for one immutable parent operation.
     *
     * This lookup is used before reading the proposal source during recovery.
     * A durable fragment is the source of truth after publication; the
     * proposal is only needed on the first execution or when no fragment has
     * crossed the publication boundary.
     */
    [[nodiscard]] virtual asio::awaitable<std::optional<TaskGraphFragmentRecord>> find_published_lineage(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id) const {
        (void)owner_scope;
        (void)parent_run_id;
        (void)expansion_operation_id;
        co_return std::nullopt;
    }
    /**
     * Locate an already published expansion by immutable parent lineage and
     * proposal identity.
     *
     * The default is deliberately empty for legacy stores; such stores remain
     * safe but cannot recover an expansion without replanning.
     */
    [[nodiscard]] virtual asio::awaitable<std::optional<TaskGraphFragmentRecord>> find_published(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id,
        std::string_view proposal_hash) const {
        (void)owner_scope;
        (void)parent_run_id;
        (void)expansion_operation_id;
        (void)proposal_hash;
        co_return std::nullopt;
    }
    [[nodiscard]] virtual asio::awaitable<TaskGraphPublishResult> compare_update(
        std::string_view fragment_id,
        std::uint64_t expected_revision,
        const TaskGraphFragmentRecord& record) = 0;
};

/**
 * Base of a TaskGraphFragmentStore whose backend is synchronous. A derived class implements the `..._sync` operations
 * and gets the awaitable interface from them, as SyncProgramTransitionStore does.
 */
class NEOGRAPH_PROGRAM_API SyncTaskGraphFragmentStore : public TaskGraphFragmentStore {
public:
    // The synchronous operations. Each has the contract of the awaitable operation of the same name.
    virtual TaskGraphPublishResult publish_sync(const TaskGraphFragmentRecord& record) = 0;
    virtual std::optional<TaskGraphFragmentRecord> load_sync(std::string_view fragment_id) const = 0;
    virtual std::optional<TaskGraphFragmentRecord> find_published_lineage_sync(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id) const {
        (void)owner_scope;
        (void)parent_run_id;
        (void)expansion_operation_id;
        return std::nullopt;
    }
    virtual std::optional<TaskGraphFragmentRecord> find_published_sync(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id,
        std::string_view proposal_hash) const {
        (void)owner_scope;
        (void)parent_run_id;
        (void)expansion_operation_id;
        (void)proposal_hash;
        return std::nullopt;
    }
    virtual TaskGraphPublishResult compare_update_sync(
        std::string_view fragment_id,
        std::uint64_t expected_revision,
        const TaskGraphFragmentRecord& record) = 0;

    // The awaitable interface, completed by the operations above: at once on the calling thread,
    // or on a thread of a bounded pool when offloads() is true.
    [[nodiscard]] asio::awaitable<TaskGraphPublishResult> publish(const TaskGraphFragmentRecord& record) final;
    [[nodiscard]] asio::awaitable<std::optional<TaskGraphFragmentRecord>> load(
        std::string_view fragment_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<TaskGraphFragmentRecord>> find_published_lineage(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<TaskGraphFragmentRecord>> find_published(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id,
        std::string_view proposal_hash) const final;
    [[nodiscard]] asio::awaitable<TaskGraphPublishResult> compare_update(
        std::string_view fragment_id,
        std::uint64_t expected_revision,
        const TaskGraphFragmentRecord& record) final;

    /** True when every awaitable operation completes at once on the calling thread, not on a pool thread. */
    bool completes_at_once() const noexcept { return !offloads(); }

protected:
    /** True for a backend that blocks for long: each operation then runs on a pool thread. */
    virtual bool offloads() const noexcept { return false; }
};

/**
 * A TaskGraphFragmentStore for a caller that cannot suspend. It waits for a store that is not synchronous as
 * BlockingProgramTransitionStore does.
 */
class NEOGRAPH_PROGRAM_API BlockingTaskGraphFragmentStore {
public:
    BlockingTaskGraphFragmentStore(std::shared_ptr<TaskGraphFragmentStore> store, BlockingStorePolicy policy = {});
    /** A view of a store that outlives it. */
    BlockingTaskGraphFragmentStore(TaskGraphFragmentStore& store, BlockingStorePolicy policy = {});

    const std::shared_ptr<TaskGraphFragmentStore>& store() const noexcept { return store_; }

    TaskGraphPublishResult publish(const TaskGraphFragmentRecord& record) const;
    std::optional<TaskGraphFragmentRecord> load(std::string_view fragment_id) const;
    std::optional<TaskGraphFragmentRecord> find_published_lineage(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id) const;
    std::optional<TaskGraphFragmentRecord> find_published(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id,
        std::string_view proposal_hash) const;
    TaskGraphPublishResult compare_update(
        std::string_view fragment_id,
        std::uint64_t expected_revision,
        const TaskGraphFragmentRecord& record) const;

private:
    std::shared_ptr<TaskGraphFragmentStore> store_;
    SyncTaskGraphFragmentStore* sync_ = nullptr;
    BlockingStorePolicy policy_;
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
class NEOGRAPH_PROGRAM_API InMemoryTaskGraphFragmentStore final : public SyncTaskGraphFragmentStore {
public:
    InMemoryTaskGraphFragmentStore();
    InMemoryTaskGraphFragmentStore(InMemoryTaskGraphFragmentStore&&) noexcept;
    InMemoryTaskGraphFragmentStore& operator=(InMemoryTaskGraphFragmentStore&&) noexcept;
    InMemoryTaskGraphFragmentStore(const InMemoryTaskGraphFragmentStore&) = delete;
    InMemoryTaskGraphFragmentStore& operator=(const InMemoryTaskGraphFragmentStore&) = delete;
    ~InMemoryTaskGraphFragmentStore() override;

    TaskGraphPublishResult publish_sync(const TaskGraphFragmentRecord& record) override;
    std::optional<TaskGraphFragmentRecord> load_sync(std::string_view fragment_id) const override;
    std::optional<TaskGraphFragmentRecord> find_published_lineage_sync(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id) const override;
    std::optional<TaskGraphFragmentRecord> find_published_sync(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view expansion_operation_id,
        std::string_view proposal_hash) const override;
    TaskGraphPublishResult compare_update_sync(
        std::string_view fragment_id, std::uint64_t expected_revision,
        const TaskGraphFragmentRecord& record) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::program
