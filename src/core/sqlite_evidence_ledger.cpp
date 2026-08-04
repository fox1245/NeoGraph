#include <neograph/research/sqlite_evidence_ledger.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace neograph::research {
namespace {

constexpr std::uint64_t kMaximumLeaseDurationMs = 31ULL * 24 * 60 * 60 * 1000;

[[noreturn]] void throw_sqlite_error(sqlite3* db, const char* operation) {
    throw std::runtime_error(std::string("SqliteEvidenceLedger ") + operation + ": "
                             + sqlite3_errmsg(db));
}

void require(bool condition, std::string_view message) {
    if (!condition) throw std::invalid_argument(std::string(message));
}

void require_nonempty(std::string_view value, std::string_view name) {
    require(!value.empty(), std::string(name) + " must not be empty");
    require(value.size() <= 4096, std::string(name) + " exceeds 4096 bytes");
}

std::uint64_t checked_deadline(std::uint64_t now, std::uint64_t duration) {
    if (duration > std::numeric_limits<std::uint64_t>::max() - now) {
        throw std::invalid_argument("research task lease deadline overflows");
    }
    return now + duration;
}

class Statement final {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw_sqlite_error(db, "prepare failed");
        }
    }
    ~Statement() { if (statement_) sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite_error(db_, "bind text failed");
        }
    }
    void bind_int(int index, int value) {
        if (sqlite3_bind_int(statement_, index, value) != SQLITE_OK) {
            throw_sqlite_error(db_, "bind integer failed");
        }
    }
    void bind_int64(int index, std::uint64_t value) {
        require(value <= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()),
                "integer exceeds SQLite range");
        if (sqlite3_bind_int64(statement_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
            throw_sqlite_error(db_, "bind integer failed");
        }
    }
    int step() const { return sqlite3_step(statement_); }
    std::string column_text(int index) const {
        const auto* text = sqlite3_column_text(statement_, index);
        if (!text) return {};
        return std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(statement_, index));
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction final {
public:
    explicit Transaction(sqlite3* db) : db_(db) {
        char* error = nullptr;
        if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("SqliteEvidenceLedger begin transaction: " + message);
        }
    }
    ~Transaction() {
        if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        char* error = nullptr;
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("SqliteEvidenceLedger commit transaction: " + message);
        }
        committed_ = true;
    }

private:
    sqlite3* db_ = nullptr;
    bool committed_ = false;
};

void execute(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error("SqliteEvidenceLedger schema setup: " + message);
    }
}

constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS neograph_research_source (
    source_id TEXT PRIMARY KEY,
    canonical_locator TEXT NOT NULL,
    version TEXT NOT NULL,
    content_hash TEXT NOT NULL,
    payload TEXT NOT NULL,
    UNIQUE(canonical_locator, version, content_hash)
);
CREATE TABLE IF NOT EXISTS neograph_research_source_alias (
    alias TEXT PRIMARY KEY,
    source_id TEXT NOT NULL REFERENCES neograph_research_source(source_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS neograph_research_task (
    task_id TEXT PRIMARY KEY,
    owner_scope TEXT NOT NULL,
    source_id TEXT NOT NULL REFERENCES neograph_research_source(source_id),
    scope TEXT NOT NULL,
    task_kind TEXT NOT NULL,
    intentional_replication INTEGER NOT NULL,
    state TEXT NOT NULL,
    generation INTEGER NOT NULL,
    lease_id TEXT NOT NULL DEFAULT '',
    leased_by TEXT NOT NULL DEFAULT '',
    lease_expires_at_ms INTEGER NOT NULL DEFAULT 0,
    published_artifact_id TEXT NOT NULL DEFAULT '',
    payload TEXT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS neograph_research_primary_task_identity
    ON neograph_research_task(owner_scope, source_id, scope)
    WHERE task_kind = 'primary-extraction' AND intentional_replication = 0;
CREATE INDEX IF NOT EXISTS neograph_research_task_expiry
    ON neograph_research_task(owner_scope, state, lease_expires_at_ms);
CREATE TABLE IF NOT EXISTS neograph_research_artifact (
    artifact_id TEXT PRIMARY KEY,
    task_id TEXT NOT NULL UNIQUE REFERENCES neograph_research_task(task_id),
    source_id TEXT NOT NULL REFERENCES neograph_research_source(source_id),
    claim_id TEXT NOT NULL,
    owner_scope TEXT NOT NULL,
    polarity TEXT NOT NULL,
    payload TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS neograph_research_artifact_claim
    ON neograph_research_artifact(owner_scope, claim_id, artifact_id);
CREATE INDEX IF NOT EXISTS neograph_research_artifact_source
    ON neograph_research_artifact(owner_scope, source_id, artifact_id);
)SQL";

json to_json(const SourceIdentity& source) {
    return json{{"schema_version", source.schema_version},
                {"source_id", source.source_id},
                {"canonical_locator", source.canonical_locator},
                {"version", source.version},
                {"content_hash", source.content_hash},
                {"aliases", source.aliases},
                {"metadata", source.metadata}};
}

SourceIdentity source_from_json(const json& value) {
    SourceIdentity source;
    source.schema_version = value.at("schema_version").get<std::uint32_t>();
    source.source_id = value.at("source_id").get<std::string>();
    source.canonical_locator = value.at("canonical_locator").get<std::string>();
    source.version = value.at("version").get<std::string>();
    source.content_hash = value.at("content_hash").get<std::string>();
    source.aliases = value.at("aliases").get<std::vector<std::string>>();
    source.metadata = value.at("metadata");
    return source;
}

json to_json(const ResearchTaskSpec& task) {
    return json{{"schema_version", task.schema_version},
                {"task_id", task.task_id},
                {"objective_id", task.objective_id},
                {"source_id", task.source_id},
                {"claim_id", task.claim_id},
                {"owner_scope", task.owner_scope},
                {"scope", task.scope},
                {"kind", std::string(to_string(task.kind))},
                {"replication_of_task_id", task.replication_of_task_id},
                {"intentional_replication", task.intentional_replication},
                {"lease_duration_ms", task.lease_duration_ms},
                {"requirements", task.requirements}};
}

ResearchTaskSpec task_spec_from_json(const json& value) {
    ResearchTaskSpec task;
    task.schema_version = value.at("schema_version").get<std::uint32_t>();
    task.task_id = value.at("task_id").get<std::string>();
    task.objective_id = value.at("objective_id").get<std::string>();
    task.source_id = value.at("source_id").get<std::string>();
    task.claim_id = value.at("claim_id").get<std::string>();
    task.owner_scope = value.at("owner_scope").get<std::string>();
    task.scope = value.at("scope").get<std::string>();
    task.kind = research_task_kind_from_string(value.at("kind").get<std::string>());
    task.replication_of_task_id = value.at("replication_of_task_id").get<std::string>();
    task.intentional_replication = value.at("intentional_replication").get<bool>();
    task.lease_duration_ms = value.at("lease_duration_ms").get<std::uint64_t>();
    task.requirements = value.at("requirements");
    return task;
}

json to_json(const ResearchTask& task) {
    return json{{"spec", to_json(task.spec)},
                {"state", std::string(to_string(task.state))},
                {"generation", task.generation},
                {"lease_id", task.lease_id},
                {"leased_by", task.leased_by},
                {"lease_expires_at_unix_ms", task.lease_expires_at_unix_ms},
                {"published_artifact_id", task.published_artifact_id}};
}

ResearchTask task_from_json(const json& value) {
    ResearchTask task;
    task.spec = task_spec_from_json(value.at("spec"));
    task.state = research_task_state_from_string(value.at("state").get<std::string>());
    task.generation = value.at("generation").get<std::uint64_t>();
    task.lease_id = value.at("lease_id").get<std::string>();
    task.leased_by = value.at("leased_by").get<std::string>();
    task.lease_expires_at_unix_ms = value.at("lease_expires_at_unix_ms").get<std::uint64_t>();
    task.published_artifact_id = value.at("published_artifact_id").get<std::string>();
    return task;
}

json to_json(const EvidenceArtifact& artifact) {
    return json{{"schema_version", artifact.schema_version},
                {"artifact_id", artifact.artifact_id},
                {"task_id", artifact.task_id},
                {"source_id", artifact.source_id},
                {"claim_id", artifact.claim_id},
                {"owner_scope", artifact.owner_scope},
                {"source_version", artifact.source_version},
                {"source_content_hash", artifact.source_content_hash},
                {"polarity", std::string(to_string(artifact.polarity))},
                {"evidence_locator", artifact.evidence_locator},
                {"observation", artifact.observation},
                {"searched_scope", artifact.searched_scope},
                {"conditions", artifact.conditions},
                {"applicability", artifact.applicability},
                {"uncertainty", artifact.uncertainty},
                {"limitations", artifact.limitations},
                {"contradictions", artifact.contradictions},
                {"recommended_next_task", artifact.recommended_next_task},
                {"confidence", artifact.confidence},
                {"worker_id", artifact.worker_id},
                {"model_id", artifact.model_id},
                {"tool_id", artifact.tool_id},
                {"program_version_id", artifact.program_version_id},
                {"program_run_id", artifact.program_run_id},
                {"a2a_message_id", artifact.a2a_message_id},
                {"provenance", artifact.provenance}};
}

EvidenceArtifact artifact_from_json(const json& value) {
    EvidenceArtifact artifact;
    artifact.schema_version = value.at("schema_version").get<std::uint32_t>();
    artifact.artifact_id = value.at("artifact_id").get<std::string>();
    artifact.task_id = value.at("task_id").get<std::string>();
    artifact.source_id = value.at("source_id").get<std::string>();
    artifact.claim_id = value.at("claim_id").get<std::string>();
    artifact.owner_scope = value.at("owner_scope").get<std::string>();
    artifact.source_version = value.at("source_version").get<std::string>();
    artifact.source_content_hash = value.at("source_content_hash").get<std::string>();
    artifact.polarity = evidence_polarity_from_string(value.at("polarity").get<std::string>());
    artifact.evidence_locator = value.at("evidence_locator").get<std::string>();
    artifact.observation = value.at("observation").get<std::string>();
    artifact.searched_scope = value.at("searched_scope").get<std::string>();
    artifact.conditions = value.at("conditions");
    artifact.applicability = value.at("applicability");
    artifact.uncertainty = value.at("uncertainty");
    artifact.limitations = value.at("limitations").get<std::vector<std::string>>();
    artifact.contradictions = value.at("contradictions").get<std::vector<std::string>>();
    artifact.recommended_next_task = value.at("recommended_next_task").get<std::string>();
    artifact.confidence = value.at("confidence").get<double>();
    artifact.worker_id = value.at("worker_id").get<std::string>();
    artifact.model_id = value.at("model_id").get<std::string>();
    artifact.tool_id = value.at("tool_id").get<std::string>();
    artifact.program_version_id = value.at("program_version_id").get<std::string>();
    artifact.program_run_id = value.at("program_run_id").get<std::string>();
    artifact.a2a_message_id = value.at("a2a_message_id").get<std::string>();
    artifact.provenance = value.at("provenance");
    return artifact;
}

void validate_source(const SourceIdentity& source) {
    require(source.schema_version == 1, "unsupported source identity schema version");
    require_nonempty(source.source_id, "source_id");
    require_nonempty(source.canonical_locator, "canonical_locator");
    require_nonempty(source.version, "source version");
    require_nonempty(source.content_hash, "source content_hash");
    require(source.metadata.is_object(), "source metadata must be an object");
    std::unordered_set<std::string> seen;
    for (const auto& alias : source.aliases) {
        require_nonempty(alias, "source alias");
        require(alias != source.source_id, "source alias must not equal source_id");
        require(seen.insert(alias).second, "source aliases must be unique");
    }
}

void validate_task(const ResearchTaskSpec& task) {
    require(task.schema_version == 1, "unsupported research task schema version");
    require_nonempty(task.task_id, "task_id");
    require_nonempty(task.objective_id, "objective_id");
    require_nonempty(task.source_id, "task source_id");
    require_nonempty(task.claim_id, "task claim_id");
    require_nonempty(task.owner_scope, "task owner_scope");
    require_nonempty(task.scope, "task scope");
    require(task.lease_duration_ms > 0 && task.lease_duration_ms <= kMaximumLeaseDurationMs,
            "research task lease_duration_ms must be 1..31 days");
    require(task.requirements.is_object(), "task requirements must be an object");
    if (task.kind == ResearchTaskKind::PrimaryExtraction) {
        require(!task.intentional_replication, "primary extraction cannot be an intentional replication");
        require(task.replication_of_task_id.empty(),
                "primary extraction cannot reference replicated work");
    } else if (task.kind == ResearchTaskKind::IndependentReview
               || task.kind == ResearchTaskKind::Reproduction
               || task.kind == ResearchTaskKind::Rebuttal) {
        require(task.intentional_replication,
                "review, reproduction, and rebuttal tasks must declare intentional replication");
        require_nonempty(task.replication_of_task_id, "replication_of_task_id");
    }
}

void validate_artifact(const EvidenceArtifact& artifact, const ResearchTask& task,
                       const SourceIdentity& source, const ResearchTaskLease& lease) {
    require(artifact.schema_version == 1, "unsupported evidence artifact schema version");
    require_nonempty(artifact.artifact_id, "artifact_id");
    require(artifact.task_id == task.spec.task_id, "artifact task_id does not match lease task");
    require(artifact.source_id == task.spec.source_id, "artifact source_id does not match task");
    require(artifact.claim_id == task.spec.claim_id, "artifact claim_id does not match task");
    require(artifact.owner_scope == task.spec.owner_scope,
            "artifact owner_scope does not match task");
    require(artifact.owner_scope == lease.owner_scope,
            "artifact owner_scope does not match lease");
    require(artifact.source_version == source.version,
            "artifact source_version does not match admitted source");
    require_nonempty(artifact.evidence_locator, "artifact evidence_locator");
    require_nonempty(artifact.observation, "artifact observation");
    require_nonempty(artifact.searched_scope, "artifact searched_scope");
    require_nonempty(artifact.worker_id, "artifact worker_id");
    require(artifact.worker_id == lease.worker_id, "artifact worker_id does not own lease");
    require_nonempty(artifact.model_id, "artifact model_id");
    require_nonempty(artifact.tool_id, "artifact tool_id");
    require_nonempty(artifact.program_version_id, "artifact program_version_id");
    require_nonempty(artifact.program_run_id, "artifact program_run_id");
    require(std::isfinite(artifact.confidence) && artifact.confidence >= 0.0
                && artifact.confidence <= 1.0,
            "artifact confidence must be finite and in [0, 1]");
    require(artifact.conditions.is_object(), "artifact conditions must be an object");
    require(artifact.applicability.is_object(), "artifact applicability must be an object");
    require(artifact.uncertainty.is_object(), "artifact uncertainty must be an object");
    require(artifact.provenance.is_object(), "artifact provenance must be an object");
}

std::optional<SourceIdentity> load_source(sqlite3* db, std::string_view source_id) {
    Statement statement(db, "SELECT payload FROM neograph_research_source WHERE source_id = ?");
    statement.bind_text(1, source_id);
    const auto result = statement.step();
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw_sqlite_error(db, "load source failed");
    return source_from_json(json::parse(statement.column_text(0)));
}

std::optional<ResearchTask> load_task(sqlite3* db, std::string_view task_id) {
    Statement statement(db, "SELECT payload FROM neograph_research_task WHERE task_id = ?");
    statement.bind_text(1, task_id);
    const auto result = statement.step();
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw_sqlite_error(db, "load task failed");
    return task_from_json(json::parse(statement.column_text(0)));
}

std::optional<EvidenceArtifact> load_task_artifact(sqlite3* db, std::string_view task_id) {
    Statement statement(db, "SELECT payload FROM neograph_research_artifact WHERE task_id = ?");
    statement.bind_text(1, task_id);
    const auto result = statement.step();
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw_sqlite_error(db, "load task artifact failed");
    return artifact_from_json(json::parse(statement.column_text(0)));
}

std::optional<EvidenceArtifact> load_artifact(sqlite3* db, std::string_view artifact_id) {
    Statement statement(db, "SELECT payload FROM neograph_research_artifact WHERE artifact_id = ?");
    statement.bind_text(1, artifact_id);
    const auto result = statement.step();
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw_sqlite_error(db, "load artifact failed");
    return artifact_from_json(json::parse(statement.column_text(0)));
}

void write_task(sqlite3* db, const ResearchTask& task) {
    Statement statement(db, R"SQL(
UPDATE neograph_research_task
SET state = ?, generation = ?, lease_id = ?, leased_by = ?, lease_expires_at_ms = ?,
    published_artifact_id = ?, payload = ?
WHERE task_id = ?
)SQL");
    statement.bind_text(1, to_string(task.state));
    statement.bind_int64(2, task.generation);
    statement.bind_text(3, task.lease_id);
    statement.bind_text(4, task.leased_by);
    statement.bind_int64(5, task.lease_expires_at_unix_ms);
    statement.bind_text(6, task.published_artifact_id);
    statement.bind_text(7, to_json(task).dump());
    statement.bind_text(8, task.spec.task_id);
    if (statement.step() != SQLITE_DONE) throw_sqlite_error(db, "write task failed");
    if (sqlite3_changes(db) != 1) throw std::logic_error("research task disappeared during update");
}

ResearchTaskLease as_lease(const ResearchTask& task) {
    return ResearchTaskLease{task.spec.task_id, task.lease_id, task.leased_by,
                             task.spec.owner_scope, task.generation,
                             task.lease_expires_at_unix_ms};
}

void clear_expired_lease(ResearchTask& task, std::uint64_t now_unix_ms) {
    if (task.state == ResearchTaskState::Leased && task.lease_expires_at_unix_ms <= now_unix_ms) {
        task.state = ResearchTaskState::Ready;
        task.lease_id.clear();
        task.leased_by.clear();
        task.lease_expires_at_unix_ms = 0;
    }
}

bool has_live_lease(sqlite3* db, std::string_view owner_scope, std::string_view source_id,
                    std::uint64_t now_unix_ms) {
    Statement statement(db, R"SQL(
SELECT 1 FROM neograph_research_task
WHERE owner_scope = ? AND source_id = ? AND state = 'leased' AND lease_expires_at_ms > ? LIMIT 1
)SQL");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, source_id);
    statement.bind_int64(3, now_unix_ms);
    const auto result = statement.step();
    if (result == SQLITE_ROW) return true;
    if (result != SQLITE_DONE) throw_sqlite_error(db, "query source lease failed");
    return false;
}

struct ArtifactCounts {
    std::size_t total = 0;
    std::size_t primary = 0;
    std::size_t independent = 0;
    std::size_t supports = 0;
    std::size_t contradicts = 0;
    std::size_t inconclusive = 0;
};

ArtifactCounts source_artifact_counts(sqlite3* db, std::string_view owner_scope,
                                      std::string_view source_id) {
    Statement statement(db, R"SQL(
SELECT artifact.polarity, task.task_kind
FROM neograph_research_artifact AS artifact
JOIN neograph_research_task AS task ON task.task_id = artifact.task_id
WHERE artifact.owner_scope = ? AND artifact.source_id = ?
)SQL");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, source_id);
    ArtifactCounts counts;
    for (;;) {
        const auto result = statement.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) throw_sqlite_error(db, "query source artifacts failed");
        ++counts.total;
        const auto polarity = evidence_polarity_from_string(statement.column_text(0));
        const auto kind = research_task_kind_from_string(statement.column_text(1));
        if (kind == ResearchTaskKind::PrimaryExtraction) ++counts.primary;
        else ++counts.independent;
        if (polarity == EvidencePolarity::Supports) ++counts.supports;
        if (polarity == EvidencePolarity::Contradicts) ++counts.contradicts;
        if (polarity == EvidencePolarity::Inconclusive || polarity == EvidencePolarity::NoSupport) {
            ++counts.inconclusive;
        }
    }
    return counts;
}

} // namespace

struct SqliteEvidenceLedger::Impl {
    explicit Impl(const std::string& db_path, std::chrono::milliseconds busy_timeout) {
        if (busy_timeout.count() < 0 || busy_timeout.count() > std::numeric_limits<int>::max()) {
            throw std::invalid_argument("SQLite evidence ledger busy timeout is out of range");
        }
        if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                               | SQLITE_OPEN_FULLMUTEX, nullptr)
            != SQLITE_OK) {
            const std::string message = db ? sqlite3_errmsg(db) : "unable to allocate SQLite handle";
            if (db) sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("SqliteEvidenceLedger open: " + message);
        }
        try {
            if (sqlite3_busy_timeout(db, static_cast<int>(busy_timeout.count())) != SQLITE_OK) {
                throw_sqlite_error(db, "configure busy timeout failed");
            }
            execute(db, "PRAGMA foreign_keys = ON");
            execute(db, kSchema);
        } catch (...) {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }

    ~Impl() {
        if (db) sqlite3_close(db);
    }

    mutable std::mutex mutex;
    sqlite3* db = nullptr;
};

