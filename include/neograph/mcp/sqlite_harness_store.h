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
#include <vector>

namespace neograph::mcp {

enum class HarnessJournalPayloadMode {
    REDACTED,
    METADATA_ONLY,
    FULL,
};

struct SqliteHarnessJournalConfig {
    HarnessJournalPayloadMode mode = HarnessJournalPayloadMode::REDACTED;
    std::vector<std::string> redacted_keys = {
        "api_key", "authorization", "content", "messages", "output",
        "password", "result", "secret", "token",
    };
};

/** Local SQLite implementation of Harness snapshots and the causal journal. */
class NEOGRAPH_API SqliteHarnessRecordStore final : public HarnessRecordStore,
                                                    public HarnessJournal,
                                                    public HarnessRetentionStore {
public:
    /// Open or create a store with a five-second SQLite busy timeout.
    explicit SqliteHarnessRecordStore(const std::string& db_path);
    /// Open or create a store with an explicit competing-writer wait budget.
    SqliteHarnessRecordStore(const std::string& db_path, std::chrono::milliseconds busy_timeout);
    /// Open or create a store with explicit journal payload handling.
    SqliteHarnessRecordStore(const std::string&         db_path,
                             std::chrono::milliseconds  busy_timeout,
                             SqliteHarnessJournalConfig journal_config);
    ~SqliteHarnessRecordStore() override;

    SqliteHarnessRecordStore(const SqliteHarnessRecordStore&)            = delete;
    SqliteHarnessRecordStore& operator=(const SqliteHarnessRecordStore&) = delete;

    void                save_artifact(const std::string& artifact_id, const json& record) override;
    std::optional<json> load_artifact(const std::string& artifact_id) override;
    void                save_run(const std::string& run_id, const json& record) override;
    std::optional<json> load_run(const std::string& run_id) override;
    void                append_event(const json& event) override;
    std::vector<json>   list_events(const std::string& run_id,
                                    std::size_t        after_sequence = 0,
                                    std::size_t        limit = 1000) override;
    HarnessRetentionResult cleanup_retained(const HarnessRetentionPolicy& policy) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::mcp
