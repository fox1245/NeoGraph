/**
 * @file mcp/sqlite_harness_store.h
 * @brief SQLite-backed durable storage for Harness artifacts and runs.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/mcp/harness.h>

#include <chrono>
#include <memory>
#include <string>

namespace neograph::mcp {

/** Local SQLite implementation of the Harness record-store boundary. */
class NEOGRAPH_API SqliteHarnessRecordStore final : public HarnessRecordStore {
public:
    /// Open or create a store with a five-second SQLite busy timeout.
    explicit SqliteHarnessRecordStore(const std::string& db_path);
    /// Open or create a store with an explicit competing-writer wait budget.
    SqliteHarnessRecordStore(const std::string& db_path, std::chrono::milliseconds busy_timeout);
    ~SqliteHarnessRecordStore() override;

    SqliteHarnessRecordStore(const SqliteHarnessRecordStore&)            = delete;
    SqliteHarnessRecordStore& operator=(const SqliteHarnessRecordStore&) = delete;

    void                save_artifact(const std::string& artifact_id, const json& record) override;
    std::optional<json> load_artifact(const std::string& artifact_id) override;
    void                save_run(const std::string& run_id, const json& record) override;
    std::optional<json> load_run(const std::string& run_id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::mcp