SqliteEvidenceLedger::SqliteEvidenceLedger(const std::string& db_path)
    : SqliteEvidenceLedger(db_path, std::chrono::seconds(5)) {}

SqliteEvidenceLedger::SqliteEvidenceLedger(const std::string& db_path,
                                           std::chrono::milliseconds busy_timeout)
    : impl_(std::make_shared<Impl>(db_path, busy_timeout)) {}

SqliteEvidenceLedger::~SqliteEvidenceLedger() = default;

void SqliteEvidenceLedger::register_source(SourceIdentity source) {
    validate_source(source);
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);

    if (const auto existing = load_source(impl_->db, source.source_id)) {
        if (*existing == source) {
            transaction.commit();
            return;
        }
        throw std::invalid_argument("source_id is already bound to a different immutable source");
    }
    {
        Statement alias_collision(
            impl_->db, "SELECT source_id FROM neograph_research_source_alias WHERE alias = ?");
        alias_collision.bind_text(1, source.source_id);
        if (alias_collision.step() == SQLITE_ROW) {
            throw std::invalid_argument("source_id is already reserved as an alias");
        }
    }

    {
        Statement identity(impl_->db, R"SQL(
SELECT source_id FROM neograph_research_source
WHERE canonical_locator = ? AND version = ? AND content_hash = ?
)SQL");
        identity.bind_text(1, source.canonical_locator);
        identity.bind_text(2, source.version);
        identity.bind_text(3, source.content_hash);
        if (identity.step() == SQLITE_ROW) {
            throw std::invalid_argument("canonical source version is already registered under another id");
        }
    }
    for (const auto& alias : source.aliases) {
        {
            Statement source_collision(
                impl_->db, "SELECT source_id FROM neograph_research_source WHERE source_id = ?");
            source_collision.bind_text(1, alias);
            if (source_collision.step() == SQLITE_ROW) {
                throw std::invalid_argument("source alias is already a canonical source_id");
            }
        }
        Statement lookup(impl_->db,
                         "SELECT source_id FROM neograph_research_source_alias WHERE alias = ?");
        lookup.bind_text(1, alias);
        if (lookup.step() == SQLITE_ROW) {
            throw std::invalid_argument("source alias is already bound to another source");
        }
    }

    Statement insert_source(impl_->db, R"SQL(
INSERT INTO neograph_research_source
(source_id, canonical_locator, version, content_hash, payload) VALUES (?, ?, ?, ?, ?)
)SQL");
    insert_source.bind_text(1, source.source_id);
    insert_source.bind_text(2, source.canonical_locator);
    insert_source.bind_text(3, source.version);
    insert_source.bind_text(4, source.content_hash);
    insert_source.bind_text(5, to_json(source).dump());
    if (insert_source.step() != SQLITE_DONE) throw_sqlite_error(impl_->db, "insert source failed");

    for (const auto& alias : source.aliases) {
        Statement insert_alias(impl_->db,
                               "INSERT INTO neograph_research_source_alias(alias, source_id) VALUES (?, ?)");
        insert_alias.bind_text(1, alias);
        insert_alias.bind_text(2, source.source_id);
        if (insert_alias.step() != SQLITE_DONE) throw_sqlite_error(impl_->db, "insert alias failed");
    }
    transaction.commit();
}

