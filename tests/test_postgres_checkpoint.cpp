// Integration tests for PostgresCheckpointStore.
//
// Gated on the env var NEOGRAPH_TEST_POSTGRES_URL. When unset every
// test below is skipped (GTEST_SKIP) so a developer who hasn't started
// a local PG instance still gets a green test suite. CI / local
// developers wanting full coverage start a Postgres and export:
//
//   NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test'
//
// Each test starts from a clean schema via drop_schema() so ordering
// between tests is irrelevant — a failure in one test never poisons
// the others.

#include <gtest/gtest.h>
#include <neograph/graph/postgres_checkpoint.h>
#include <libpq-fe.h>

#include <asio/co_spawn.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace neograph::graph;
using json = neograph::json;

namespace neograph::graph::test_access {

class PostgresCheckpointStoreTestAccess {
public:
    static std::unique_ptr<PostgresCheckpointStore> make_pool(size_t pool_size) {
        return std::unique_ptr<PostgresCheckpointStore>(
            new PostgresCheckpointStore(
                PostgresCheckpointStore::PoolOnlyTag{}, pool_size));
    }

    static size_t acquire(PostgresCheckpointStore& store) {
        return store.acquire_slot();
    }

    static asio::awaitable<size_t> acquire_async(
        PostgresCheckpointStore& store) {
        co_return co_await store.acquire_slot_async();
    }

    static void release(PostgresCheckpointStore& store, size_t idx) {
        store.release_slot(idx);
    }

    static size_t free_count(PostgresCheckpointStore& store) {
        std::lock_guard lock(store.pool_mutex_);
        return store.free_.size();
    }

    static size_t waiter_count(PostgresCheckpointStore& store) {
        std::lock_guard lock(store.pool_mutex_);
        return store.async_waiters_.size();
    }
};

} // namespace neograph::graph::test_access

