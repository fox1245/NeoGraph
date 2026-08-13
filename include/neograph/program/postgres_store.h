/**
 * @file program/postgres_store.h
 * @brief PostgreSQL-backed durable ProgramStore.
 */
#pragma once

#include <neograph/program/store.h>

#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

/**
 * Transactional PostgreSQL implementation of ProgramStore.
 *
 * The store owns one libpq connection guarded by a mutex. Every mutating
 * operation uses an explicit transaction, so a process crash cannot expose a
 * partially published bundle/version or activation pointer.
 */
class NEOGRAPH_PROGRAM_API PostgreSQLProgramStore final : public ProgramStore {
public:
    explicit PostgreSQLProgramStore(std::string connection_string);
    PostgreSQLProgramStore(PostgreSQLProgramStore&&) noexcept;
    PostgreSQLProgramStore& operator=(PostgreSQLProgramStore&&) noexcept;
    PostgreSQLProgramStore(const PostgreSQLProgramStore&) = delete;
    PostgreSQLProgramStore& operator=(const PostgreSQLProgramStore&) = delete;
    ~PostgreSQLProgramStore() override;

    void publish_admitted(const ProgramBundle& bundle, const ProgramVersion& version) override;
    std::optional<ProgramBundle> get_bundle(std::string_view id) const override;
    std::optional<ProgramVersion> get_version(std::string_view id) const override;
    std::optional<ProgramBundle> get_bundle(std::string_view owner_scope,
                                            std::string_view id) const override;
    std::optional<ProgramVersion> get_version(std::string_view owner_scope,
                                              std::string_view id) const override;
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
