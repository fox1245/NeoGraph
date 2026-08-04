/**
 * @file program/migration.h
 * @brief Explicit compatibility plans between admitted Program versions.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/version.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class MigrationCompatibility : std::uint8_t {
    /// The version is admitted for new invocations only; pinned runs drain.
    NewRunsOnly,
    /// The exact source checkpoint may be projected to the target version.
    ForkCompatible,
    /// Existing source runs drain, but no checkpoint fork is published.
    DrainOnly,
    /// A human/operator must reconcile durable state before migration.
    OperatorReconciliation,
    /// The transition is not safe under the v1 contract.
    Blocked,

    // Source-compatible names retained for callers of the early P4 API.  A
    // mismatch is fail-closed and therefore maps to Blocked.
    Compatible             = ForkCompatible,
    OwnerMismatch           = 5,
    CompilerMismatch        = 6,
    RegistryMismatch        = 7,
    MaterializationMismatch = 8,
};

/** Contract dimension covered by a migration proof or rejection. */
enum class MigrationDimension : std::uint8_t {
    Channels,
    Continuation,
    PendingInput,
    PendingEffect,
    Retry,
    Cancellation,
    Interrupt,
    Budget,
    Authority,
    Checkpoint,
    Journal,
    Effects,
    Caches,
    Output,
    /// Additional dimensions are appended to preserve persisted ordering.
    Compiler,
    Registry,
    Materialization,
    Contract,
    Recovery,

    // The original P5 dimensions remain stable at values 0..13.
    // New dimensions are appended to preserve persisted enum ordering.
};

/** Stable, machine-readable evidence for one migration dimension. */
struct MigrationDiagnostic {
    MigrationDimension dimension = MigrationDimension::Continuation;
    std::string        code;
    std::string        message;
    json               source = json::object();
    json               target = json::object();

    bool operator==(const MigrationDiagnostic&) const;
};

/** Narrow, explicit mapping retained for a compatible migration dimension. */
struct MigrationMapping {
    MigrationDimension dimension = MigrationDimension::Continuation;
    std::string        rule;
    json               source = json::object();
    json               target = json::object();

    bool operator==(const MigrationMapping&) const;
};

using MigrationCompatibilityDiagnostic = MigrationDiagnostic;
using MigrationCompatibilityClass = MigrationCompatibility;
using MigrationAspect = MigrationDimension;

struct MigrationPlanData {
    std::string              source_version_id;
    std::string              target_version_id;
    std::string              owner_scope;
    MigrationCompatibility   compatibility = MigrationCompatibility::Compatible;
    std::vector<std::string> blockers;
    std::vector<MigrationDiagnostic> diagnostics;
    std::vector<MigrationMapping>    mappings;
};
/**
 * Immutable proof of whether an admitted run may move to another version.
 *
 * This plan never mutates a store or a live run. A runtime can safely apply
 * only a plan whose compatibility is ForkCompatible; all other outcomes carry
 * explicit blockers rather than silently falling back to a restart.
 */
class NEOGRAPH_PROGRAM_API MigrationPlan final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;
    /**
     * Explicit legacy input marker accepted by parse(). Legacy records are
     * converted to a fail-closed modern plan; aliases and missing evidence are
     * never accepted in the v1 schema.
     */
    static constexpr std::uint32_t LEGACY_STORAGE_SCHEMA_VERSION = 0;
    static MigrationPlan create(MigrationPlanData data);
    /**
     * Builds a conservative version-only proof. Distinct bundle identities
     * remain incompatible because their execution-relevant fields are absent.
     */
    static MigrationPlan between(const ProgramVersion& source, const ProgramVersion& target);
    /**
     * Builds a field-level proof from the admitted bundles. It permits a
     * budget-declaration change only when the fork invocation is separately
     * checked against the source remainder and target admitted bounds.
     */
    static MigrationPlan between(const ProgramVersion& source,
                                 const ProgramBundle&  source_bundle,
                                 const ProgramVersion& target,
                                 const ProgramBundle&  target_bundle);
    static MigrationPlan parse(std::string_view stored_bytes);

    const std::string& source_version_id() const noexcept;
    const std::string& target_version_id() const noexcept;
    const std::string& owner_scope() const noexcept;
    MigrationCompatibility compatibility() const noexcept;
    const std::vector<std::string>& blockers() const noexcept;
    const std::vector<MigrationDiagnostic>& diagnostics() const noexcept;
    const std::vector<MigrationMapping>& mappings() const noexcept;
    bool is_compatible() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit MigrationPlan(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

NEOGRAPH_PROGRAM_API std::string_view to_string(MigrationCompatibility compatibility) noexcept;
NEOGRAPH_PROGRAM_API MigrationCompatibility migration_compatibility_from_string(std::string_view value);
NEOGRAPH_PROGRAM_API std::string_view to_string(MigrationDimension dimension) noexcept;
NEOGRAPH_PROGRAM_API MigrationDimension migration_dimension_from_string(std::string_view value);

}  // namespace neograph::program
