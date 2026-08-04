/**
 * @file mcp/sqlite_harness_store.h
 * @brief SQLite-backed durable storage for Harness artifacts and runs.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/harness_program_store.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
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

/**
 * @brief One-shot failure points for durable Program transition tests.
 *
 * These hooks are intentionally test-only in name and semantics. They let a
 * test abort a transaction after each durable row family is written and then
 * verify that reconnecting observes the previous complete publication.
 */
enum class SqliteHarnessProgramFaultPoint : std::uint8_t {
    AfterBegin,
    AfterRunWrite,
    AfterJournalWrite,
    AfterEventWrite,
    AfterEffectWrite,
    BeforeCommit,
};

/** Local SQLite implementation of Harness snapshots and the causal journal. */
class SqliteHarnessProgramTransitionStore;
class NEOGRAPH_API SqliteHarnessRecordStore final : public HarnessRecordStore,
                                                    public HarnessJournal,
                                                    public HarnessRetentionStore,
                                                    public HarnessProgramAdapterStore,
                                                    public HarnessContractRunStore {
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
    std::shared_ptr<program::ProgramTransitionStore>
    bind_program_transitions(HarnessProgramArtifactRecord artifact) override;

    void save_contract_run(const HarnessProgramArtifactRecord& artifact,
                           const program::ProgramRunRecord&     run_record,
                           const program::ContractRun&           contract_run) override;
    std::optional<program::ContractRun>
    load_contract_run(const HarnessProgramArtifactRecord& artifact,
                      const program::ProgramRunRecord&     run_record) const override;

#ifdef NEOGRAPH_TESTING
    /// Fail one subsequent Program transition at the requested transaction point.
    void fail_next_program_transition_for_testing(SqliteHarnessProgramFaultPoint point);
    /// Crash the current process once at the requested transition point.
    /// This is test-only and is used to prove reconnect recovery after SIGKILL.
    void crash_next_program_transition_for_testing(SqliteHarnessProgramFaultPoint point);
#endif
    std::optional<HarnessProgramRunRecord>
    resolve_program_run(std::string_view owner_scope, std::string_view run_id) const override;

private:
    struct Impl;
    friend class SqliteHarnessProgramTransitionStore;
    std::shared_ptr<Impl> impl_;
};

}  // namespace neograph::mcp
