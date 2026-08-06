/**
 * @file program/plan.h
 * @brief Typed immutable orchestration plans used by ProgramRuntime.
 *
 * A ProgramPlan is a compact, read-only scheduling graph.  It is deliberately
 * not bytecode: values remain JSON at the Core/adapter boundary while control
 * references, operation tags, bounds, and source pointers are typed.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class ProgramOperationKind : std::uint8_t {
    CallCore,
    Sequence,
    Branch,
    Loop,
    Retry,
    Parallel,
    Race,
    Quorum,
    Map,
    Spawn,
    Await,
    Emit,
    Checkpoint,
    Cancel,
    Return,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramOperationKind kind) noexcept;
NEOGRAPH_PROGRAM_API ProgramOperationKind
program_operation_kind_from_string(std::string_view value);

/**
 * Immutable, typed dispatch metadata for one Program operation.
 *
 * The identifiers are resolved and validated while the plan is sealed.  An
 * empty optional is meaningful only for operation kinds which do not use that
 * reference; callers never need to parse the lowered JSON to dispatch.
 */
struct NEOGRAPH_PROGRAM_API ProgramPlanDispatchDescriptor {
    ProgramOperationKind          operation = ProgramOperationKind::CallCore;
    std::string                   source_pointer;
    std::vector<std::string>      children;
    std::optional<std::string>    then_id;
    std::optional<std::string>    else_id;
    std::optional<std::string>    body;
    std::optional<std::string>    child_binding;
    std::vector<std::string>      branches;
    bool operator==(const ProgramPlanDispatchDescriptor&) const = default;
};

using ProgramPlanDispatch = ProgramPlanDispatchDescriptor;

/** One immutable operation and its statically lowered references. */
class NEOGRAPH_PROGRAM_API ProgramPlanNode final {
public:
    ProgramPlanNode(const ProgramPlanNode&)            = default;
    ProgramPlanNode& operator=(const ProgramPlanNode&) = default;
    ProgramPlanNode(ProgramPlanNode&&) noexcept        = default;
    ProgramPlanNode& operator=(ProgramPlanNode&&) noexcept = default;
    ~ProgramPlanNode();

    const std::string& id() const noexcept;
    ProgramOperationKind operation() const noexcept;
    const ProgramPlanDispatchDescriptor& dispatch() const noexcept;
    const ProgramPlanDispatchDescriptor& dispatch_descriptor() const noexcept;
    const std::string& source_pointer() const noexcept;
    const std::optional<std::string>& core() const noexcept;
    const std::vector<std::string>& children() const noexcept;
    const std::optional<std::string>& then_id() const noexcept;
    const std::optional<std::string>& else_id() const noexcept;
    const std::optional<std::string>& body() const noexcept;
    const std::optional<std::string>& child_binding() const noexcept;
    const std::vector<std::string>& branches() const noexcept;
    const std::optional<std::uint64_t>& max_iterations() const noexcept;
    const std::optional<std::uint64_t>& max_attempts() const noexcept;
    const std::optional<std::uint64_t>& min_success() const noexcept;
    const std::optional<std::uint64_t>& timeout_ms() const noexcept;
    const std::optional<std::string>& scope() const noexcept;
    const std::optional<std::string>& reason() const noexcept;

    /** Authored condition/value/items are returned as owned immutable copies. */
    json condition() const;
    json value() const;
    json items() const;

    /** Canonical lowered representation, useful to durable compatibility code. */
    json to_json() const;

private:
    struct Impl;
    explicit ProgramPlanNode(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;

    friend class ProgramPlan;
};

/**
 * Immutable operation graph.  `from_json` accepts the canonical lowered plan
 * stored in ProgramBundle and rejects malformed references, unknown tags, and
 * invalid operation-specific fields before a run is scheduled.
 */
class NEOGRAPH_PROGRAM_API ProgramPlan final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    ProgramPlan(const ProgramPlan&)            = default;
    ProgramPlan& operator=(const ProgramPlan&) = default;
    ProgramPlan(ProgramPlan&&) noexcept        = default;
    ProgramPlan& operator=(ProgramPlan&&) noexcept = default;
    ~ProgramPlan();

    static ProgramPlan from_json(const json& plan);

    std::uint32_t schema_version() const noexcept;
    const std::string& root_id() const noexcept;
    const std::vector<ProgramPlanNode>& nodes() const noexcept;
    const ProgramPlanNode& root() const;
    const ProgramPlanNode* find(std::string_view id) const noexcept;
    json to_json() const;

private:
    struct Impl;
    explicit ProgramPlan(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/**
 * Worst-case resource floors implied by one finite typed Program tree.
 *
 * These counts are structural: branches choose their largest path, parallel
 * and race sum live work, and bounded loop/retry/map bodies are multiplied.
 * They intentionally do not infer unbounded Core-node behavior.
 */
struct NEOGRAPH_PROGRAM_API ProgramStaticBudgetRequirements {
    std::uint64_t max_program_operations = 0;
    std::uint32_t max_concurrency        = 0;
    std::uint64_t max_core_steps         = 0;
    std::uint64_t max_core_invocations   = 0;
    std::uint32_t max_child_depth        = 0;
    std::uint64_t max_total_children     = 0;

    bool operator==(const ProgramStaticBudgetRequirements&) const = default;
};

/**
 * Derive finite worst-case floors for a sealed Program operation tree.
 *
 * Throws std::invalid_argument for a cyclic or malformed graph and
 * std::overflow_error when a bounded composition exceeds the supported
 * resource integer range.
 */
NEOGRAPH_PROGRAM_API ProgramStaticBudgetRequirements
derive_static_budget_requirements(const ProgramPlan& plan);

}  // namespace neograph::program
