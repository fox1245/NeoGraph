#include <neograph/program/sqlite_store.h>

#include <sqlite3.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace neograph::program {
namespace {

[[noreturn]] void throw_sqlite(sqlite3* db, std::string_view operation) {
    throw std::runtime_error(std::string(operation) + ": " + (db ? sqlite3_errmsg(db) : "SQLite unavailable"));
}

void exec(sqlite3* db, std::string_view sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql.data(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error(std::string("SQLite ") + std::string(sql.substr(0, 32)) + ": " + message);
    }
}

class Statement final {
public:
    Statement(sqlite3* db, std::string_view sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr) !=
            SQLITE_OK)
            throw_sqlite(db_, "prepare");
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() { sqlite3_finalize(statement_); }

    sqlite3_stmt* get() const noexcept { return statement_; }

    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
            throw_sqlite(db_, "bind text");
    }

    void bind_blob(int index, std::string_view value) {
        if (sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
            throw_sqlite(db_, "bind blob");
    }

    void bind_int64(int index, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
            throw std::overflow_error("SQLite integer value is too large");
        if (sqlite3_bind_int64(statement_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            throw_sqlite(db_, "bind integer");
    }

    bool step_row() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw_sqlite(db_, "step");
    }

    void step_done() {
        if (sqlite3_step(statement_) != SQLITE_DONE) throw_sqlite(db_, "step");
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
            char* ignored = nullptr;
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &ignored);
            sqlite3_free(ignored);
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

std::string column_bytes(sqlite3_stmt* statement, int index) {
    const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement, index));
    const int   size  = sqlite3_column_bytes(statement, index);
    if (!bytes || size < 0) throw std::runtime_error("SQLite returned a null Program record");
    return std::string(bytes, static_cast<std::size_t>(size));
}

std::optional<std::string> stored_bytes(sqlite3* db, std::string_view table, std::string_view id,
                                        std::string_view id_column = "id") {
    const std::string sql = "SELECT canonical_bytes FROM " + std::string(table) + " WHERE " +
                            std::string(id_column) + " = ?1";
    Statement statement(db, sql);
    statement.bind_text(1, id);
    if (!statement.step_row()) return std::nullopt;
    return column_bytes(statement.get(), 0);
}

std::optional<ProgramActivation> load_activation(sqlite3* db, std::string_view owner_scope) {
    Statement statement(db,
                        "SELECT canonical_bytes FROM program_activations WHERE owner_scope = ?1");
    statement.bind_text(1, owner_scope);
    if (!statement.step_row()) return std::nullopt;
    return ProgramActivation::parse(column_bytes(statement.get(), 0));
}

void verify_existing(std::optional<std::string> existing, std::string_view expected,
                     std::string_view kind) {
    if (existing && *existing != expected)
        throw std::invalid_argument(std::string("SQLite ") + std::string(kind) +
                                    " content identity collision");
}

}  // namespace

struct SQLiteProgramStore::Impl {
    explicit Impl(std::string value) : path(std::move(value)) {}
    ~Impl() {
        if (db) sqlite3_close_v2(db);
    }

    std::string       path;
    sqlite3*          db = nullptr;
    mutable std::mutex mutex;
};

SQLiteProgramStore::SQLiteProgramStore(std::string database_path)
    : impl_(std::make_unique<Impl>(std::move(database_path))) {
    if (impl_->path.empty()) throw std::invalid_argument("SQLite ProgramStore path must not be empty");
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(impl_->path.c_str(), &impl_->db, flags, nullptr) != SQLITE_OK)
        throw_sqlite(impl_->db, "open");
    try {
        exec(impl_->db, "PRAGMA foreign_keys = ON");
        exec(impl_->db,
             "CREATE TABLE IF NOT EXISTS program_bundles ("
             "id TEXT PRIMARY KEY NOT NULL, canonical_bytes BLOB NOT NULL)");
        exec(impl_->db,
             "CREATE TABLE IF NOT EXISTS program_versions ("
             "id TEXT PRIMARY KEY NOT NULL, bundle_id TEXT NOT NULL, owner_scope TEXT NOT NULL, "
             "canonical_bytes BLOB NOT NULL)");
        exec(impl_->db,
             "CREATE TABLE IF NOT EXISTS program_activations ("
             "owner_scope TEXT PRIMARY KEY NOT NULL, generation INTEGER NOT NULL, "
             "active_version_id TEXT NOT NULL, policy_snapshot_hash TEXT NOT NULL, "
             "canonical_bytes BLOB NOT NULL)");
        exec(impl_->db,
             "CREATE INDEX IF NOT EXISTS program_versions_owner_idx ON program_versions(owner_scope)");
    } catch (...) {
        sqlite3_close_v2(impl_->db);
        impl_->db = nullptr;
        throw;
    }
}

SQLiteProgramStore::SQLiteProgramStore(SQLiteProgramStore&&) noexcept = default;
SQLiteProgramStore& SQLiteProgramStore::operator=(SQLiteProgramStore&&) noexcept = default;
SQLiteProgramStore::~SQLiteProgramStore() = default;

