/**
 * @file program/fork.h
 * @brief Exact-checkpoint Program fork compatibility values.
 */
#pragma once

#include <neograph/graph/checkpoint.h>
#include <neograph/program/bundle.h>
#include <neograph/program/journal.h>
#include <neograph/program/result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** Exact source coordinate. An empty/latest-checkpoint reference is never accepted. */
struct ExactProgramCheckpointReference {
    std::string source_run_id;
    std::string source_checkpoint_id;

    bool operator==(const ExactProgramCheckpointReference&) const = default;
};

enum class ForkCompatibilityField : std::uint8_t {
    OwnerScope,
    SourceRun,
    SourceCheckpoint,
    TargetVersion,
    CoreName,
    CoreGeneration,
    CheckpointSchema,
    Channel,
    Reducer,
    Continuation,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ForkCompatibilityField field) noexcept;
NEOGRAPH_PROGRAM_API ForkCompatibilityField
fork_compatibility_field_from_string(std::string_view value);

/** Machine-readable reason that one exact fork fact is incompatible. */
struct ForkCompatibilityWitness {
    ForkCompatibilityField field = ForkCompatibilityField::Continuation;
    std::string            subject;
    json                   source;
    json                   target;

    bool operator==(const ForkCompatibilityWitness&) const = default;
};

enum class ForkCompatibilityStatus : std::uint8_t { Compatible, Rejected };

NEOGRAPH_PROGRAM_API std::string_view to_string(ForkCompatibilityStatus status) noexcept;
NEOGRAPH_PROGRAM_API ForkCompatibilityStatus
fork_compatibility_status_from_string(std::string_view value);

/**
 * Fully resolved facts for the PR7 single-call_core fork check.
 *
 * Runtime constructs these facts only from owner-scoped stored Program values,
 * the exact published checkpoint reference, and the loaded Core checkpoint.
 * No source compilation or invocation replay is involved.
 */
struct ExactForkCompatibilityFacts {
    std::string owner_scope;
    std::string source_owner_scope;
    std::string target_owner_scope;

    ExactProgramCheckpointReference requested_source;
    std::string                     stored_source_run_id;
    std::string                     source_program_version_id;
    std::string                     target_program_version_id;
    std::string                     resolved_target_program_version_id;

    CoreCheckpointIdentity published_checkpoint;
    graph::Checkpoint       loaded_checkpoint;

    CorePlanIdentity      source_core_plan;
    CorePlanIdentity      target_core_plan;
    SealedCoreDefinition source_core_definition;
    SealedCoreDefinition target_core_definition;
    ProgramContinuation  source_continuation;
};

struct ForkCompatibilityReceiptData {
    std::string                           owner_scope;
    std::string                           source_run_id;
    std::string                           source_program_version_id;
    std::string                           source_checkpoint_id;
    std::string                           target_program_version_id;
    ForkCompatibilityStatus               status = ForkCompatibilityStatus::Rejected;
    std::vector<ForkCompatibilityWitness> witnesses;
};

/** Immutable identity of the value applied at the target's initial fork boundary. */
struct ForkInitialResumeBinding {
    std::optional<std::string> target_pending_id;
    std::string                resume_value_identity;

    bool operator==(const ForkInitialResumeBinding&) const = default;
};

/** Canonical immutable compatibility receipt stored with target-run lineage. */
class NEOGRAPH_PROGRAM_API ForkCompatibilityReceipt {

public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;

    explicit ForkCompatibilityReceipt(ForkCompatibilityReceiptData data);
    static ForkCompatibilityReceipt parse(std::string_view stored_bytes);

    const std::string&                           id() const noexcept;
    const std::string&                           owner_scope() const noexcept;
    const std::string&                           source_run_id() const noexcept;
    const std::string&                           source_program_version_id() const noexcept;
    const std::string&                           source_checkpoint_id() const noexcept;
    const std::string&                           target_program_version_id() const noexcept;
    ForkCompatibilityStatus                     status() const noexcept;
    const std::vector<ForkCompatibilityWitness>& witnesses() const noexcept;
    std::uint32_t                                  storage_schema_version() const noexcept;
    const std::optional<ForkInitialResumeBinding>& initial_resume_binding() const noexcept;
    bool                                         compatible() const noexcept;
    ForkCompatibilityReceipt                       with_initial_resume_binding(
                              std::optional<std::string> target_pending_id, const json& resume_value) const;
    bool matches_initial_resume(std::string_view target_pending_id, const json& resume_value) const;
    std::string                                  serialize_canonical() const;

private:
    struct Impl;
    explicit ForkCompatibilityReceipt(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Typed pre-start rejection carrying the complete immutable witness receipt. */
class NEOGRAPH_PROGRAM_API ProgramForkCompatibilityError final
    : public std::runtime_error {
public:
    explicit ProgramForkCompatibilityError(ForkCompatibilityReceipt receipt);

    const ForkCompatibilityReceipt& receipt() const noexcept;

private:
    ForkCompatibilityReceipt receipt_;
};

/** Pure, fail-closed compatibility check. Rejection never mutates either run. */
NEOGRAPH_PROGRAM_API ForkCompatibilityReceipt
check_exact_fork_compatibility(ExactForkCompatibilityFacts facts);

}  // namespace neograph::program
