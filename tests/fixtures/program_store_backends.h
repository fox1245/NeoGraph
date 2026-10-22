#pragma once

// The Program stores a conformance suite runs on. The in-memory stores are the
// reference. SQLite runs on a database file in a private directory, so a second
// connection or a reopen sees the same data. PostgreSQL runs when
// NEOGRAPH_TEST_POSTGRES_URL names the disposable neograph_test database; those
// cases drop the Program tables, so CMake runs them under the postgres_test_db
// resource lock.
//
// A StoreDeployment is one database. It hands out stores over that database,
// injects a failure into one write of a publication, and corrupts a stored
// record, so a case written once can check the same contract clause on every
// backend. A backend that cannot take part in a case says why and the case is
// skipped with that reason.
//
// A store is called in two forms (StoreForm). Direct calls a synchronous store's own
// operations, as synchronous code does. Awaited calls the awaitable operations that the
// Program runtime and a host's coroutine call, and waits for each on a private io_context.
// The conformance suites run on every backend in both forms (store_cases()).
//
// JavaScript runs the suites against the adapters of wasm/js_stores.h over the reference JavaScript store (the
// in-memory C++ stores, wasm/memory_stores.h), under Node, in the Emscripten build. It is built only when
// NEOGRAPH_PROGRAM_TESTS_HAVE_JS_STORES is defined. It keeps its values in the process like the in-memory backend, so it
// has no stored form to restart from or to corrupt, and its cases that need one are skipped with the same reasons.

#include "unique_temp_dir.h"

#include <neograph/program/store.h>
#include <neograph/program/store_call.h>
#include <neograph/program/transition_store.h>
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>
#include <sqlite3.h>
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
#include <neograph/program/postgres_store.h>
#include <neograph/program/postgres_transition_store.h>
#include <libpq-fe.h>
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_JS_STORES
#include "../../wasm/js_stores.h"
#include "../../wasm/test/js_store_deployment.h"
#endif

#include <cstdlib>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program::test_support {

enum class StoreBackend { InMemory, Sqlite, PostgreSql, JavaScript };

inline const char* backend_name(StoreBackend backend) {
    switch (backend) {
    case StoreBackend::InMemory: return "InMemory";
    case StoreBackend::Sqlite: return "SQLite";
    case StoreBackend::PostgreSql: return "PostgreSQL";
    case StoreBackend::JavaScript: return "JavaScript";
    }
    return "Unknown";
}

// Whether the backend keeps its data in a stored form that a restart reads and a tamper can change. The in-memory and
// JavaScript backends keep typed values in the process.
inline bool backend_is_durable(StoreBackend backend) noexcept {
    return backend == StoreBackend::Sqlite || backend == StoreBackend::PostgreSql;
}

// Names the parameter in test listings instead of its bytes.
inline void PrintTo(StoreBackend backend, std::ostream* out) { *out << backend_name(backend); }

inline std::vector<StoreBackend> store_backends() {
    std::vector<StoreBackend> out{StoreBackend::InMemory};
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
    out.push_back(StoreBackend::Sqlite);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
    out.push_back(StoreBackend::PostgreSql);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_JS_STORES
    out.push_back(StoreBackend::JavaScript);
#endif
    return out;
}

enum class StoreForm { Direct, Awaited };

inline const char* form_name(StoreForm form) { return form == StoreForm::Direct ? "Direct" : "Awaited"; }

// One conformance case: a backend, and the form its store is called in.
struct StoreCase {
    StoreBackend backend = StoreBackend::InMemory;
    StoreForm    form    = StoreForm::Direct;
};

// The Direct form keeps the backend's name, so a case keeps the name it had before the forms.
inline std::string case_name(const StoreCase& value) {
    return std::string(backend_name(value.backend)) + (value.form == StoreForm::Awaited ? "Awaited" : "");
}

inline void PrintTo(const StoreCase& value, std::ostream* out) { *out << case_name(value); }

inline std::vector<StoreCase> store_cases() {
    std::vector<StoreCase> out;
    for (const auto backend : store_backends())
        for (const auto form : {StoreForm::Direct, StoreForm::Awaited}) out.push_back({backend, form});
    return out;
}

inline BlockingStorePolicy form_policy(StoreForm form) {
    BlockingStorePolicy policy;
    policy.await_synchronous_stores = form == StoreForm::Awaited;
    return policy;
}

inline const char* postgres_test_url() {
    const char* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    return url && *url ? url : nullptr;
}