namespace {

using PoolTestAccess = test_access::PostgresCheckpointStoreTestAccess;

struct TestPgConn {
    PGconn* raw = nullptr;
    ~TestPgConn() { if (raw) PQfinish(raw); }
};

struct TestPgResult {
    PGresult* raw = nullptr;
    ~TestPgResult() { if (raw) PQclear(raw); }
};

std::optional<int> wait_for_blocked_checkpoint_lock(PGconn* observer) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        TestPgResult result{PQexec(observer,
            "SELECT pid FROM pg_locks "
            "WHERE relation = 'neograph_checkpoints'::regclass "
            "  AND NOT granted LIMIT 1")};
        if (result.raw && PQresultStatus(result.raw) == PGRES_TUPLES_OK
            && PQntuples(result.raw) == 1) {
            return std::stoi(PQgetvalue(result.raw, 0, 0));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

bool wait_for_backend_exit(PGconn* observer, int pid) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string sql = "SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = "
                    + std::to_string(pid) + ")";
    while (std::chrono::steady_clock::now() < deadline) {
        TestPgResult result{PQexec(observer, sql.c_str())};
        if (result.raw && PQresultStatus(result.raw) == PGRES_TUPLES_OK
            && PQntuples(result.raw) == 1
            && std::string(PQgetvalue(result.raw, 0, 0)) == "t") {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

const char* pg_url() {
    const char* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    return (url && *url) ? url : nullptr;
}

// Build a Checkpoint whose channel_values matches GraphState::serialize().
Checkpoint make_state_cp(const std::string& thread_id,
                         int step,
                         const std::map<std::string, std::pair<json, uint64_t>>& channels,
                         CheckpointPhase phase = CheckpointPhase::Completed) {
    Checkpoint cp;
    cp.id = Checkpoint::generate_id();
    cp.thread_id = thread_id;
    cp.step = step;
    cp.timestamp = step * 1000 + 1;  // monotone for deterministic ordering
    cp.next_nodes = {"__end__"};
    cp.interrupt_phase = phase;
    cp.current_node = "test_node";

    json cv = json::object();
    json chs = json::object();
    for (const auto& [name, vv] : channels) {
        json entry = json::object();
        entry["value"] = vv.first;
        entry["version"] = vv.second;
        chs[name] = entry;
    }
    cv["channels"] = chs;
    cv["global_version"] = static_cast<uint64_t>(channels.size());
    cp.channel_values = cv;
    return cp;
}

class PostgresCheckpointTest : public ::testing::Test {
protected:
    std::unique_ptr<PostgresCheckpointStore> store;

    void SetUp() override {
        const char* url = pg_url();
        if (!url) {
            GTEST_SKIP() << "NEOGRAPH_TEST_POSTGRES_URL not set; "
                         << "skipping PostgresCheckpointStore integration tests.";
        }
        store = std::make_unique<PostgresCheckpointStore>(url);
        // Each test starts on a freshly recreated schema.
        store->drop_schema();
    }

    void TearDown() override {
        if (store) store->drop_schema();
    }
};

} // namespace

TEST(PostgresCheckpointPoolTest, AsyncWaitDoesNotBlockIoContext) {
    auto pool = PoolTestAccess::make_pool(/*pool_size=*/1);
    const size_t occupied = PoolTestAccess::acquire(*pool);

    asio::io_context io;
    std::optional<std::chrono::milliseconds> heartbeat_elapsed;
    std::exception_ptr waiter_error;
    bool waiter_done = false;
    auto started = std::chrono::steady_clock::now();

    asio::steady_timer heartbeat(io, std::chrono::milliseconds(20));
    heartbeat.async_wait([&](const asio::error_code& ec) {
        if (!ec) {
            heartbeat_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        }
    });
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            size_t idx = co_await PoolTestAccess::acquire_async(*pool);
            PoolTestAccess::release(*pool, idx);
        },
        [&](std::exception_ptr error) {
            waiter_error = error;
            waiter_done = true;
        });

    // A separate thread bounds the pre-fix blocking acquire. The heartbeat
    // must run well before this releases the only slot.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        PoolTestAccess::release(*pool, occupied);
    });

    io.run();
    releaser.join();

    EXPECT_EQ(waiter_error, nullptr);
    EXPECT_TRUE(waiter_done);
    ASSERT_TRUE(heartbeat_elapsed.has_value());
    EXPECT_LT(*heartbeat_elapsed, std::chrono::milliseconds(150))
        << "an async pool waiter blocked the io_context heartbeat for "
        << heartbeat_elapsed->count() << "ms";
    EXPECT_EQ(PoolTestAccess::free_count(*pool), 1u);
}

TEST(PostgresCheckpointPoolTest, CancelledWaiterIsRemoved) {
    auto pool = PoolTestAccess::make_pool(/*pool_size=*/1);
    const size_t occupied = PoolTestAccess::acquire(*pool);

    asio::io_context io;
    asio::cancellation_signal cancel;
    std::exception_ptr waiter_error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            (void)co_await PoolTestAccess::acquire_async(*pool);
        },
        asio::bind_cancellation_slot(cancel.slot(),
            [&](std::exception_ptr error) { waiter_error = error; }));
    asio::steady_timer cancel_timer(io, std::chrono::milliseconds(20));
    cancel_timer.async_wait([&](const asio::error_code& ec) {
        if (!ec) cancel.emit(asio::cancellation_type::all);
    });

    io.run();

    ASSERT_NE(waiter_error, nullptr);
    try {
        std::rethrow_exception(waiter_error);
    } catch (const asio::system_error& error) {
        EXPECT_EQ(error.code(), asio::error::operation_aborted);
    } catch (...) {
        FAIL() << "cancelled pool waiter returned an unexpected exception";
    }
    EXPECT_EQ(PoolTestAccess::waiter_count(*pool), 0u);

    PoolTestAccess::release(*pool, occupied);
    EXPECT_EQ(PoolTestAccess::free_count(*pool), 1u)
        << "a cancelled waiter retained or lost the released pool slot";
}

