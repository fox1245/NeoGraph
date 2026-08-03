#include <neograph/program/sqlite_transition_store.h>

#include <sqlite3.h>

#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace neograph::program {
namespace {

[[noreturn]] void throw_sqlite(sqlite3* db, std::string_view operation) {
    throw std::runtime_error(std::string(operation) + ": " +
                             (db ? sqlite3_errmsg(db) : "SQLite unavailable"));
}

void exec(sqlite3* db, std::string_view sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql.data(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error("SQLite " + std::string(sql.substr(0, 48)) + ": " + message);
    }
}

class Statement final {
public:
    Statement(sqlite3* db, std::string_view sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr) !=
            SQLITE_OK)
            throw_sqlite(db_, "SQLite prepare");
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() { sqlite3_finalize(statement_); }

    sqlite3_stmt* get() const noexcept { return statement_; }

    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
            throw_sqlite(db_, "SQLite bind text");
    }

    void bind_blob(int index, std::string_view value) {
        if (sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
            throw_sqlite(db_, "SQLite bind blob");
    }

    bool step_row() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw_sqlite(db_, "SQLite step");
    }

    void step_done() {
        if (sqlite3_step(statement_) != SQLITE_DONE) throw_sqlite(db_, "SQLite step");
    }

private:
    sqlite3*       db_        = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction final {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE"); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if (!committed_) {
            char* error = nullptr;
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &error);
            sqlite3_free(error);
        }
    }

    void commit() {
        exec(db_, "COMMIT");
        committed_ = true;
    }

private:
    sqlite3* db_        = nullptr;
    bool     committed_ = false;
};

std::string column_blob(sqlite3_stmt* statement, int index) {
    const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement, index));
    const int size = sqlite3_column_bytes(statement, index);
    if (!bytes || size < 0) throw std::runtime_error("SQLite returned a null transition publication");
    return std::string(bytes, static_cast<std::size_t>(size));
}

struct StoredPublication {
    ProgramTransitionPublication publication;
    std::string                   last_publication_bytes;
};

std::optional<StoredPublication> load_publication(sqlite3* db, std::string_view owner_scope,
                                                   std::string_view run_id) {
    Statement statement(
        db, "SELECT canonical_bytes, last_publication_bytes FROM program_transition_runs "
            "WHERE owner_scope = ?1 AND run_id = ?2");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    if (!statement.step_row()) return std::nullopt;

    return StoredPublication{
        ProgramTransitionPublication::parse(column_blob(statement.get(), 0)),
        column_blob(statement.get(), 1)};
}

bool is_final(ContinuationState state) noexcept {
    return state != ContinuationState::Running && state != ContinuationState::Interrupted &&
           state != ContinuationState::AmbiguousEffect;
}

bool same_fork(const ProgramRunRecord& old_run, const ProgramRunRecord& new_run) {
    const auto old_receipt = old_run.fork_receipt();
    const auto new_receipt = new_run.fork_receipt();
    return old_receipt.has_value() == new_receipt.has_value() &&
           (!old_receipt || old_receipt->id() == new_receipt->id());
}

bool valid_initial_publication(const ProgramTransitionPublication& publication) {
    const auto& journal = publication.journal_record;
    const auto  terminal = publication.run_record.terminal_result();
    const bool  terminal_event = !publication.events.empty() &&
                                 publication.events.back().kind == ProgramEventKind::Terminal;
    return journal.previous_id.empty() && journal.sequence == 1 && !publication.events.empty() &&
           publication.events.front().kind == ProgramEventKind::Started &&
           publication.run_record.event_sequence() == publication.events.size() &&
           publication.run_record.effect_sequence() == publication.effects.size() &&
           publication.events.front().sequence == 1 &&
           (publication.effects.empty() || publication.effects.front().sequence() == 1) &&
           (!terminal || terminal_event);
}

bool valid_increment(const ProgramTransitionPublication& old_publication,
                     const ProgramTransitionPublication& next_publication,
                     std::string_view expected_journal_head) {
    const auto& old_run  = old_publication.run_record;
    const auto& next_run = next_publication.run_record;
    const auto& old_journal = old_publication.journal_record;
    const auto& next_journal = next_publication.journal_record;
    const auto  old_terminal = old_run.terminal_result();
    const auto  new_terminal = next_run.terminal_result();
    const bool  terminal_event = !next_publication.events.empty() &&
                                 next_publication.events.back().kind == ProgramEventKind::Terminal;

    if ((old_terminal && is_final(old_run.continuation().state) && !new_terminal) ||
        (new_terminal && (!old_terminal || old_terminal->id() != new_terminal->id()) &&
         !terminal_event) ||
        (next_publication.events.empty() && next_publication.effects.empty())) {
        return false;
    }
    if (old_journal.id != expected_journal_head || next_journal.previous_id != expected_journal_head ||
        !is_valid_program_journal_transition(old_journal, next_journal) ||
        next_run.created_at_ms() != old_run.created_at_ms() ||
        next_run.updated_at_ms() < old_run.updated_at_ms() ||
        next_run.binding_fingerprint() != old_run.binding_fingerprint() ||
        !same_fork(old_run, next_run) ||
        next_run.recorded_binding_set_fingerprint() != old_run.recorded_binding_set_fingerprint() ||
        next_run.invocation() != old_run.invocation() ||
        next_run.event_sequence() != old_run.event_sequence() + next_publication.events.size() ||
        next_run.effect_sequence() != old_run.effect_sequence() + next_publication.effects.size()) {
        return false;
    }
    if (!next_publication.events.empty() &&
        next_publication.events.front().sequence != old_run.event_sequence() + 1)
        return false;
    if (!next_publication.effects.empty() &&
        next_publication.effects.front().sequence() != old_run.effect_sequence() + 1)
        return false;
    return true;
}