// Why the backend cannot run here; empty when it can.
inline std::optional<std::string> backend_unavailable_reason(StoreBackend backend) {
    if (backend == StoreBackend::PostgreSql && !postgres_test_url())
        return "NEOGRAPH_TEST_POSTGRES_URL is not set";
    return std::nullopt;
}

// The writes one publication makes in the same transaction. Failing one of them
// must leave every other write of that publication unobservable.
enum class WriteStage { Run, Events, Effects, Context, Hooks, Lineage };

inline const char* write_stage_name(WriteStage stage) {
    switch (stage) {
    case WriteStage::Run: return "Run";
    case WriteStage::Events: return "Events";
    case WriteStage::Effects: return "Effects";
    case WriteStage::Context: return "Context";
    case WriteStage::Hooks: return "Hooks";
    case WriteStage::Lineage: return "Lineage";
    }
    return "Unknown";
}

// The writes the catalog store makes in one transaction.
enum class CatalogWriteStage { Version, Activation };

namespace detail {

inline std::string quote_literal(std::string_view value) {
    std::string out{"'"};
    for (const char c : value) {
        if (c == '\'') out.push_back('\'');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

struct StageTable {
    const char* table;
    bool        updates;  // the stores UPDATE this table as well as INSERT into it
};

inline StageTable transition_stage_table(StoreBackend backend, WriteStage stage) {
    const bool pg = backend == StoreBackend::PostgreSql;
    switch (stage) {
    case WriteStage::Run:
        return {pg ? "neograph_program_transition_run_heads_v2"
                   : "program_transition_run_heads_v2", true};
    case WriteStage::Events:
        return {pg ? "neograph_program_transition_event_log_v2"
                   : "program_transition_event_log_v2", false};
    case WriteStage::Effects:
        return {pg ? "neograph_program_transition_effect_log_v2"
                   : "program_transition_effect_log_v2", false};
    case WriteStage::Context:
        return {pg ? "neograph_program_transition_context_log_v1"
                   : "program_transition_context_log_v1", false};
    case WriteStage::Hooks:
        return {pg ? "neograph_program_transition_hook_outbox_log_v1"
                   : "program_transition_hook_outbox_log_v1", false};
    case WriteStage::Lineage:
        return {pg ? "neograph_program_run_lineage_heads_v1"
                   : "program_run_lineage_heads_v1", true};
    }
    throw std::logic_error("unknown write stage");
}

inline StageTable catalog_stage_table(StoreBackend backend, CatalogWriteStage stage) {
    const bool pg = backend == StoreBackend::PostgreSql;
    switch (stage) {
    case CatalogWriteStage::Version:
        return {pg ? "neograph_program_versions" : "program_versions", false};
    case CatalogWriteStage::Activation:
        return {pg ? "neograph_program_activations" : "program_activations", true};
    }
    throw std::logic_error("unknown catalog write stage");
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
// Executes SQL on a fresh connection to the test database.
inline void postgres_execute(const char* url, const std::string& sql) {
    std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
        throw std::runtime_error("PostgreSQL conformance connection failed: " +
                                 std::string(connection ? PQerrorMessage(connection.get()) : ""));
    std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), sql.c_str()),
                                                          PQclear);
    const auto status = result ? PQresultStatus(result.get()) : PGRES_FATAL_ERROR;
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
        throw std::runtime_error("PostgreSQL conformance statement failed: " +
                                 std::string(PQerrorMessage(connection.get())));
}

// Empties the Program tables of the disposable test database.
inline void reset_postgres_program_tables(const char* url) {
    std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
        throw std::runtime_error("PostgreSQL conformance reset failed: " +
                                 std::string(connection ? PQerrorMessage(connection.get()) : ""));
    std::unique_ptr<PGresult, decltype(&PQclear)> database(
        PQexec(connection.get(), "SELECT current_database()"), PQclear);
    const bool known = database && PQresultStatus(database.get()) == PGRES_TUPLES_OK &&
                       PQntuples(database.get()) == 1 && PQnfields(database.get()) == 1;
    if (!known || std::string(PQgetvalue(database.get(), 0, 0)) != "neograph_test")
        throw std::runtime_error(
            "PostgreSQL conformance tests require the disposable neograph_test database");
    postgres_execute(url, R"SQL(
DROP TABLE IF EXISTS neograph_program_execution_leases_v1;
DROP TABLE IF EXISTS neograph_program_graph_migration_capsules_v1;
DROP TABLE IF EXISTS neograph_program_run_lineage_runs_v1;
DROP TABLE IF EXISTS neograph_program_run_lineage_history_v1;
DROP TABLE IF EXISTS neograph_program_run_generation_publications_v1;
DROP TABLE IF EXISTS neograph_program_run_generations_v1;
DROP TABLE IF EXISTS neograph_program_run_lineage_heads_v1;
DROP TABLE IF EXISTS neograph_program_transition_synthesis_log_v1;
DROP TABLE IF EXISTS neograph_program_transition_hook_outbox_log_v1;
DROP TABLE IF EXISTS neograph_program_transition_context_log_v1;
DROP TABLE IF EXISTS neograph_program_transition_javascript_command_log_v2;
DROP TABLE IF EXISTS neograph_program_transition_effect_log_v2;
DROP TABLE IF EXISTS neograph_program_transition_event_log_v2;
DROP TABLE IF EXISTS neograph_program_transition_run_heads_v2;
DROP TABLE IF EXISTS neograph_program_activations;
DROP TABLE IF EXISTS neograph_program_versions;
DROP TABLE IF EXISTS neograph_program_bundles;
)SQL");
}
#endif

}  // namespace detail