TEST_F(PostgresCheckpointTest, SaveAndLoadLatestRoundTrip) {
    auto cp = make_state_cp("t", 0, {{"x", {42, 1}}, {"msg", {"hi", 2}}});
    store->save(cp);

    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp.id);
    EXPECT_EQ(loaded->thread_id, "t");
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 42);
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["version"].get<uint64_t>(), 1u);
    EXPECT_EQ(loaded->channel_values["channels"]["msg"]["value"].get<std::string>(), "hi");
}

TEST_F(PostgresCheckpointTest, LoadByIdReturnsCheckpoint) {
    auto cp = make_state_cp("t", 0, {{"x", {99, 5}}});
    store->save(cp);

    auto loaded = store->load_by_id(cp.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 99);
}

TEST_F(PostgresCheckpointTest, LoadLatestReturnsNewestByTimestamp) {
    store->save(make_state_cp("t", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("t", 1, {{"x", {2, 2}}}));
    auto cp3 = make_state_cp("t", 2, {{"x", {3, 3}}});
    store->save(cp3);

    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp3.id);
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 3);
}

// The headline feature: blobs are deduplicated across cps. Three saves
// where only one channel changes per step → 3 distinct counter blobs +
// 1 shared config blob = 4 total, not 6.
TEST_F(PostgresCheckpointTest, BlobsDedupedAcrossSteps) {
    json config = json::object();
    config["model"] = "claude";

    store->save(make_state_cp("t", 0, {{"counter", {1, 1}}, {"config", {config, 2}}}));
    store->save(make_state_cp("t", 1, {{"counter", {2, 3}}, {"config", {config, 2}}}));
    store->save(make_state_cp("t", 2, {{"counter", {3, 4}}, {"config", {config, 2}}}));

    EXPECT_EQ(store->blob_count(), 4u)
        << "expected 3 distinct counter blobs + 1 shared config blob";
}

TEST_F(PostgresCheckpointTest, IdenticalSavesShareBlobs) {
    auto cp1 = make_state_cp("t", 0, {{"x", {7, 1}}});
    auto cp2 = make_state_cp("t", 1, {{"x", {7, 1}}});  // same value+version
    store->save(cp1);
    store->save(cp2);
    EXPECT_EQ(store->blob_count(), 1u);
}

