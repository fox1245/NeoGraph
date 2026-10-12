#pragma once

// The checkpoint stores an engine contract suite runs on. The in-memory store
// is the reference; SQLite runs on the real clock and on a coarse clock that
// gives every save one timestamp; PostgreSQL runs on the coarse clock when
// NEOGRAPH_TEST_POSTGRES_URL names a database. Those cases drop the checkpoint
// tables, so CMake runs them under the postgres_test_db resource lock.

#include "coarse_clock_store.h"

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/sqlite_checkpoint.h>
#ifdef NEOGRAPH_TESTS_HAVE_POSTGRES
#include <neograph/graph/postgres_checkpoint.h>
#endif

#include <cstdlib>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace neograph::graph::test_support {

enum class Backend { InMemory, Sqlite, SqliteCoarseClock, PostgresCoarseClock };

inline const char* backend_name(Backend backend) {
    switch (backend) {
    case Backend::InMemory: return "InMemory";
    case Backend::Sqlite: return "Sqlite";
    case Backend::SqliteCoarseClock: return "SqliteCoarseClock";
    case Backend::PostgresCoarseClock: return "PostgresCoarseClock";
    }
    return "Unknown";
}

// Names the parameter in test listings instead of its bytes.
inline void PrintTo(Backend backend, std::ostream* out) { *out << backend_name(backend); }

inline std::vector<Backend> backends() {
    std::vector<Backend> out{Backend::InMemory, Backend::Sqlite, Backend::SqliteCoarseClock};
#ifdef NEOGRAPH_TESTS_HAVE_POSTGRES
    out.push_back(Backend::PostgresCoarseClock);
#endif
    return out;
}

inline const char* postgres_test_url() {
    const char* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    return url && *url ? url : nullptr;
}

// Whether the backend can run here; a PostgreSQL case needs a database.
inline bool backend_available(Backend backend) {
    return backend != Backend::PostgresCoarseClock || postgres_test_url() != nullptr;
}

// Empties a database backend's checkpoint tables.
inline void reset_backend(Backend backend) {
#ifdef NEOGRAPH_TESTS_HAVE_POSTGRES
    if (backend == Backend::PostgresCoarseClock && postgres_test_url())
        PostgresCheckpointStore(postgres_test_url(), 1).drop_schema();
#else
    (void)backend;
#endif
}

// Opens the backend. SQLite opens `sqlite_file`; another call with the same
// file or database is a second store over the same data, as after a restart.
inline std::shared_ptr<CheckpointStore> open_backend(Backend backend,
                                                     const std::string& sqlite_file) {
    switch (backend) {
    case Backend::InMemory: return std::make_shared<InMemoryCheckpointStore>();
    case Backend::Sqlite: return std::make_shared<SqliteCheckpointStore>(sqlite_file);
    case Backend::SqliteCoarseClock:
        return std::make_shared<CoarseClockStore<SqliteCheckpointStore>>(sqlite_file);
    case Backend::PostgresCoarseClock:
#ifdef NEOGRAPH_TESTS_HAVE_POSTGRES
        return std::make_shared<CoarseClockStore<PostgresCheckpointStore>>(
            std::string(postgres_test_url()));
#else
        break;
#endif
    }
    return nullptr;
}

}  // namespace neograph::graph::test_support