class StoreDeployment {
public:
    explicit StoreDeployment(StoreBackend backend, StoreForm form = StoreForm::Direct)
        : backend_(backend), form_(form) {
        if (backend == StoreBackend::InMemory) {
            memory_transitions_ = std::make_shared<InMemoryProgramTransitionStore>();
            memory_catalog_     = std::make_shared<InMemoryProgramStore>();
            return;
        }
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_JS_STORES
        if (backend == StoreBackend::JavaScript) {
            reference_ = std::make_unique<wasm::testing::ReferenceStores>();
            return;
        }
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (backend == StoreBackend::Sqlite) {
            directory_.emplace("neograph-program-conformance-");
            database_ = directory_->file("program.db");
            return;
        }
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (backend == StoreBackend::PostgreSql) {
            url_ = postgres_test_url();
            if (!url_) throw std::runtime_error("NEOGRAPH_TEST_POSTGRES_URL is not set");
            detail::reset_postgres_program_tables(url_);
            return;
        }
#endif
        throw std::logic_error(std::string(backend_name(backend)) + " is not built");
    }

    ~StoreDeployment() {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (sqlite_) sqlite3_close_v2(sqlite_);
#endif
    }

    StoreDeployment(const StoreDeployment&)            = delete;
    StoreDeployment& operator=(const StoreDeployment&) = delete;

    StoreBackend backend() const noexcept { return backend_; }
    StoreForm    form() const noexcept { return form_; }

    // False for the stores that keep typed values in the process (in memory, JavaScript): they have no stored form to
    // restart from or corrupt.
    bool durable() const noexcept { return backend_is_durable(backend_); }

    // A store over this deployment's data. The in-memory deployment hands out its one
    // store each time; a database deployment opens a new connection each time, so two
    // calls are two writers and a call after the first store is gone is a restart.
    std::shared_ptr<ProgramTransitionStore> transition_store() const {
        if (backend_ == StoreBackend::InMemory) return memory_transitions_;
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_JS_STORES
        // New adapters each time, over the one reference store: a second call is a second writer on the same data.
        if (backend_ == StoreBackend::JavaScript) return wasm::adapt_js_stores(reference_->handle()).transitions;
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (backend_ == StoreBackend::Sqlite)
            return std::make_shared<SQLiteProgramTransitionStore>(database_);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (backend_ == StoreBackend::PostgreSql)
            return std::make_shared<PostgreSQLProgramTransitionStore>(url_);
#endif
        throw std::logic_error("backend is not built");
    }

    // The same store for a case that cannot suspend, called in this deployment's form.
    std::shared_ptr<BlockingProgramTransitionStore> blocking_transition_store() const {
        return std::make_shared<BlockingProgramTransitionStore>(transition_store(), form_policy(form_));
    }

    std::shared_ptr<BlockingProgramStore> blocking_program_store() const {
        return std::make_shared<BlockingProgramStore>(program_store(), form_policy(form_));
    }

    std::shared_ptr<ProgramStore> program_store() const {
        if (backend_ == StoreBackend::InMemory) return memory_catalog_;
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_JS_STORES
        if (backend_ == StoreBackend::JavaScript) return wasm::adapt_js_stores(reference_->handle()).programs;
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (backend_ == StoreBackend::Sqlite)
            return std::make_shared<SQLiteProgramStore>(database_);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (backend_ == StoreBackend::PostgreSql)
            return std::make_shared<PostgreSQLProgramStore>(url_);
#endif
        throw std::logic_error("backend is not built");
    }

