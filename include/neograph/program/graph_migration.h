/**
 * @file program/graph_migration.h
 * @brief Immutable source capsules for prepared GraphEngine migration.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/safe_point.h>
#include <neograph/program/bundle.h>
#include <neograph/program/result.h>
#include <neograph/program/version.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

class MigrationPlan;
struct ProgramEvent;
class ProgramRunGeneration;
class ProgramRunLineage;
class ProgramRunRecord;

/** Exact root Core thread identity used by ProgramRuntime. */
NEOGRAPH_PROGRAM_API std::string program_root_core_thread_id(
    std::string_view run_id,
    std::string_view core_generation_id);

/** Content identity of exact checkpoint bytes published under a source head. */
NEOGRAPH_PROGRAM_API std::string graph_migration_checkpoint_content_id(
    const graph::Checkpoint& checkpoint);

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

struct ProgramGraphMigrationReceiptData {
    GraphMigrationCapsule capsule;
    ProgramBundle         source_bundle;
    ProgramVersion        source_version;
    ProgramBundle         target_bundle;
    ProgramVersion        target_version;
    std::string           migration_plan_id;
    std::uint64_t         target_generation = 0;
    std::string           target_run_id;
    std::string           target_program_version_id;
    std::string           target_bundle_id;
    std::string           target_invocation_id;
    std::string           target_binding_fingerprint;
    std::string           target_initial_run_record_id;
    std::string           target_initial_journal_head;
    CoreCheckpointIdentity target_core_checkpoint;
    graph::Checkpoint      target_checkpoint;
};

/** Immutable evidence admitted with one same-lineage GraphEngine successor. */
class NEOGRAPH_PROGRAM_API ProgramGraphMigrationReceipt final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramGraphMigrationReceipt(ProgramGraphMigrationReceiptData data);
    static ProgramGraphMigrationReceipt parse(std::string_view stored_bytes);

    const std::string& id() const noexcept;
    const GraphMigrationCapsule& capsule() const noexcept;
    const ProgramBundle& source_bundle() const noexcept;
    const ProgramVersion& source_version() const noexcept;
    const ProgramBundle& target_bundle() const noexcept;
    const ProgramVersion& target_version() const noexcept;
    const std::string& migration_plan_id() const noexcept;
    std::uint64_t target_generation() const noexcept;
    const std::string& target_run_id() const noexcept;
    const std::string& target_program_version_id() const noexcept;
    const std::string& target_bundle_id() const noexcept;
    const std::string& target_invocation_id() const noexcept;
    const std::string& target_binding_fingerprint() const noexcept;
    const std::string& target_initial_run_record_id() const noexcept;
    const std::string& target_initial_journal_head() const noexcept;
    const CoreCheckpointIdentity& target_core_checkpoint() const noexcept;
    const graph::Checkpoint& target_checkpoint() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramGraphMigrationReceipt(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Shared fail-closed validator used by every atomic transition backend. */
NEOGRAPH_PROGRAM_API bool is_valid_program_graph_migration_transition(
    const ProgramRunGeneration& predecessor,
    const ProgramRunLineage& previous_lineage,
    const ProgramRunRecord& source,
    const MigrationPlan& migration_plan,
    const ProgramRunGeneration& successor,
    const ProgramRunLineage& next_lineage,
    const ProgramRunRecord& target) noexcept;

/** Exact initial event required for a graph-migration successor publication. */
NEOGRAPH_PROGRAM_API bool does_program_graph_migration_started_event_bind(
    const ProgramEvent& event,
    const ProgramRunRecord& target) noexcept;

}  // namespace neograph::program
