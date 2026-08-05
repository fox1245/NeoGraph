/**
 * @file research/sqlite_evidence_ledger.h
 * @brief SQLite implementation of the durable research evidence ledger.
 */
#pragma once

#include <neograph/research/evidence_ledger.h>

#include <chrono>
#include <memory>
#include <string>

namespace neograph::research {

/**
 * SQLite-backed ledger with `BEGIN IMMEDIATE` lease/publication transitions.
 * Multiple processes may share one database; SQLite serializes writers, while
 * the task generation and lease id prevent stale workers from publishing.
 */
class NEOGRAPH_API SqliteEvidenceLedger final : public EvidenceLedger {
public:
    explicit SqliteEvidenceLedger(const std::string& db_path);
    SqliteEvidenceLedger(const std::string& db_path,
                         std::chrono::milliseconds busy_timeout);
    ~SqliteEvidenceLedger() override;

    SqliteEvidenceLedger(const SqliteEvidenceLedger&) = delete;
    SqliteEvidenceLedger& operator=(const SqliteEvidenceLedger&) = delete;

    void register_source(SourceIdentity source) override;
    [[nodiscard]] std::optional<SourceIdentity> source(std::string_view source_id) const override;

    void create_task(ResearchTaskSpec task) override;
    [[nodiscard]] std::optional<ResearchTask>
    task(std::string_view owner_scope, std::string_view task_id) const override;

    [[nodiscard]] std::vector<ResearchTask>
    tasks(std::string_view owner_scope, bool include_terminal) const override;

    [[nodiscard]] std::optional<ResearchTaskLease>
    acquire_lease(ResearchLeaseRequest request) override;
    [[nodiscard]] bool renew_lease(const ResearchTaskLease& lease,
                                   std::uint64_t now_unix_ms) override;
    [[nodiscard]] std::vector<std::string>
    expire_leases(std::string_view owner_scope, std::uint64_t now_unix_ms) override;
    [[nodiscard]] EvidencePublishResult
    publish(const ResearchTaskLease& lease, EvidenceArtifact artifact,
            std::uint64_t now_unix_ms) override;

    [[nodiscard]] std::vector<EvidenceArtifact>
    artifacts_for_claim(std::string_view owner_scope, std::string_view claim_id) const override;
    [[nodiscard]] SourceLifecycle source_lifecycle(std::string_view owner_scope,
                                                    std::string_view source_id,
                                                    std::uint64_t now_unix_ms) const override;
    [[nodiscard]] ClaimResolution resolve_claim(std::string_view owner_scope,
                                                std::string_view claim_id) const override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace neograph::research