    // The next write of `stage` inside a publication fails with std::runtime_error. The
    // in-memory store fails at the matching staged boundary; a database fails the write
    // itself, through a trigger (SQLite) or a check constraint (PostgreSQL), so the rest of
    // the publication's transaction has already run when it fails.
    void arm_fault(WriteStage stage) {
        if (backend_ == StoreBackend::InMemory) {
            memory_transitions_->fail_next_publication_for_testing(memory_fault_point(stage));
            return;
        }
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_JS_STORES
        if (backend_ == StoreBackend::JavaScript) {
            reference_->fail_next_publication(memory_fault_point(stage));
            return;
        }
#endif
        arm_table_fault(detail::transition_stage_table(backend_, stage));
    }

    // True when the backend can fail a catalog write; the in-memory catalog store has no
    // injection point.
    bool can_fault_catalog() const noexcept { return durable(); }

    void arm_catalog_fault(CatalogWriteStage stage) {
        if (!can_fault_catalog()) throw std::logic_error("no catalog fault injection");
        arm_table_fault(detail::catalog_stage_table(backend_, stage));
    }

    // Removes every armed database fault.
    void disarm_faults() {
        for (const auto& table : armed_) {
            if (backend_ == StoreBackend::Sqlite) {
                sqlite_execute("DROP TRIGGER IF EXISTS neograph_fault_insert_" + table);
                sqlite_execute("DROP TRIGGER IF EXISTS neograph_fault_update_" + table);
            } else {
                postgres_execute("ALTER TABLE " + table +
                                 " DROP CONSTRAINT IF EXISTS neograph_injected_fault");
            }
        }
        armed_.clear();
    }

    // Replaces a stored run record with bytes that are not a canonical record.
    void corrupt_run_record(std::string_view owner_scope, std::string_view run_id) {
        const auto table = detail::transition_stage_table(backend_, WriteStage::Run).table;
        execute("UPDATE " + std::string(table) + " SET run_record_bytes = 'broken' WHERE " +
                "owner_scope = " + detail::quote_literal(owner_scope) +
                " AND run_id = " + detail::quote_literal(run_id));
    }

    // Gives a stored generation row an identity column that its record's bytes do not carry.
    void corrupt_generation_identity(std::string_view owner_scope, std::string_view lineage_id) {
        const bool pg = backend_ == StoreBackend::PostgreSql;
        execute(std::string("UPDATE ") +
                (pg ? "neograph_program_run_generations_v1" : "program_run_generations_v1") +
                " SET generation_id = 'tampered' WHERE owner_scope = " +
                detail::quote_literal(owner_scope) +
                " AND lineage_id = " + detail::quote_literal(lineage_id));
    }

    // Gives a stored command row a coordinate column that the entry in its bytes does not carry.
    void corrupt_command_coordinate(std::string_view owner_scope, std::string_view run_id) {
        execute("UPDATE " + table("program_transition_javascript_command_log_v2") +
                " SET coordinate_id = 'tampered' WHERE owner_scope = " +
                detail::quote_literal(owner_scope) + " AND run_id = " + detail::quote_literal(run_id));
    }

    // Gives a stored execution lease an id column that the lease in its bytes does not carry.
    void corrupt_lease_identity(std::string_view owner_scope, std::string_view run_id) {
        execute("UPDATE " + table("program_execution_leases_v1") +
                " SET lease_id = 'tampered' WHERE owner_scope = " +
                detail::quote_literal(owner_scope) + " AND run_id = " + detail::quote_literal(run_id));
    }

    // Replaces the stored head and the stored history of one lineage with the records of
    // another, so the records no longer carry the lineage they are stored under.
    void replace_lineage_records(std::string_view owner_scope, std::string_view from_lineage,
                                 std::string_view into_lineage) {
        const auto owner = detail::quote_literal(owner_scope);
        const auto from  = detail::quote_literal(from_lineage);
        const auto into  = detail::quote_literal(into_lineage);
        execute("UPDATE " + table("program_run_lineage_heads_v1") +
                " SET head_bytes = (SELECT head_bytes FROM " +
                table("program_run_lineage_heads_v1") + " WHERE owner_scope = " + owner +
                " AND lineage_id = " + from + ") WHERE owner_scope = " + owner +
                " AND lineage_id = " + into);
        execute("UPDATE " + table("program_run_lineage_history_v1") +
                " SET canonical_bytes = (SELECT canonical_bytes FROM " +
                table("program_run_lineage_history_v1") + " WHERE owner_scope = " + owner +
                " AND lineage_id = " + from + " LIMIT 1) WHERE owner_scope = " + owner +
                " AND lineage_id = " + into);
    }