std::optional<SourceIdentity> SqliteEvidenceLedger::source(std::string_view source_id) const {
    std::lock_guard lock(impl_->mutex);
    if (const auto direct = load_source(impl_->db, source_id)) return direct;
    Statement alias(impl_->db,
                    "SELECT source_id FROM neograph_research_source_alias WHERE alias = ?");
    alias.bind_text(1, source_id);
    const auto result = alias.step();
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "resolve source alias failed");
    return load_source(impl_->db, alias.column_text(0));
}

void SqliteEvidenceLedger::create_task(ResearchTaskSpec spec) {
    validate_task(spec);
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);

    if (!load_source(impl_->db, spec.source_id)) {
        throw std::invalid_argument("research task references an unknown source");
    }
    if (!spec.replication_of_task_id.empty()) {
        const auto replicated = load_task(impl_->db, spec.replication_of_task_id);
        if (!replicated || replicated->spec.owner_scope != spec.owner_scope) {
            throw std::invalid_argument(
                "research task references an unknown or foreign replicated task");
        }
    }
    if (const auto existing = load_task(impl_->db, spec.task_id)) {
        if (existing->spec == spec) {
            transaction.commit();
            return;
        }
        throw std::invalid_argument("task_id is already bound to a different task");
    }
    if (spec.kind == ResearchTaskKind::PrimaryExtraction) {
        Statement duplicate(impl_->db, R"SQL(
SELECT task_id FROM neograph_research_task
WHERE owner_scope = ? AND source_id = ? AND scope = ? AND task_kind = 'primary-extraction'
  AND intentional_replication = 0
)SQL");
        duplicate.bind_text(1, spec.owner_scope);
        duplicate.bind_text(2, spec.source_id);
        duplicate.bind_text(3, spec.scope);
        if (duplicate.step() == SQLITE_ROW) {
            throw std::invalid_argument("primary extraction already exists for this source and scope");
        }
    }

    ResearchTask task;
    task.spec = std::move(spec);
    Statement insert(impl_->db, R"SQL(
INSERT INTO neograph_research_task
(task_id, owner_scope, source_id, scope, task_kind, intentional_replication, state, generation,
 lease_id, leased_by, lease_expires_at_ms, published_artifact_id, payload)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL");
    insert.bind_text(1, task.spec.task_id);
    insert.bind_text(2, task.spec.owner_scope);
    insert.bind_text(3, task.spec.source_id);
    insert.bind_text(4, task.spec.scope);
    insert.bind_text(5, to_string(task.spec.kind));
    insert.bind_int(6, task.spec.intentional_replication ? 1 : 0);
    insert.bind_text(7, to_string(task.state));
    insert.bind_int64(8, task.generation);
    insert.bind_text(9, task.lease_id);
    insert.bind_text(10, task.leased_by);
    insert.bind_int64(11, task.lease_expires_at_unix_ms);
    insert.bind_text(12, task.published_artifact_id);
    insert.bind_text(13, to_json(task).dump());
    if (insert.step() != SQLITE_DONE) throw_sqlite_error(impl_->db, "insert task failed");
    transaction.commit();
}