TEST_F(PostgresCheckpointTest, ListReturnsNewestFirst) {
    store->save(make_state_cp("t", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("t", 1, {{"x", {2, 2}}}));
    store->save(make_state_cp("t", 2, {{"x", {3, 3}}}));

    auto cps = store->list("t");
    ASSERT_EQ(cps.size(), 3u);
    EXPECT_EQ(cps[0].channel_values["channels"]["x"]["value"].get<int>(), 3);
    EXPECT_EQ(cps[2].channel_values["channels"]["x"]["value"].get<int>(), 1);
}

TEST_F(PostgresCheckpointTest, ListRespectsLimit) {
    store->save(make_state_cp("t", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("t", 1, {{"x", {2, 2}}}));
    store->save(make_state_cp("t", 2, {{"x", {3, 3}}}));

    auto cps = store->list("t", 2);
    EXPECT_EQ(cps.size(), 2u);
}

TEST_F(PostgresCheckpointTest, ListIsolatedByThread) {
    store->save(make_state_cp("ta", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("tb", 0, {{"x", {2, 1}}}));
    EXPECT_EQ(store->list("ta").size(), 1u);
    EXPECT_EQ(store->list("tb").size(), 1u);
}

TEST_F(PostgresCheckpointTest, DeleteThreadDropsCheckpointsAndBlobs) {
    store->save(make_state_cp("ta", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("tb", 0, {{"y", {2, 1}}}));
    EXPECT_EQ(store->blob_count(), 2u);

    store->delete_thread("ta");

    EXPECT_FALSE(store->load_latest("ta").has_value());
    EXPECT_TRUE(store->load_latest("tb").has_value());
    EXPECT_EQ(store->blob_count(), 1u);
}

// barrier_state must round-trip — admin update_state during an in-flight
// barrier accumulates upstream signals, and dropping them on save would
// silently corrupt the AND-join.
TEST_F(PostgresCheckpointTest, BarrierStateRoundTrips) {
    auto cp = make_state_cp("t", 0, {{"x", {1, 1}}});
    cp.barrier_state["join_node"] = {"upstream_a", "upstream_b"};
    cp.barrier_state["another"] = {"only_one"};
    store->save(cp);

    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->barrier_state.size(), 2u);
    EXPECT_EQ(loaded->barrier_state["join_node"].size(), 2u);
    EXPECT_EQ(loaded->barrier_state["join_node"].count("upstream_a"), 1u);
    EXPECT_EQ(loaded->barrier_state["another"].count("only_one"), 1u);
}

// All five CheckpointPhase values must survive the to_string/parse round
// trip through PG. Guards against an enum value being added without
// updating the parser.
TEST_F(PostgresCheckpointTest, AllPhasesRoundTrip) {
    int step = 0;
    for (auto p : {CheckpointPhase::Before, CheckpointPhase::After,
                   CheckpointPhase::Completed, CheckpointPhase::NodeInterrupt,
                   CheckpointPhase::Updated}) {
        auto cp = make_state_cp("t", step++, {{"x", {step, 1}}}, p);
        store->save(cp);
        auto loaded = store->load_by_id(cp.id);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->interrupt_phase, p)
            << "phase " << to_string(p) << " did not round-trip";
    }
}

TEST_F(PostgresCheckpointTest, NextNodesRoundTrip) {
    auto cp = make_state_cp("t", 0, {{"x", {1, 1}}});
    cp.next_nodes = {"a", "b", "c"};
    store->save(cp);
    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->next_nodes.size(), 3u);
    EXPECT_EQ(loaded->next_nodes[0], "a");
    EXPECT_EQ(loaded->next_nodes[2], "c");
}

// ── Pending writes ────────────────────────────────────────────────────

TEST_F(PostgresCheckpointTest, PutGetClearWritesRoundTrip) {
    PendingWrite pw;
    pw.task_id = "task-1";
    pw.task_path = "s0:executor_1";
    pw.node_name = "executor";
    json writes = json::array();
    json w = json::object();
    w["channel"] = "messages";
    w["value"] = "hello";
    writes.push_back(w);
    pw.writes = writes;
    pw.command = json();  // null
    pw.sends = json::array();
    pw.step = 5;
    pw.timestamp = 12345;

    store->put_writes("t", "parent-cp", pw);

    auto loaded = store->get_writes("t", "parent-cp");
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].task_id, "task-1");
    EXPECT_EQ(loaded[0].task_path, "s0:executor_1");
    EXPECT_EQ(loaded[0].node_name, "executor");
    EXPECT_EQ(loaded[0].step, 5);
    EXPECT_EQ(loaded[0].writes[0]["channel"].get<std::string>(), "messages");

    store->clear_writes("t", "parent-cp");
    EXPECT_EQ(store->get_writes("t", "parent-cp").size(), 0u);
}

// Pending writes must come back in insertion order. Engine replay
// applies them in order, so a swap would change semantics for any node
// whose writes aren't commutative.
TEST_F(PostgresCheckpointTest, PendingWritesPreserveOrder) {
    for (int i = 0; i < 5; ++i) {
        PendingWrite pw;
        pw.task_id = "task-" + std::to_string(i);
        pw.task_path = "p" + std::to_string(i);
        pw.node_name = "n";
        pw.writes = json::array();
        pw.command = json();
        pw.sends = json::array();
        pw.step = 0;
        pw.timestamp = i;
        store->put_writes("t", "parent", pw);
    }
    auto loaded = store->get_writes("t", "parent");
    ASSERT_EQ(loaded.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(loaded[i].task_id, "task-" + std::to_string(i));
    }
}

