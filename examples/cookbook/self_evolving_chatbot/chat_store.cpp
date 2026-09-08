#include "chat_store.h"

#include <stdexcept>
#if defined(CHAT_SQLITE)
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>

#include <sqlite3.h>
#endif
#if defined(CHAT_POSTGRES)
#include <neograph/graph/postgres_checkpoint.h>
#include <neograph/program/postgres_store.h>
#include <neograph/program/postgres_transition_store.h>

#include <libpq-fe.h>
#endif

namespace evolving_chat {
using namespace neograph::program;
struct ChatStore::Impl {
    std::mutex mutex;
#if defined(CHAT_SQLITE)
    sqlite3* sqlite = nullptr;
#endif
#if defined(CHAT_POSTGRES)
    PGconn* pg = nullptr;
#endif
    ~Impl() {
#if defined(CHAT_SQLITE)
        if (sqlite) sqlite3_close(sqlite);
#endif
#if defined(CHAT_POSTGRES)
        if (pg) PQfinish(pg);
#endif
    }
};
ChatStore::ChatStore(const Options& o) : impl_(std::make_unique<Impl>()) {
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS neograph_chat_sessions ("
        "owner TEXT PRIMARY KEY, revision BIGINT NOT NULL, body TEXT NOT NULL)";
    if (!o.postgres_url.empty()) {
#if defined(CHAT_POSTGRES)
        impl_->pg = PQconnectdb(o.postgres_url.c_str());
        if (PQstatus(impl_->pg) != CONNECTION_OK)
            throw std::runtime_error("Chat PostgreSQL connection failed");
        auto r  = PQexec(impl_->pg, ddl);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        PQclear(r);
        if (!ok) throw std::runtime_error("Chat PostgreSQL schema failed");
        programs    = std::make_shared<PostgreSQLProgramStore>(o.postgres_url);
        transitions = std::make_shared<PostgreSQLProgramTransitionStore>(o.postgres_url);
        checkpoints = std::make_shared<neograph::graph::PostgresCheckpointStore>(o.postgres_url, 4);
#else
        throw std::runtime_error("Build with NEOGRAPH_BUILD_POSTGRES=ON");
#endif
    } else {
#if defined(CHAT_SQLITE)
        if (sqlite3_open(o.database.c_str(), &impl_->sqlite) != SQLITE_OK)
            throw std::runtime_error("Chat SQLite connection failed");
        sqlite3_busy_timeout(impl_->sqlite, 10000);
        if (sqlite3_exec(impl_->sqlite, ddl, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Chat SQLite schema failed");
        programs    = std::make_shared<SQLiteProgramStore>(o.database);
        transitions = std::make_shared<SQLiteProgramTransitionStore>(o.database);
        checkpoints = std::make_shared<neograph::graph::SqliteCheckpointStore>(o.database);
#else
        throw std::runtime_error("Build with NEOGRAPH_BUILD_SQLITE=ON");
#endif
    }
}
ChatStore::~ChatStore() = default;
json ChatStore::load(const std::string& owner) {
    std::lock_guard lock(impl_->mutex);
#if defined(CHAT_POSTGRES)
    if (impl_->pg) {
        const char* values[] = {owner.c_str()};
        auto r = PQexecParams(impl_->pg, "SELECT body FROM neograph_chat_sessions WHERE owner=$1",
                              1, nullptr, values, nullptr, nullptr, 0);
        std::unique_ptr<PGresult, decltype(&PQclear)> result(r, PQclear);
        if (PQresultStatus(r) != PGRES_TUPLES_OK)
            throw std::runtime_error("Chat snapshot read failed");
        return PQntuples(r) ? json::parse(PQgetvalue(r, 0, 0)) : json();
    }
#endif
#if defined(CHAT_SQLITE)
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(impl_->sqlite, "SELECT body FROM neograph_chat_sessions WHERE owner=?1",
                           -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Chat snapshot read failed");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
    sqlite3_bind_text(raw, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(raw);
    if (rc == SQLITE_DONE) return json();
    if (rc != SQLITE_ROW) throw std::runtime_error("Chat snapshot read failed");
    return json::parse(reinterpret_cast<const char*>(sqlite3_column_text(raw, 0)));
#else
    throw std::runtime_error("No chat storage backend");
#endif
}
void ChatStore::save(const std::string& owner, json& value) {
    std::lock_guard lock(impl_->mutex);
    const auto      previous = value.value("revision", 0ULL);
    auto            next     = json::parse(value.dump());
    next["revision"]         = previous + 1;
    const auto bytes         = next.dump();
    if (bytes.size() > 4 * 1024 * 1024)
        throw std::runtime_error("Chat snapshot size limit reached");
    const auto revision = std::to_string(previous + 1), expected = std::to_string(previous);
#if defined(CHAT_POSTGRES)
    if (impl_->pg) {
        const char* values[] = {owner.c_str(), revision.c_str(), bytes.c_str(), expected.c_str()};
        auto        r        = PQexecParams(
            impl_->pg,
            previous ? "UPDATE neograph_chat_sessions SET revision=$2::bigint,body=$3 WHERE "
                                     "owner=$1 AND revision=$4::bigint RETURNING revision"
                                   : "INSERT INTO neograph_chat_sessions(owner,revision,body) "
                                     "VALUES($1,$2::bigint,$3) ON CONFLICT DO NOTHING RETURNING revision",
            previous ? 4 : 3, nullptr, values, nullptr, nullptr, 0);
        std::unique_ptr<PGresult, decltype(&PQclear)> result(r, PQclear);
        if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1)
            throw std::runtime_error("Chat snapshot CAS conflict or storage failure");
        value = std::move(next);
        return;
    }
#endif
#if defined(CHAT_SQLITE)
    sqlite3_stmt* raw = nullptr;
    const char*   sql =
        previous
              ? "UPDATE neograph_chat_sessions SET revision=?2,body=?3 WHERE owner=?1 AND revision=?4"
              : "INSERT OR IGNORE INTO neograph_chat_sessions(owner,revision,body) VALUES(?1,?2,?3)";
    if (sqlite3_prepare_v2(impl_->sqlite, sql, -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Chat snapshot write failed");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
    sqlite3_bind_text(raw, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(raw, 2, static_cast<sqlite3_int64>(previous + 1));
    sqlite3_bind_text(raw, 3, bytes.c_str(), -1, SQLITE_TRANSIENT);
    if (previous) sqlite3_bind_int64(raw, 4, static_cast<sqlite3_int64>(previous));
    if (sqlite3_step(raw) != SQLITE_DONE || sqlite3_changes(impl_->sqlite) != 1)
        throw std::runtime_error("Chat snapshot CAS conflict or storage failure");
    value = std::move(next);
#else
    throw std::runtime_error("No chat storage backend");
#endif
}
}  // namespace evolving_chat