std::optional<ResearchTask> SqliteEvidenceLedger::task(std::string_view owner_scope,
                                                        std::string_view task_id) const {
    require_nonempty(owner_scope, "task owner_scope");
    require_nonempty(task_id, "task_id");
    std::lock_guard lock(impl_->mutex);
    const auto found = load_task(impl_->db, task_id);
    if (!found || found->spec.owner_scope != owner_scope) return std::nullopt;
    return found;
}

std::optional<ResearchTaskLease>
SqliteEvidenceLedger::acquire_lease(ResearchLeaseRequest request) {
    require_nonempty(request.task_id, "lease task_id");
    require_nonempty(request.lease_id, "lease_id");
    require_nonempty(request.worker_id, "lease worker_id");
    require_nonempty(request.owner_scope, "lease owner_scope");
    require(request.now_unix_ms > 0, "lease now_unix_ms must be explicit and positive");
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    auto task = load_task(impl_->db, request.task_id);
    if (!task) {
        transaction.commit();
        return std::nullopt;
    }
    if (task->spec.owner_scope != request.owner_scope) {
        transaction.commit();
        return std::nullopt;
    }

    if (task->state == ResearchTaskState::Leased) {
        if (task->lease_expires_at_unix_ms > request.now_unix_ms
            && task->lease_id == request.lease_id && task->leased_by == request.worker_id) {
            const auto lease = as_lease(*task);
            transaction.commit();
            return lease;
        }
        clear_expired_lease(*task, request.now_unix_ms);
        if (task->state == ResearchTaskState::Leased) {
            transaction.commit();
            return std::nullopt;
        }
    }
    if (task->state != ResearchTaskState::Ready) {
        transaction.commit();
        return std::nullopt;
    }

    task->state = ResearchTaskState::Leased;
    ++task->generation;
    task->lease_id = std::move(request.lease_id);
    task->leased_by = std::move(request.worker_id);
    task->lease_expires_at_unix_ms = checked_deadline(request.now_unix_ms, task->spec.lease_duration_ms);
    write_task(impl_->db, *task);
    const auto lease = as_lease(*task);
    transaction.commit();
    return lease;
}