// delete_thread must wipe pending writes too — otherwise re-running a
// previously deleted thread would replay stale writes.
TEST_F(PostgresCheckpointTest, DeleteThreadClearsPendingWrites) {
    PendingWrite pw;
    pw.task_id = "t1";
    pw.task_path = "p";
    pw.node_name = "n";
    pw.writes = json::array();
    pw.command = json();
    pw.sends = json::array();
    pw.step = 0;
    pw.timestamp = 0;
    store->put_writes("ta", "parent", pw);
    store->put_writes("tb", "parent", pw);

    store->delete_thread("ta");
    EXPECT_EQ(store->get_writes("ta", "parent").size(), 0u);
    EXPECT_EQ(store->get_writes("tb", "parent").size(), 1u);
}

// Saving the same checkpoint id twice with mutated fields updates in
// place rather than failing the PK constraint. This supports patterns
// like "save shell first, then enrich" used by some recovery flows.
TEST_F(PostgresCheckpointTest, ResaveSameIdUpdatesInPlace) {
    auto cp = make_state_cp("t", 0, {{"x", {1, 1}}});
    store->save(cp);

    cp.next_nodes = {"new_target"};
    store->save(cp);

    auto loaded = store->load_by_id(cp.id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->next_nodes.size(), 1u);
    EXPECT_EQ(loaded->next_nodes[0], "new_target");
}

// load_latest on a fresh thread (no saves) returns nullopt rather
// than throwing — engine code branches on this for the "fresh run vs
// resume" decision.
TEST_F(PostgresCheckpointTest, LoadLatestEmptyReturnsNullopt) {
    EXPECT_FALSE(store->load_latest("never-saved").has_value());
}

TEST_F(PostgresCheckpointTest, LoadByIdMissingReturnsNullopt) {
    EXPECT_FALSE(store->load_by_id("nonexistent-uuid").has_value());
}

// Nested JSON values (arrays of objects) must round-trip exactly. This
// is the "messages" channel shape — the most common real payload.
// Reconnect-after-broken-connection path. Forces PG to drop every
// pool slot's backend via a sibling connection issuing
// pg_terminate_backend; subsequent operations must detect the dead
// socket (PQstatus != CONNECTION_OK, or SQLSTATE 08xxx on the next
// exec) and replace the slot.
//
// With pool_size=8 and a single load_latest call the store will only
// touch ONE slot, so reconnect_count goes 0 → 1. The test pins
// pool_size=1 to make the assertion deterministic regardless of
// which slot acquire_slot picks.
TEST_F(PostgresCheckpointTest, RetriesAfterBrokenConnection) {
    // Re-construct the store with pool_size=1 so the assertion below
    // doesn't depend on which slot the next acquire_slot returned.
    store = std::make_unique<PostgresCheckpointStore>(pg_url(), /*pool_size=*/1);
    store->drop_schema();

    store->save(make_state_cp("rt", 0, {{"x", {1, 1}}}));
    EXPECT_EQ(store->reconnect_count(), 0u);

    // Kill every backend on neograph_test EXCEPT our killer's own —
    // that includes the store's pool slot. pg_terminate_backend is
    // available to any superuser; the test container's `postgres` user
    // qualifies.
    {
        PGconn* killer = PQconnectdb(pg_url());
        ASSERT_EQ(PQstatus(killer), CONNECTION_OK) << PQerrorMessage(killer);
        PGresult* r = PQexec(killer,
            "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
            "WHERE datname = current_database() "
            "  AND pid <> pg_backend_pid()");
        PQclear(r);
        PQfinish(killer);
    }

    auto loaded = store->load_latest("rt");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 1);
    EXPECT_EQ(store->reconnect_count(), 1u)
        << "with_conn must detect the dead connection and replace the slot";
}