ProgramTransitionPublication merged_publication(const StoredPublication& old_publication,
                                                ProgramTransitionPublication next) {
    auto events = old_publication.publication.events;
    events.insert(events.end(), next.events.begin(), next.events.end());
    auto effects = old_publication.publication.effects;
    effects.insert(effects.end(), next.effects.begin(), next.effects.end());
    return {std::move(next.run_record), std::move(next.journal_record), std::move(events),
            std::move(effects)};
}

}  // namespace

struct SQLiteProgramTransitionStore::Impl {
    explicit Impl(std::string value) : path(std::move(value)) {}
    ~Impl() {
        if (db) sqlite3_close_v2(db);
    }

    std::string       path;
    sqlite3*          db = nullptr;
    mutable std::mutex mutex;
};

SQLiteProgramTransitionStore::SQLiteProgramTransitionStore(std::string database_path)
    : impl_(std::make_unique<Impl>(std::move(database_path))) {
    if (impl_->path.empty())
        throw std::invalid_argument("SQLite ProgramTransitionStore path must not be empty");

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(impl_->path.c_str(), &impl_->db, flags, nullptr) != SQLITE_OK)
        throw_sqlite(impl_->db, "SQLite open");

    try {
        exec(impl_->db, "PRAGMA foreign_keys = ON");
        exec(impl_->db,
             "CREATE TABLE IF NOT EXISTS program_transition_runs ("
             "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, "
             "canonical_bytes BLOB NOT NULL, last_publication_bytes BLOB NOT NULL, "
             "PRIMARY KEY(owner_scope, run_id))");
    } catch (...) {
        sqlite3_close_v2(impl_->db);
        impl_->db = nullptr;
        throw;
    }
}

SQLiteProgramTransitionStore::SQLiteProgramTransitionStore(SQLiteProgramTransitionStore&&) noexcept =
    default;
SQLiteProgramTransitionStore&
SQLiteProgramTransitionStore::operator=(SQLiteProgramTransitionStore&&) noexcept = default;
SQLiteProgramTransitionStore::~SQLiteProgramTransitionStore() = default;

std::optional<ProgramRunRecord> SQLiteProgramTransitionStore::load(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto publication = load_publication(impl_->db, owner_scope, run_id);
    if (!publication) return std::nullopt;
    return publication->publication.run_record;
}

std::optional<ProgramJournalRecord> SQLiteProgramTransitionStore::latest(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto publication = load_publication(impl_->db, owner_scope, run_id);
    if (!publication) return std::nullopt;
    return publication->publication.journal_record;
}

std::vector<ProgramEvent> SQLiteProgramTransitionStore::load_events(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    const auto publication = load_publication(impl_->db, owner_scope, run_id);
    if (!publication) return {};

    std::vector<ProgramEvent> result;
    for (const auto& event : publication->publication.events)
        if (event.sequence > after_sequence) result.push_back(event);
    return result;
}

std::vector<ProgramEffectOutboxEntry> SQLiteProgramTransitionStore::load_effects(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    const auto publication = load_publication(impl_->db, owner_scope, run_id);
    if (!publication) return {};

    std::vector<ProgramEffectOutboxEntry> result;
    for (const auto& effect : publication->publication.effects)
        if (effect.sequence() > after_sequence) result.push_back(effect);
    return result;
}

ProgramTransitionPublishResult SQLiteProgramTransitionStore::compare_publish(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication) {
    std::string publication_bytes;
    try {
        if (publication.run_record.owner_scope() != owner_scope)
            throw std::invalid_argument("Program transition owner scope does not match the publication");
        publication_bytes = publication.serialize_canonical();
    } catch (const std::invalid_argument&) {
        return ProgramTransitionPublishResult::Conflict;
    }

    const std::string run_id = publication.run_record.run_id();
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    const auto current = load_publication(impl_->db, owner_scope, run_id);

    if (current && current->last_publication_bytes == publication_bytes) {
        const auto result = expected_journal_head == publication.journal_record.previous_id
                                ? ProgramTransitionPublishResult::AlreadyPresent
                                : ProgramTransitionPublishResult::Conflict;
        transaction.commit();
        return result;
    }

    if (!current) {
        if (!expected_journal_head.empty() || !valid_initial_publication(publication)) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    } else if (!valid_increment(current->publication, publication, expected_journal_head)) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }

    ProgramTransitionPublication candidate = current
                                                 ? merged_publication(*current, std::move(publication))
                                                 : std::move(publication);
    std::string candidate_bytes;
    try {
        candidate_bytes = candidate.serialize_canonical();
    } catch (const std::invalid_argument&) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }

    if (!current) {
        Statement statement(
            impl_->db,
            "INSERT INTO program_transition_runs"
            "(owner_scope, run_id, canonical_bytes, last_publication_bytes) VALUES(?1, ?2, ?3, ?4)");
        statement.bind_text(1, owner_scope);
        statement.bind_text(2, run_id);
        statement.bind_blob(3, candidate_bytes);
        statement.bind_blob(4, candidate_bytes);
        statement.step_done();
    } else {
        Statement statement(
            impl_->db,
            "UPDATE program_transition_runs SET canonical_bytes = ?1, last_publication_bytes = ?2 "
            "WHERE owner_scope = ?3 AND run_id = ?4");
        statement.bind_blob(1, candidate_bytes);
        statement.bind_blob(2, publication_bytes);
        statement.bind_text(3, owner_scope);
        statement.bind_text(4, run_id);
        statement.step_done();
    }
    transaction.commit();
    return ProgramTransitionPublishResult::Published;
}

}  // namespace neograph::program