bool SqliteEvidenceLedger::renew_lease(const ResearchTaskLease& lease,
                                       std::uint64_t now_unix_ms) {
    require_nonempty(lease.task_id, "lease task_id");
    require_nonempty(lease.lease_id, "lease_id");
    require_nonempty(lease.worker_id, "lease worker_id");
    require_nonempty(lease.owner_scope, "lease owner_scope");
    require(now_unix_ms > 0, "lease now_unix_ms must be explicit and positive");
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    auto task = load_task(impl_->db, lease.task_id);
    if (!task || task->spec.owner_scope != lease.owner_scope
        || task->state != ResearchTaskState::Leased || task->lease_id != lease.lease_id
        || task->leased_by != lease.worker_id || task->generation != lease.generation
        || task->lease_expires_at_unix_ms <= now_unix_ms) {
        transaction.commit();
        return false;
    }
    task->lease_expires_at_unix_ms = checked_deadline(now_unix_ms, task->spec.lease_duration_ms);
    write_task(impl_->db, *task);
    transaction.commit();
    return true;
}

std::vector<std::string> SqliteEvidenceLedger::expire_leases(std::string_view owner_scope,
                                                              std::uint64_t now_unix_ms) {
    require_nonempty(owner_scope, "lease owner_scope");
    require(now_unix_ms > 0, "lease now_unix_ms must be explicit and positive");
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    Statement select(impl_->db, R"SQL(
SELECT payload FROM neograph_research_task
WHERE owner_scope = ? AND state = 'leased' AND lease_expires_at_ms <= ? ORDER BY task_id
)SQL");
    select.bind_text(1, owner_scope);
    select.bind_int64(2, now_unix_ms);
    std::vector<ResearchTask> expired;
    for (;;) {
        const auto result = select.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "find expired leases failed");
        expired.push_back(task_from_json(json::parse(select.column_text(0))));
    }
    std::vector<std::string> ids;
    ids.reserve(expired.size());
    for (auto& task : expired) {
        clear_expired_lease(task, now_unix_ms);
        ids.push_back(task.spec.task_id);
        write_task(impl_->db, task);
    }
    transaction.commit();
    return ids;
}

