/**
 * @file program/sqlite_store.h
 * @brief SQLite-backed ProgramStore implementation.
 */
#pragma once

#include <neograph/program/store.h>

#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

/**
 * Transactional SQLite implementation of ProgramStore.
 *
 * A single connection is guarded by an internal mutex.  Publish and
 * activation compare-and-swap each use a database transaction, so a process
 * crash cannot expose a half-written bundle/version or activation pointer.
 */
class NEOGRAPH_PROGRAM_API SQLiteProgramStore final : public ProgramStore {
public:
    explicit SQLiteProgramStore(std::string database_path);
    SQLiteProgramStore(SQLiteProgramStore&&) noexcept;
    SQLiteProgramStore& operator=(SQLiteProgramStore&&) noexcept;
    SQLiteProgramStore(const SQLiteProgramStore&) = delete;
    SQLiteProgramStore& operator=(const SQLiteProgramStore&) = delete;
    ~SQLiteProgramStore() override;

    void publish_admitted(const ProgramBundle& bundle, const ProgramVersion& version) override;
    std::optional<ProgramBundle> get_bundle(std::string_view id) const override;
    std::optional<ProgramVersion> get_version(std::string_view id) const override;
    std::optional<ProgramActivation>
    get_activation(std::string_view owner_scope) const override;
    ProgramActivationResult compare_activate(std::string_view owner_scope,
                                             std::uint64_t    expected_generation,
                                             std::string_view version_id,
                                             std::string_view policy_snapshot_hash) override;
    std::vector<ProgramVersion> list_versions(std::string_view owner_scope) const override;
    ProgramRetentionReport collect_garbage(
        std::string_view owner_scope,
        const std::vector<std::string>& pinned_version_ids) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