// Pool sizing: constructor honours the requested pool_size and the
// invalid argument 0 is rejected. The benchmark relies on >1 slots to
// scale, so a regression here would silently re-serialize commits.
TEST_F(PostgresCheckpointTest, PoolSizeIsHonoured) {
    auto store4 = std::make_unique<PostgresCheckpointStore>(pg_url(), 4);
    EXPECT_EQ(store4->pool_size(), 4u);

    auto store1 = std::make_unique<PostgresCheckpointStore>(pg_url(), 1);
    EXPECT_EQ(store1->pool_size(), 1u);

    EXPECT_THROW(PostgresCheckpointStore(pg_url(), 0),
                 std::invalid_argument);
}

// Concurrent saves across pool slots must all land — proves the pool
// actually serves multiple acquire_slot callers in parallel and the
// per-slot transactions don't trample each other.
TEST_F(PostgresCheckpointTest, PoolHandlesConcurrentSaves) {
    auto pooled = std::make_unique<PostgresCheckpointStore>(pg_url(), 4);
    pooled->drop_schema();

    constexpr int N = 32;
    std::vector<std::thread> ts;
    ts.reserve(N);
    for (int i = 0; i < N; ++i) {
        ts.emplace_back([&, i] {
            pooled->save(make_state_cp("p", i, {{"x", {i, uint64_t(i + 1)}}}));
        });
    }
    for (auto& t : ts) t.join();

    auto cps = pooled->list("p", 100);
    EXPECT_EQ(cps.size(), static_cast<size_t>(N));
}

TEST_F(PostgresCheckpointTest, NestedJsonRoundTrips) {
    json msgs = json::array();
    json m1 = json::object();
    m1["role"] = "user";
    m1["content"] = "안녕하세요";
    msgs.push_back(m1);
    json m2 = json::object();
    m2["role"] = "assistant";
    m2["content"] = "Hi!";
    msgs.push_back(m2);

    auto cp = make_state_cp("t", 0, {{"messages", {msgs, 1}}});
    store->save(cp);
    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    auto loaded_msgs = loaded->channel_values["channels"]["messages"]["value"];
    ASSERT_EQ(loaded_msgs.size(), 2u);
    EXPECT_EQ(loaded_msgs[0]["content"].get<std::string>(), "안녕하세요");
    EXPECT_EQ(loaded_msgs[1]["role"].get<std::string>(), "assistant");
}

// ─────────────────────────────────────────────────────────────────────
// Async peers (Sem 4 follow-up)
//
// save_async / load_*_async / *_writes_async — true async, libpq
// nonblocking + asio::posix on PQsocket. Two properties we pin here:
//   1. Round-trip equivalence with the sync peers (same rows in,
//      same rows out via the async path).
//   2. The async path actually yields to the io_context — multiple
//      saves on one io_context make progress concurrently across
//      pool slots rather than serialising.
// ─────────────────────────────────────────────────────────────────────

TEST_F(PostgresCheckpointTest, AsyncSaveAndLoadLatestRoundTrip) {
    auto cp = make_state_cp("t-async", 3, {{"x", {42, 1}}, {"msg", {"hi", 2}}});

    asio::io_context io;
    std::optional<Checkpoint> loaded;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            co_await store->save_async(cp);
            loaded = co_await store->load_latest_async("t-async");
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp.id);
    EXPECT_EQ(loaded->step, 3);
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 42);
}

TEST_F(PostgresCheckpointTest, AsyncListReturnsNewestFirst) {
    for (int i = 0; i < 3; ++i) {
        auto cp = make_state_cp("t-list", i, {{"n", {i, static_cast<uint64_t>(i + 1)}}});
        cp.timestamp = 1000 + i * 10;  // strictly increasing
        store->save(cp);
    }

    asio::io_context io;
    std::vector<Checkpoint> listed;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            listed = co_await store->list_async("t-list", 100);
        },
        asio::detached);
    io.run();

    ASSERT_EQ(listed.size(), 3u);
    EXPECT_EQ(listed[0].step, 2);  // newest
    EXPECT_EQ(listed[2].step, 0);  // oldest
}

