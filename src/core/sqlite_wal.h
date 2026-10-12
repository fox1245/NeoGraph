#pragma once

// Switches a SQLite database to write-ahead logging when a store opens it,
// safely while another connection, in this process or another, opens the same
// file at the same moment.

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace neograph::detail {

// On a database that is not yet in WAL mode, PRAGMA journal_mode=WAL reads the
// file and then takes the write lock to rewrite its header. When two
// connections switch the same file at once (two stores, or two processes,
// opening one new database), both hold the read lock, and the one that asks
// for the write lock second cannot wait for the first without deadlock: SQLite
// returns SQLITE_BUSY to it at once, without calling the busy handler
// (https://www.sqlite.org/c3ref/busy_handler.html), whatever the busy timeout.
// This retries the switch, backing off from 1 ms to 50 ms, until `budget` has
// elapsed, and returns the last SQLite result code; the connection's error
// message describes a failure. A database already in WAL mode needs no write
// lock, so the first attempt returns.
inline int sqlite_enable_wal(sqlite3* db, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    auto       pause    = std::chrono::milliseconds(1);
    while (true) {
        const int status = sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        if ((status & 0xff) != SQLITE_BUSY) return status;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return status;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(pause, deadline - now));
        pause = std::min(pause * 2, std::chrono::milliseconds(50));
    }
}

}  // namespace neograph::detail
