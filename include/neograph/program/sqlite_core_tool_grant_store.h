#pragma once

#include <neograph/program/core_tool_grant_store.h>

#include <memory>
#include <string>

namespace neograph::program {

/** SQLite-backed, owner-scoped, immutable Core Tool grant records. */
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