TEST_F(PostgresCheckpointTest, AsyncPendingWritesRoundTrip) {
    // Seed a parent cp first so put_writes_async's empty-parent guard
    // doesn't short-circuit.
    auto parent = make_state_cp("t-pw", 0, {{"x", {1, 1}}});
    store->save(parent);

    PendingWrite pw;
    pw.task_id   = "task-A";
    pw.task_path = "s1:n1";
    pw.node_name = "n1";
    pw.step      = 1;
    pw.timestamp = 123;

    asio::io_context io;
    std::vector<PendingWrite> loaded;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            co_await store->put_writes_async("t-pw", parent.id, pw);
            loaded = co_await store->get_writes_async("t-pw", parent.id);
            co_await store->clear_writes_async("t-pw", parent.id);
        },
        asio::detached);
    io.run();

    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].task_id, "task-A");

    // After clear, a subsequent sync get returns empty.
    EXPECT_TRUE(store->get_writes("t-pw", parent.id).empty());
}

TEST_F(PostgresCheckpointTest, AsyncSavesOverlapAcrossPoolSlots) {
    // 4 concurrent save_async on a pool of size 4. If the async path
    // were actually serialising (e.g. through a single connection or
    // a sync wait), wall-clock would be ~4 × single-save latency;
    // with real overlap across slots each save's commit fsync
    // happens concurrently and wall-clock stays close to one save's
    // latency. This is the whole point of the async migration.
    store = std::make_unique<PostgresCheckpointStore>(pg_url(), /*pool_size=*/4);
    store->drop_schema();

    std::vector<Checkpoint> cps;
    for (int i = 0; i < 4; ++i) {
        cps.push_back(make_state_cp("t-par-" + std::to_string(i), 0,
                                     {{"n", {i, 1}}}));
    }

    asio::io_context io;
    std::atomic<int> done{0};
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < 4; ++i) {
        asio::co_spawn(io,
            [&, i]() -> asio::awaitable<void> {
                co_await store->save_async(cps[i]);
                done.fetch_add(1, std::memory_order_relaxed);
            },
            asio::detached);
    }
    io.run();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(done.load(), 4);
    // Rough overlap check — serial with commit fsync ~10ms would be
    // 40ms+; true overlap lands closer to 20ms or under (single-
    // commit + scheduling overhead). Use a generous upper bound to
    // tolerate slow CI disks while still catching full serialization.
    EXPECT_LT(elapsed_ms, 200)
        << "4 concurrent async saves took " << elapsed_ms
        << "ms — likely serialising instead of overlapping";
}

TEST_F(PostgresCheckpointTest, SqlErrorsLeavePoolConnectionReusable) {
    for (int i = 0; i < 3; ++i) {
        EXPECT_THROW((void)store->list("sql-error", -1), std::runtime_error);
    }

    asio::io_context io;
    std::exception_ptr async_error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            (void)co_await store->list_async("sql-error", -1);
        },
        [&](std::exception_ptr error) { async_error = error; });
    io.run();

    EXPECT_NE(async_error, nullptr);
    EXPECT_NO_THROW((void)store->load_latest("after-sql-error"));
}