    // Stores a graph migration capsule under an id column that the capsule in its bytes does
    // not carry.
    void insert_capsule_with_foreign_identity(std::string_view owner_scope,
                                              std::string_view source_run_id,
                                              std::string_view source_lineage_head_id,
                                              std::string_view capsule_bytes) {
        execute("INSERT INTO " + table("program_graph_migration_capsules_v1") +
                "(owner_scope, source_run_id, source_lineage_head_id, capsule_id, capsule_bytes) "
                "VALUES(" + detail::quote_literal(owner_scope) + ", " +
                detail::quote_literal(source_run_id) + ", " +
                detail::quote_literal(source_lineage_head_id) + ", 'tampered', " +
                detail::quote_literal(capsule_bytes) + ")");
    }

    // Replaces a stored version with bytes that are not a canonical version.
    void corrupt_program_version(std::string_view version_id) {
        const auto table = detail::catalog_stage_table(backend_, CatalogWriteStage::Version).table;
        execute("UPDATE " + std::string(table) + " SET canonical_bytes = 'broken' WHERE id = " +
                detail::quote_literal(version_id));
    }

private:
    // A table of the Program schema: the SQLite name, or the same name under the PostgreSQL prefix.
    std::string table(std::string_view name) const {
        return backend_ == StoreBackend::PostgreSql ? "neograph_" + std::string(name)
                                                    : std::string(name);
    }

    static ProgramTransitionFaultPoint memory_fault_point(WriteStage stage) {
        switch (stage) {
        case WriteStage::Run: return ProgramTransitionFaultPoint::AfterRunSnapshot;
        case WriteStage::Events: return ProgramTransitionFaultPoint::AfterEventSnapshot;
        case WriteStage::Effects: return ProgramTransitionFaultPoint::AfterEffectSnapshot;
        case WriteStage::Context: return ProgramTransitionFaultPoint::AfterContextSnapshot;
        case WriteStage::Hooks: return ProgramTransitionFaultPoint::AfterHookSnapshot;
        case WriteStage::Lineage: return ProgramTransitionFaultPoint::AfterLineageSnapshot;
        }
        throw std::logic_error("unknown write stage");
    }

    void arm_table_fault(const detail::StageTable& target) {
        const std::string table = target.table;
        if (backend_ == StoreBackend::Sqlite) {
            sqlite_execute("CREATE TRIGGER neograph_fault_insert_" + table + " BEFORE INSERT ON " +
                           table + " BEGIN SELECT RAISE(ABORT, 'injected write failure'); END");
            if (target.updates)
                sqlite_execute("CREATE TRIGGER neograph_fault_update_" + table +
                               " BEFORE UPDATE ON " + table +
                               " BEGIN SELECT RAISE(ABORT, 'injected write failure'); END");
        } else {
            // A NOT VALID constraint is not checked against existing rows but is enforced on
            // every insert and update that follows.
            postgres_execute("ALTER TABLE " + table +
                             " ADD CONSTRAINT neograph_injected_fault CHECK (false) NOT VALID");
        }
        armed_.push_back(table);
    }

    void execute(const std::string& sql) {
        if (backend_ == StoreBackend::Sqlite) sqlite_execute(sql);
        else if (backend_ == StoreBackend::PostgreSql) postgres_execute(sql);
        else throw std::logic_error("the in-memory stores have no stored form");
    }

    void sqlite_execute([[maybe_unused]] const std::string& sql) {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (!sqlite_) {
            if (sqlite3_open_v2(database_.c_str(), &sqlite_,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
                throw std::runtime_error("conformance SQLite open failed");
            sqlite3_busy_timeout(sqlite_, 5000);
        }
        char* error = nullptr;
        if (sqlite3_exec(sqlite_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(sqlite_);
            sqlite3_free(error);
            throw std::runtime_error("conformance SQLite statement failed: " + message);
        }
#endif
    }

    void postgres_execute([[maybe_unused]] const std::string& sql) {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        detail::postgres_execute(url_, sql);
#endif
    }

    StoreBackend backend_;
    StoreForm    form_ = StoreForm::Direct;
    std::shared_ptr<InMemoryProgramTransitionStore> memory_transitions_;
    std::shared_ptr<InMemoryProgramStore>           memory_catalog_;
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_JS_STORES
    std::unique_ptr<wasm::testing::ReferenceStores> reference_;
#endif
    std::optional<neograph::graph::test_support::UniqueTempDir> directory_;
    std::string database_;
    const char* url_ = nullptr;
    std::vector<std::string> armed_;
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
    sqlite3* sqlite_ = nullptr;
#endif
};

}  // namespace neograph::program::test_support
