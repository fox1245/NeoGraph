/**
 * @file program/graph_migration.h
 * @brief Immutable source capsules for prepared GraphEngine migration.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/safe_point.h>
#include <neograph/program/lineage.h>
#include <neograph/program/result.h>
#include <neograph/program/version.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

/** Exact root Core thread identity used by ProgramRuntime. */
NEOGRAPH_PROGRAM_API std::string program_root_core_thread_id(
    std::string_view run_id,
    std::string_view core_generation_id);

/**
 * Exact, content-addressed source evidence captured at a durable Core boundary.
 *
 * This value grants no authority and names no target. A migration plan and an
 * atomic lineage transition must independently validate and consume it.
 */
class NEOGRAPH_PROGRAM_API GraphMigrationCapsule final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static GraphMigrationCapsule seal(
        const ProgramRunGeneration& generation,
        const ProgramRunLineage& lineage,
        const ProgramVersion& version,
        const graph::GraphSafePoint& safe_point);
    static GraphMigrationCapsule parse(std::string_view stored_bytes);

    const std::string& id() const noexcept;
    const std::string& owner_scope() const noexcept;
    const std::string& lineage_id() const noexcept;
    std::uint64_t source_generation() const noexcept;
    const std::string& source_generation_id() const noexcept;
    const std::string& source_lineage_head_id() const noexcept;
    const std::string& source_run_id() const noexcept;
    const std::string& source_program_version_id() const noexcept;
    const std::string& source_bundle_id() const noexcept;
    const CoreCheckpointIdentity& core_checkpoint() const noexcept;
    const graph::Checkpoint& checkpoint() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit GraphMigrationCapsule(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