TEST_F(PostgresCheckpointTest, CancelledAsyncQueryDiscardsConnectionWithoutRetry) {
    store = std::make_unique<PostgresCheckpointStore>(pg_url(), /*pool_size=*/1);
    store->drop_schema();
    auto cp = make_state_cp("cancel-write", 0, {{"x", {1, 1}}});

    TestPgConn locker{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(locker.raw), CONNECTION_OK) << PQerrorMessage(locker.raw);
    {
        TestPgResult result{PQexec(locker.raw,
            "DROP SEQUENCE IF EXISTS neograph_cancel_attempt_seq CASCADE;"
            "CREATE SEQUENCE neograph_cancel_attempt_seq;"
            "CREATE OR REPLACE FUNCTION neograph_count_cancel_attempt() "
            "RETURNS trigger LANGUAGE plpgsql AS $$"
            "BEGIN PERFORM nextval('neograph_cancel_attempt_seq'); RETURN NEW; END"
            "$$;"
            "CREATE TRIGGER neograph_count_cancel_attempt "
            "BEFORE INSERT OR UPDATE ON neograph_checkpoints "
            "FOR EACH ROW EXECUTE FUNCTION neograph_count_cancel_attempt()")};
        ASSERT_NE(result.raw, nullptr);
        ASSERT_EQ(PQresultStatus(result.raw), PGRES_COMMAND_OK)
            << PQresultErrorMessage(result.raw);
    }
    {
        TestPgResult result{PQexec(locker.raw,
            "BEGIN; LOCK TABLE neograph_checkpoints IN ACCESS EXCLUSIVE MODE")};
        ASSERT_NE(result.raw, nullptr);
        ASSERT_EQ(PQresultStatus(result.raw), PGRES_COMMAND_OK)
            << PQresultErrorMessage(result.raw);
    }
    TestPgConn observer{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(observer.raw), CONNECTION_OK) << PQerrorMessage(observer.raw);

    asio::io_context io;
    asio::cancellation_signal cancel;
    std::exception_ptr save_error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> { co_await store->save_async(cp); },
        asio::bind_cancellation_slot(cancel.slot(),
            [&](std::exception_ptr error) { save_error = error; }));
    std::thread io_thread([&] { io.run(); });

    auto blocked_pid = wait_for_blocked_checkpoint_lock(observer.raw);
    if (!blocked_pid) {
        TestPgResult result{PQexec(locker.raw, "ROLLBACK")};
        io_thread.join();
        FAIL() << "async save never reached the PostgreSQL table lock";
    }

    asio::post(io, [&] { cancel.emit(asio::cancellation_type::all); });
    if (!wait_for_backend_exit(observer.raw, *blocked_pid)) {
        TestPgResult result{PQexec(locker.raw, "ROLLBACK")};
        io_thread.join();
        FAIL() << "cancelled async save did not discard its PostgreSQL backend";
    }
    {
        TestPgResult result{PQexec(locker.raw, "COMMIT")};
    }
    io_thread.join();

    ASSERT_NE(save_error, nullptr);
    try {
        std::rethrow_exception(save_error);
    } catch (const asio::system_error& error) {
        EXPECT_EQ(error.code(), asio::error::operation_aborted);
    } catch (...) {
        FAIL() << "cancelled async query returned an unexpected exception";
    }
    EXPECT_EQ(store->reconnect_count(), 1u);

    int attempts = -1;
    {
        TestPgResult result{PQexec(observer.raw,
            "SELECT CASE WHEN is_called THEN last_value ELSE 0 END "
            "FROM neograph_cancel_attempt_seq")};
        ASSERT_NE(result.raw, nullptr);
        ASSERT_EQ(PQresultStatus(result.raw), PGRES_TUPLES_OK)
            << PQresultErrorMessage(result.raw);
        attempts = std::stoi(PQgetvalue(result.raw, 0, 0));
    }
    EXPECT_EQ(attempts, 0)
        << "a cancelled write reached PostgreSQL after its connection was discarded";
    EXPECT_FALSE(store->load_by_id(cp.id).has_value())
        << "a cancelled write was retried on the replacement connection";

    {
        TestPgResult result{PQexec(observer.raw,
            "DROP TRIGGER IF EXISTS neograph_count_cancel_attempt "
            "ON neograph_checkpoints;"
            "DROP FUNCTION IF EXISTS neograph_count_cancel_attempt();"
            "DROP SEQUENCE IF EXISTS neograph_cancel_attempt_seq")};
        ASSERT_NE(result.raw, nullptr);
        ASSERT_EQ(PQresultStatus(result.raw), PGRES_COMMAND_OK)
            << PQresultErrorMessage(result.raw);
    }
}
