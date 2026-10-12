// SQLite stores for the C++ Program API examples that must survive a restart (69).
//
// examples::program::Host keeps bundles, versions, run records and checkpoints in memory by
// default. A Host built on sqlite_stores(directory) keeps them in SQLite files instead, so a
// second process (or a rebuilt runtime) that opens the same directory sees the same runs.
// Link: neograph::program, neograph::program_sqlite and neograph::sqlite.
#pragma once

#include "program_support.h"

#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>

#include <filesystem>

namespace examples::program {

inline Stores sqlite_stores(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto program = (directory / "program.db").string();
    Stores stores;
    stores.program = std::make_shared<SQLiteProgramStore>(program);
    stores.transitions = std::make_shared<SQLiteProgramTransitionStore>(program);
    stores.checkpoints = std::make_shared<SqliteCheckpointStore>((directory / "checkpoints.db").string());
    return stores;
}

}  // namespace examples::program