EvidencePublishResult SqliteEvidenceLedger::publish(const ResearchTaskLease& lease,
                                                    EvidenceArtifact artifact,
                                                    std::uint64_t now_unix_ms) {
    require_nonempty(lease.task_id, "lease task_id");
    require_nonempty(lease.lease_id, "lease_id");
    require_nonempty(lease.worker_id, "lease worker_id");
    require_nonempty(lease.owner_scope, "lease owner_scope");
    require(now_unix_ms > 0, "publish now_unix_ms must be explicit and positive");
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    auto task = load_task(impl_->db, lease.task_id);
    if (!task) throw std::invalid_argument("cannot publish unknown research task");
    const auto source = load_source(impl_->db, task->spec.source_id);
    if (!source) throw std::logic_error("research task source disappeared");
    validate_artifact(artifact, *task, *source, lease);

    if (task->state == ResearchTaskState::Published) {
        const auto existing = load_task_artifact(impl_->db, task->spec.task_id);
        if (existing && *existing == artifact) {
            transaction.commit();
            return EvidencePublishResult::Duplicate;
        }
        throw std::logic_error("a published research task cannot be overwritten");
    }
    if (task->spec.owner_scope != lease.owner_scope
        || task->state != ResearchTaskState::Leased || task->lease_id != lease.lease_id
        || task->leased_by != lease.worker_id || task->generation != lease.generation
        || task->lease_expires_at_unix_ms <= now_unix_ms) {
        throw std::logic_error("evidence publication does not own a live task lease");
    }
    if (const auto existing = load_artifact(impl_->db, artifact.artifact_id)) {
        if (*existing == artifact) {
            throw std::logic_error("artifact exists without its task publication");
        }
        throw std::invalid_argument("artifact_id is already bound to different evidence");
    }

    Statement insert(impl_->db, R"SQL(
INSERT INTO neograph_research_artifact
(artifact_id, task_id, source_id, claim_id, owner_scope, polarity, payload)
VALUES (?, ?, ?, ?, ?, ?, ?)
)SQL");
    insert.bind_text(1, artifact.artifact_id);
    insert.bind_text(2, artifact.task_id);
    insert.bind_text(3, artifact.source_id);
    insert.bind_text(4, artifact.claim_id);
    insert.bind_text(5, artifact.owner_scope);
    insert.bind_text(6, to_string(artifact.polarity));
    insert.bind_text(7, to_json(artifact).dump());
    if (insert.step() != SQLITE_DONE) throw_sqlite_error(impl_->db, "insert evidence artifact failed");

    task->state = ResearchTaskState::Published;
    task->lease_id.clear();
    task->leased_by.clear();
    task->lease_expires_at_unix_ms = 0;
    task->published_artifact_id = artifact.artifact_id;
    write_task(impl_->db, *task);
    transaction.commit();
    return EvidencePublishResult::Published;
}

