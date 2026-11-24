#pragma once

#include <neograph/program/core_tool_grant_store.h>
#include <neograph/store_opened_in_another_process.h>

#include <memory>
#include <string>

#if defined(NEOGRAPH_PROGRAM_TESTING)
struct sqlite3;
#endif

namespace neograph::program {

/**
 * SQLite-backed, owner-scoped, immutable Core Tool grant records.
 *
 * A child made by fork opens a store of its own: the copy it inherited refuses
 * every operation, synchronous or awaitable, with StoreOpenedInAnotherProcess,
 * and destroying the copy leaves the parent's connection alone.
 */
class NEOGRAPH_PROGRAM_API SQLiteProgramCoreToolGrantStore final
    : public SyncProgramCoreToolGrantStore {
public:
    explicit SQLiteProgramCoreToolGrantStore(std::string database_path);
    ~SQLiteProgramCoreToolGrantStore() override;

    SQLiteProgramCoreToolGrantStore(const SQLiteProgramCoreToolGrantStore&) = delete;
    SQLiteProgramCoreToolGrantStore& operator=(const SQLiteProgramCoreToolGrantStore&) = delete;

    ProgramCoreToolGrantAdmission admit_sync(
        const ProgramCoreToolGrantRecord& record) override;
    std::optional<ProgramCoreToolGrantRecord> load_sync(
        const ProgramCoreToolGrantContext& context) const override;
    bool revoke_sync(const ProgramCoreToolGrantContext& context) override;
#if defined(NEOGRAPH_PROGRAM_TESTING)
    /// The store's database connection, for tests that watch what the store does with it.
    sqlite3* connection_for_testing() const noexcept;
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
