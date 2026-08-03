/**
 * @file program/migration.h
 * @brief Explicit compatibility plans between admitted Program versions.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/version.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class MigrationCompatibility : std::uint8_t {
    Compatible,
    OwnerMismatch,
    CompilerMismatch,
    RegistryMismatch,
    MaterializationMismatch,
};

struct MigrationPlanData {
    std::string              source_version_id;
    std::string              target_version_id;
    std::string              owner_scope;
    MigrationCompatibility   compatibility = MigrationCompatibility::Compatible;
    std::vector<std::string> blockers;
};

/**
 * Immutable proof of whether an admitted run may move to another version.
 *
 * This plan never mutates a store or a live run.  A runtime can safely apply
 * only a plan whose compatibility is Compatible; all other outcomes carry
 * explicit blockers rather than silently falling back to a restart.
 */
class NEOGRAPH_PROGRAM_API MigrationPlan final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static MigrationPlan create(MigrationPlanData data);
    static MigrationPlan between(const ProgramVersion& source, const ProgramVersion& target);
    static MigrationPlan parse(std::string_view stored_bytes);

    const std::string& source_version_id() const noexcept;
    const std::string& target_version_id() const noexcept;
    const std::string& owner_scope() const noexcept;
    MigrationCompatibility compatibility() const noexcept;
    const std::vector<std::string>& blockers() const noexcept;
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

}  // namespace neograph::program