std::vector<EvidenceArtifact>
SqliteEvidenceLedger::artifacts_for_claim(std::string_view owner_scope,
                                          std::string_view claim_id) const {
    require_nonempty(owner_scope, "artifact owner_scope");
    require_nonempty(claim_id, "claim_id");
    std::lock_guard lock(impl_->mutex);
    Statement statement(impl_->db, R"SQL(
SELECT payload FROM neograph_research_artifact
WHERE owner_scope = ? AND claim_id = ? ORDER BY artifact_id
)SQL");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, claim_id);
    std::vector<EvidenceArtifact> artifacts;
    for (;;) {
        const auto result = statement.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "list claim artifacts failed");
        artifacts.push_back(artifact_from_json(json::parse(statement.column_text(0))));
    }
    return artifacts;
}

SourceLifecycle SqliteEvidenceLedger::source_lifecycle(std::string_view owner_scope,
                                                        std::string_view source_id,
                                                        std::uint64_t now_unix_ms) const {
    require_nonempty(owner_scope, "lifecycle owner_scope");
    require_nonempty(source_id, "source_id");
    require(now_unix_ms > 0, "lifecycle now_unix_ms must be explicit and positive");
    std::lock_guard lock(impl_->mutex);
    if (!load_source(impl_->db, source_id)) {
        throw std::invalid_argument("unknown source identity");
    }
    const auto counts = source_artifact_counts(impl_->db, owner_scope, source_id);
    if (counts.total == 0) {
        return has_live_lease(impl_->db, owner_scope, source_id, now_unix_ms)
                   ? SourceLifecycle::Claimed
                   : SourceLifecycle::Unclaimed;
    }
    if (counts.supports > 0 && counts.contradicts > 0) return SourceLifecycle::Contradicted;
    if (counts.independent > 0 && counts.supports >= 2) return SourceLifecycle::Corroborated;
    if (counts.independent > 0) {
        if (counts.contradicts > 0) return SourceLifecycle::Contradicted;
        if (counts.inconclusive > 0) return SourceLifecycle::Inconclusive;
        return SourceLifecycle::Reviewed;
    }
    return SourceLifecycle::Extracted;
}

ClaimResolution SqliteEvidenceLedger::resolve_claim(std::string_view owner_scope,
                                                    std::string_view claim_id) const {
    require_nonempty(owner_scope, "claim owner_scope");
    require_nonempty(claim_id, "claim_id");
    ClaimResolution resolution;
    resolution.claim_id = std::string(claim_id);
    const auto artifacts = artifacts_for_claim(owner_scope, claim_id);
    for (const auto& artifact : artifacts) {
        switch (artifact.polarity) {
            case EvidencePolarity::Supports:
                resolution.supporting_artifact_ids.push_back(artifact.artifact_id);
                break;
            case EvidencePolarity::Contradicts:
                resolution.contradicting_artifact_ids.push_back(artifact.artifact_id);
                break;
            case EvidencePolarity::Inconclusive:
            case EvidencePolarity::NoSupport:
                resolution.inconclusive_artifact_ids.push_back(artifact.artifact_id);
                break;
        }
    }
    if (!resolution.supporting_artifact_ids.empty() && !resolution.contradicting_artifact_ids.empty()) {
        resolution.kind = ClaimResolutionKind::ReconciliationRequired;
        resolution.recommended_next_task = ResearchTaskKind::Reconciliation;
    } else if (resolution.supporting_artifact_ids.size() >= 2) {
        resolution.kind = ClaimResolutionKind::Corroborated;
    } else if (resolution.contradicting_artifact_ids.size() >= 2) {
        resolution.kind = ClaimResolutionKind::Contradicted;
    } else if (!resolution.supporting_artifact_ids.empty()) {
        resolution.kind = ClaimResolutionKind::Unresolved;
        resolution.recommended_next_task = ResearchTaskKind::IndependentReview;
    } else if (!resolution.contradicting_artifact_ids.empty()) {
        resolution.kind = ClaimResolutionKind::Unresolved;
        resolution.recommended_next_task = ResearchTaskKind::Rebuttal;
    } else if (!resolution.inconclusive_artifact_ids.empty()) {
        resolution.kind = ClaimResolutionKind::Inconclusive;
        resolution.recommended_next_task = ResearchTaskKind::IndependentReview;
    }
    return resolution;
}

} // namespace neograph::research