void SQLiteProgramStore::publish_admitted(const ProgramBundle& bundle, const ProgramVersion& version) {
    if (version.bundle_id() != bundle.id())
        throw std::invalid_argument("Program version does not bind the published bundle");
    const auto bundle_bytes  = bundle.serialize_canonical();
    const auto version_bytes = version.serialize_canonical();

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    verify_existing(stored_bytes(impl_->db, "program_bundles", bundle.id()), bundle_bytes, "bundle");
    verify_existing(stored_bytes(impl_->db, "program_versions", version.id()), version_bytes,
                    "version");

    {
        Statement statement(impl_->db,
                            "INSERT OR IGNORE INTO program_bundles(id, canonical_bytes) VALUES(?1, ?2)");
        statement.bind_text(1, bundle.id());
        statement.bind_blob(2, bundle_bytes);
        statement.step_done();
    }
    {
        Statement statement(impl_->db,
                            "INSERT OR IGNORE INTO program_versions"
                            "(id, bundle_id, owner_scope, canonical_bytes) VALUES(?1, ?2, ?3, ?4)");
        statement.bind_text(1, version.id());
        statement.bind_text(2, version.bundle_id());
        statement.bind_text(3, version.ownership_scope());
        statement.bind_blob(4, version_bytes);
        statement.step_done();
    }
    transaction.commit();
}

std::optional<ProgramBundle> SQLiteProgramStore::get_bundle(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto bytes = stored_bytes(impl_->db, "program_bundles", id);
    if (!bytes) return std::nullopt;
    return ProgramBundle::parse(*bytes);
}

std::optional<ProgramVersion> SQLiteProgramStore::get_version(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto bytes = stored_bytes(impl_->db, "program_versions", id);
    if (!bytes) return std::nullopt;
    return ProgramVersion::parse(*bytes);
}

std::optional<ProgramActivation>
SQLiteProgramStore::get_activation(std::string_view owner_scope) const {
    std::lock_guard lock(impl_->mutex);
    return load_activation(impl_->db, owner_scope);
}

ProgramActivationResult SQLiteProgramStore::compare_activate(
    std::string_view owner_scope,
    std::uint64_t expected_generation,
    std::string_view version_id,
    std::string_view policy_snapshot_hash) {
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);

    const auto bytes = stored_bytes(impl_->db, "program_versions", version_id);
    if (!bytes) throw std::invalid_argument("Cannot activate an unpublished Program version");
    const auto version = ProgramVersion::parse(*bytes);
    if (version.ownership_scope() != owner_scope)
        throw std::invalid_argument("Program activation owner scope does not match the version");

    const auto current = load_activation(impl_->db, owner_scope);
    const auto generation = current ? current->generation() : std::uint64_t{0};
    if (generation != expected_generation) {
        transaction.commit();
        return ProgramActivationResult::Conflict;
    }
    if (current && current->active_version_id() == version_id &&
        current->policy_snapshot_hash() == policy_snapshot_hash) {
        transaction.commit();
        return ProgramActivationResult::AlreadyPresent;
    }
    if (expected_generation == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Program activation generation exhausted");

    auto activation = ProgramActivation::create(
        ProgramActivationData{std::string(owner_scope), std::string(version_id), expected_generation + 1,
                              std::string(policy_snapshot_hash)});
    const auto activation_bytes = activation.serialize_canonical();
    Statement statement(
        impl_->db,
        "INSERT INTO program_activations"
        "(owner_scope, generation, active_version_id, policy_snapshot_hash, canonical_bytes)"
        " VALUES(?1, ?2, ?3, ?4, ?5)"
        " ON CONFLICT(owner_scope) DO UPDATE SET generation=excluded.generation,"
        " active_version_id=excluded.active_version_id, policy_snapshot_hash=excluded.policy_snapshot_hash,"
        " canonical_bytes=excluded.canonical_bytes");
    statement.bind_text(1, owner_scope);
    statement.bind_int64(2, activation.generation());
    statement.bind_text(3, activation.active_version_id());
    statement.bind_text(4, activation.policy_snapshot_hash());
    statement.bind_blob(5, activation_bytes);
    statement.step_done();
    transaction.commit();
    return ProgramActivationResult::Activated;
}

std::vector<ProgramVersion>
SQLiteProgramStore::list_versions(std::string_view owner_scope) const {
    std::lock_guard lock(impl_->mutex);
    Statement statement(impl_->db,
                        "SELECT canonical_bytes FROM program_versions WHERE owner_scope = ?1 ORDER BY id");
    statement.bind_text(1, owner_scope);
    std::vector<ProgramVersion> result;
    while (statement.step_row()) result.push_back(ProgramVersion::parse(column_bytes(statement.get(), 0)));
    return result;
}

ProgramRetentionReport SQLiteProgramStore::collect_garbage(
    std::string_view owner_scope,
    const std::vector<std::string>& pinned_version_ids) {
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    std::set<std::string, std::less<>> keep(pinned_version_ids.begin(), pinned_version_ids.end());
    const auto active = load_activation(impl_->db, owner_scope);
    if (active) keep.insert(active->active_version_id());

    Statement list(impl_->db,
                   "SELECT id FROM program_versions WHERE owner_scope = ?1 ORDER BY id");
    list.bind_text(1, owner_scope);
    std::vector<std::string> remove;
    while (list.step_row()) {
        const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(list.get(), 0));
        if (id && !keep.contains(id)) remove.emplace_back(id);
    }

    ProgramRetentionReport report;
    for (const auto& id : remove) {
        Statement statement(impl_->db, "DELETE FROM program_versions WHERE id = ?1");
        statement.bind_text(1, id);
        statement.step_done();
        ++report.versions_removed;
    }
    exec(impl_->db,
         "DELETE FROM program_bundles WHERE id NOT IN "
         "(SELECT DISTINCT bundle_id FROM program_versions)");
    report.bundles_removed = static_cast<std::uint64_t>(sqlite3_changes(impl_->db));
    transaction.commit();
    return report;
}

}  // namespace neograph::program
