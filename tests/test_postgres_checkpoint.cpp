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

#include "fixtures/coarse_clock_store.h"

#include <gtest/gtest.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/postgres_checkpoint.h>
#include <neograph/graph/state.h>
#include <libpq-fe.h>

#include <asio/co_spawn.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace neograph::graph;
using json = neograph::json;

namespace neograph::graph::test_access {

class PostgresCheckpointStoreTestAccess {
public:
    using Layout = std::array<size_t, 6>;

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
        return store.waiter_count_for_test();
    }

    static void set_conn_str(PostgresCheckpointStore& store,
                             std::string conn_str) {
        store.conn_str_ = std::move(conn_str);
    }

    static asio::awaitable<void> rebuild_async(
        PostgresCheckpointStore& store, size_t idx) {
        co_await store.rebuild_slot_async(idx);
    }

    static bool slot_is_empty(PostgresCheckpointStore& store, size_t idx) {
        return !store.pool_[idx];
    }

    static asio::awaitable<bool> wait_socket_either(int fd) {
        co_return co_await PostgresCheckpointStore::wait_socket_either_for_test(fd);
    }

    static void set_async_connection_test_seams(int poll_delay_ms,
                                                int timeout_ms) {
        PostgresCheckpointStore::set_async_connection_test_seams(
            poll_delay_ms, timeout_ms);
    }

    static int async_connection_timeout_ms(const std::string& conn_str) {
        return PostgresCheckpointStore::async_connection_timeout_ms_for_test(
            conn_str);
    }

    static Layout layout(const PostgresCheckpointStore& store) {
        auto base = reinterpret_cast<uintptr_t>(&store);
        return {
            reinterpret_cast<uintptr_t>(&store.conn_str_) - base,
            reinterpret_cast<uintptr_t>(&store.pool_) - base,
            reinterpret_cast<uintptr_t>(&store.free_) - base,
            reinterpret_cast<uintptr_t>(&store.pool_mutex_) - base,
            reinterpret_cast<uintptr_t>(&store.pool_cv_) - base,
            reinterpret_cast<uintptr_t>(&store.reconnect_count_) - base,
        };
    }
};

} // namespace neograph::graph::test_access

namespace {

using PoolTestAccess = test_access::PostgresCheckpointStoreTestAccess;

struct OriginPostgresLayout : CheckpointStore {
    std::string conn_str;
    std::vector<std::unique_ptr<PgConn>> pool;
    std::queue<size_t> free;
    std::mutex pool_mutex;
    std::condition_variable pool_cv;
    std::atomic<size_t> reconnect_count{0};
};

PoolTestAccess::Layout origin_layout(const OriginPostgresLayout& store) {
    auto base = reinterpret_cast<uintptr_t>(&store);
    return {
        reinterpret_cast<uintptr_t>(&store.conn_str) - base,
        reinterpret_cast<uintptr_t>(&store.pool) - base,
        reinterpret_cast<uintptr_t>(&store.free) - base,
        reinterpret_cast<uintptr_t>(&store.pool_mutex) - base,
        reinterpret_cast<uintptr_t>(&store.pool_cv) - base,
        reinterpret_cast<uintptr_t>(&store.reconnect_count) - base,
    };
}

struct TestPgConn {
    PGconn* raw = nullptr;
    ~TestPgConn() { if (raw) PQfinish(raw); }
};

struct TestPgResult {
    PGresult* raw = nullptr;
    ~TestPgResult() { if (raw) PQclear(raw); }
};

struct StalledTcpServer {
    using AcceptCallback =
        std::function<void(const std::shared_ptr<asio::ip::tcp::socket>&,
                           const asio::error_code&)>;

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard{
        asio::make_work_guard(io)};
    asio::ip::tcp::acceptor acceptor{io};
    std::shared_ptr<asio::ip::tcp::socket> client;
    std::thread worker;
    unsigned short port = 0;

    explicit StalledTcpServer(AcceptCallback on_accept = {}) {
        acceptor.open(asio::ip::tcp::v4());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind({asio::ip::tcp::v4(), 0});
        acceptor.listen();
        port = acceptor.local_endpoint().port();
        auto pending = std::make_shared<asio::ip::tcp::socket>(io);
        acceptor.async_accept(*pending,
            [this, pending, on_accept = std::move(on_accept)](
                const asio::error_code& ec) mutable {
                if (!ec) client = pending;
                if (on_accept && ec != asio::error::operation_aborted) {
                    on_accept(pending, ec);
                }
            });
        worker = std::thread([this] { io.run(); });
    }

    void stop() {
        work_guard.reset();
        io.stop();
        if (worker.joinable()) worker.join();
        client.reset();
    }

    ~StalledTcpServer() {
        stop();
    }
};

struct AsyncConnectionTestSeamReset {
    ~AsyncConnectionTestSeamReset() {
        PoolTestAccess::set_async_connection_test_seams(0, -1);
    }
};

std::optional<int> wait_for_blocked_table_lock(PGconn* observer,
                                                const char* table) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string sql = "SELECT pid FROM pg_locks WHERE relation = '"
                    + std::string(table)
                    + "'::regclass AND NOT granted LIMIT 1";
    while (std::chrono::steady_clock::now() < deadline) {
        TestPgResult result{PQexec(observer, sql.c_str())};
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

// The value of one channel of a checkpoint built by make_state_cp(), or null when it has none.
json channel_val(const Checkpoint& cp, const std::string& name) {
    if (!cp.channel_values.contains("channels")) return json();
    const auto& channels = cp.channel_values.at("channels");
    if (!channels.contains(name)) return json();
    const auto& channel = channels.at(name);
    return channel.contains("value") ? channel.at("value") : json();
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

TEST(PostgresCheckpointAbiTest, MatchesOriginLayout) {
    static_assert(sizeof(PostgresCheckpointStore) == sizeof(OriginPostgresLayout));
    static_assert(alignof(PostgresCheckpointStore) == alignof(OriginPostgresLayout));

    auto store = PoolTestAccess::make_pool(/*pool_size=*/1);
    OriginPostgresLayout origin;
#if defined(__ANDROID__) && defined(__aarch64__) && INTPTR_MAX == INT64_MAX
    EXPECT_EQ(sizeof(*store), 200u);
#elif defined(__linux__) && defined(__x86_64__) && INTPTR_MAX == INT64_MAX
    EXPECT_EQ(sizeof(*store), 240u);
#elif defined(__linux__) && defined(__aarch64__) && INTPTR_MAX == INT64_MAX
    EXPECT_EQ(sizeof(*store), 248u);
#endif
    EXPECT_EQ(alignof(PostgresCheckpointStore), alignof(OriginPostgresLayout));
    EXPECT_EQ(PoolTestAccess::layout(*store), origin_layout(origin));
}

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

TEST(PostgresCheckpointPoolTest, MixedWaitersKeepArrivalOrder) {
    auto pool = PoolTestAccess::make_pool(/*pool_size=*/1);
    const size_t occupied = PoolTestAccess::acquire(*pool);
    auto wait_for_count = [&](size_t expected) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (PoolTestAccess::waiter_count(*pool) == expected) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };

    std::mutex order_mutex;
    std::vector<std::string> order;
    std::thread sync_waiter([&] {
        size_t idx = PoolTestAccess::acquire(*pool);
        {
            std::lock_guard lock(order_mutex);
            order.push_back("sync");
        }
        PoolTestAccess::release(*pool, idx);
    });
    if (!wait_for_count(1)) {
        PoolTestAccess::release(*pool, occupied);
        sync_waiter.join();
        FAIL() << "sync waiter did not enter the mixed FIFO queue";
    }

    asio::io_context io;
    std::exception_ptr async_error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            size_t idx = co_await PoolTestAccess::acquire_async(*pool);
            {
                std::lock_guard lock(order_mutex);
                order.push_back("async");
            }
            PoolTestAccess::release(*pool, idx);
        },
        [&](std::exception_ptr error) { async_error = error; });
    std::thread io_thread([&] { io.run(); });
    if (!wait_for_count(2)) {
        PoolTestAccess::release(*pool, occupied);
        sync_waiter.join();
        io_thread.join();
        FAIL() << "async waiter did not join behind the sync waiter";
    }

    PoolTestAccess::release(*pool, occupied);
    sync_waiter.join();
    io_thread.join();

    EXPECT_EQ(async_error, nullptr);
    EXPECT_EQ(order, (std::vector<std::string>{"sync", "async"}));
    EXPECT_EQ(PoolTestAccess::free_count(*pool), 1u);
}

TEST(PostgresCheckpointAsyncIoTest, ReconnectPollDoesNotBlockIoContext) {
    std::vector<std::string> events;
    std::exception_ptr rebuild_error;
    std::atomic<bool> timed_out{false};
    asio::cancellation_signal cancel;
    asio::io_context io;
    asio::steady_timer probe_timer(io);
    auto packet = std::make_shared<std::vector<unsigned char>>();

    auto fail_from_server = [&](const asio::error_code& ec) {
        if (ec == asio::error::operation_aborted) return;
        asio::post(io, [&] {
            events.push_back("server_error");
            cancel.emit(asio::cancellation_type::all);
        });
    };

    std::function<void(const std::shared_ptr<asio::ip::tcp::socket>&)> read_startup_packet;
    read_startup_packet = [&](const std::shared_ptr<asio::ip::tcp::socket>& sock) {
        packet->assign(4, 0);
        asio::async_read(*sock, asio::buffer(*packet),
            [&, sock](const asio::error_code& hdr_ec, std::size_t) {
                if (hdr_ec) {
                    fail_from_server(hdr_ec);
                    return;
                }
                const std::uint32_t len =
                    (static_cast<std::uint32_t>((*packet)[0]) << 24) |
                    (static_cast<std::uint32_t>((*packet)[1]) << 16) |
                    (static_cast<std::uint32_t>((*packet)[2]) << 8) |
                    static_cast<std::uint32_t>((*packet)[3]);
                if (len < 8 || len > 4096) {
                    fail_from_server(asio::error::invalid_argument);
                    return;
                }
                packet->assign(len - 4, 0);
                asio::async_read(*sock, asio::buffer(*packet),
                    [&, sock, len](const asio::error_code& body_ec, std::size_t) {
                        if (body_ec) {
                            fail_from_server(body_ec);
                            return;
                        }
                        if (len == 8) {
                            static constexpr char kDeclineEncryption = 'N';
                            asio::async_write(*sock, asio::buffer(&kDeclineEncryption, 1),
                                [&, sock](const asio::error_code& write_ec, std::size_t) {
                                    if (write_ec) {
                                        fail_from_server(write_ec);
                                        return;
                                    }
                                    read_startup_packet(sock);
                                });
                            return;
                        }
                        // poll_connection_off_executor polls its worker future on a 2ms
                        // steady_timer on `io`. Scheduling the probe on `io` after 10ms
                        // orders it after that 2ms timer in `io`'s timer heap so
                        // `open_conn_async` has entered `wait_pg_socket_until(raw, true, ...)`
                        // with an empty receive buffer before the server replies.
                        asio::post(io, [&, sock] {
                            probe_timer.expires_after(std::chrono::milliseconds(10));
                            probe_timer.async_wait([&, sock](const asio::error_code& timer_ec) {
                                if (timer_ec) return;
                                events.push_back("work_before_auth");
                                asio::post(sock->get_executor(), [&, sock] {
                                    static constexpr std::array<unsigned char, 9> kAuthCleartext = {
                                        'R', 0, 0, 0, 8, 0, 0, 0, 3
                                    };
                                    asio::async_write(*sock, asio::buffer(kAuthCleartext),
                                        [&, sock](const asio::error_code& auth_ec, std::size_t) {
                                            if (auth_ec) {
                                                fail_from_server(auth_ec);
                                                return;
                                            }
                                            packet->assign(5, 0);
                                            asio::async_read(*sock, asio::buffer(*packet),
                                                [&, sock](const asio::error_code& pw_hdr_ec, std::size_t) {
                                                    if (pw_hdr_ec || (*packet)[0] != 'p') {
                                                        fail_from_server(pw_hdr_ec
                                                            ? pw_hdr_ec
                                                            : asio::error::invalid_argument);
                                                        return;
                                                    }
                                                    const std::uint32_t pw_len =
                                                        (static_cast<std::uint32_t>((*packet)[1]) << 24) |
                                                        (static_cast<std::uint32_t>((*packet)[2]) << 16) |
                                                        (static_cast<std::uint32_t>((*packet)[3]) << 8) |
                                                        static_cast<std::uint32_t>((*packet)[4]);
                                                    if (pw_len < 5 || pw_len > 4096) {
                                                        fail_from_server(asio::error::invalid_argument);
                                                        return;
                                                    }
                                                    packet->assign(pw_len - 4, 0);
                                                    asio::async_read(*sock, asio::buffer(*packet),
                                                        [&](const asio::error_code& pw_body_ec, std::size_t) {
                                                            if (pw_body_ec) {
                                                                fail_from_server(pw_body_ec);
                                                                return;
                                                            }
                                                            asio::post(io, [&] {
                                                                probe_timer.expires_after(
                                                                    std::chrono::milliseconds(10));
                                                                probe_timer.async_wait(
                                                                    [&](const asio::error_code& cancel_timer_ec) {
                                                                        if (cancel_timer_ec) return;
                                                                        events.push_back("work_after_auth");
                                                                        cancel.emit(
                                                                            asio::cancellation_type::all);
                                                                    });
                                                            });
                                                        });
                                                });
                                        });
                                });
                            });
                        });
                    });
            });
    };

    StalledTcpServer server(
        [&](const std::shared_ptr<asio::ip::tcp::socket>& sock,
            const asio::error_code& accept_ec) {
            if (accept_ec) {
                fail_from_server(accept_ec);
                return;
            }
            read_startup_packet(sock);
        });
    auto hang_guard = std::make_shared<asio::steady_timer>(
        server.io, std::chrono::seconds(10));
    asio::post(server.io, [&, hang_guard] {
        hang_guard->async_wait([&, hang_guard](const asio::error_code& ec) {
            if (ec) return;
            timed_out.store(true, std::memory_order_relaxed);
            asio::error_code ignored;
            server.acceptor.close(ignored);
            if (server.client) {
                server.client->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
                server.client->close(ignored);
            }
            asio::post(io, [&] {
                cancel.emit(asio::cancellation_type::all);
                io.stop();
            });
            io.stop();
        });
    });

    auto store = PoolTestAccess::make_pool(/*pool_size=*/1);
    PoolTestAccess::set_conn_str(*store,
        "hostaddr=127.0.0.1 port=" + std::to_string(server.port)
        + " user=stall password=stall dbname=stall sslmode=disable");

    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            events.push_back("poll_started");
            co_await PoolTestAccess::rebuild_async(*store, 0);
        },
        asio::bind_cancellation_slot(cancel.slot(),
            [&, hang_guard](std::exception_ptr error) {
                events.push_back("poll_completed");
                rebuild_error = error;
                probe_timer.cancel();
                asio::post(server.io, [hang_guard] { hang_guard->cancel(); });
            }));

    io.run();
    server.stop();

    EXPECT_FALSE(timed_out.load(std::memory_order_relaxed));
    EXPECT_EQ(events,
        (std::vector<std::string>{
            "poll_started", "work_before_auth", "work_after_auth", "poll_completed"}));
    ASSERT_NE(rebuild_error, nullptr);
    try {
        std::rethrow_exception(rebuild_error);
    } catch (const asio::system_error& error) {
        EXPECT_EQ(error.code(), asio::error::operation_aborted);
    } catch (...) {
        FAIL() << "cancelled async reconnect returned an unexpected exception";
    }
    EXPECT_TRUE(PoolTestAccess::slot_is_empty(*store, 0));
}

TEST(PostgresCheckpointAsyncIoTest, ResolverPollDoesNotBlockIoContext) {
    AsyncConnectionTestSeamReset reset;
    PoolTestAccess::set_async_connection_test_seams(
        /*poll_delay_ms=*/300, /*timeout_ms=*/80);
    StalledTcpServer server;
    auto store = PoolTestAccess::make_pool(/*pool_size=*/1);
    PoolTestAccess::set_conn_str(*store,
        "hostaddr=127.0.0.1 port=" + std::to_string(server.port)
        + " user=stall dbname=stall sslmode=disable");

    asio::io_context io;
    std::optional<std::chrono::milliseconds> heartbeat_elapsed;
    std::exception_ptr rebuild_error;
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
            co_await PoolTestAccess::rebuild_async(*store, 0);
        },
        [&](std::exception_ptr error) { rebuild_error = error; });

    io.run();
    auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(heartbeat_elapsed.has_value());
    EXPECT_LT(*heartbeat_elapsed, std::chrono::milliseconds(80));
    EXPECT_LT(elapsed, std::chrono::milliseconds(200))
        << "deadline waited for the blocking resolver worker";
    EXPECT_NE(rebuild_error, nullptr);
    EXPECT_TRUE(PoolTestAccess::slot_is_empty(*store, 0));
}

TEST(PostgresCheckpointAsyncIoTest, TimeoutPolicyParsesDefaultAndMinimum) {
    EXPECT_EQ(PoolTestAccess::async_connection_timeout_ms(
        "host=localhost sslmode=disable"), 30000);
    EXPECT_EQ(PoolTestAccess::async_connection_timeout_ms(
        "host=localhost sslmode=disable connect_timeout=0"), 30000);
    EXPECT_EQ(PoolTestAccess::async_connection_timeout_ms(
        "host=localhost sslmode=disable connect_timeout=-1"), 30000);
    EXPECT_EQ(PoolTestAccess::async_connection_timeout_ms(
        "host=localhost sslmode=disable connect_timeout=1"), 2000);
    EXPECT_EQ(PoolTestAccess::async_connection_timeout_ms(
        "host=localhost sslmode=disable connect_timeout=60"), 60000);
    EXPECT_EQ(PoolTestAccess::async_connection_timeout_ms(
        "host=one,two sslmode=disable connect_timeout=3"), 3000)
        << "multi-host conninfo must not multiply the global budget";
}

TEST(PostgresCheckpointAsyncIoTest, ConnectPollUsesOneGlobalConninfoDeadline) {
    AsyncConnectionTestSeamReset reset;
    PoolTestAccess::set_async_connection_test_seams(
        /*poll_delay_ms=*/0, /*timeout_ms=*/-1);
    StalledTcpServer first_server;
    StalledTcpServer second_server;
    auto store = PoolTestAccess::make_pool(/*pool_size=*/1);
    PoolTestAccess::set_conn_str(*store,
        "hostaddr=127.0.0.1,127.0.0.1 port="
        + std::to_string(first_server.port) + ","
        + std::to_string(second_server.port)
        + " user=stall dbname=stall sslmode=disable connect_timeout=1");

    asio::io_context io;
    std::exception_ptr rebuild_error;
    auto started = std::chrono::steady_clock::now();
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            co_await PoolTestAccess::rebuild_async(*store, 0);
        },
        [&](std::exception_ptr error) { rebuild_error = error; });
    io.run();
    auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_NE(rebuild_error, nullptr);
    try {
        std::rethrow_exception(rebuild_error);
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(
            "connection timed out after 2000 ms while polling libpq"),
            std::string::npos);
    } catch (...) {
        FAIL() << "timed async reconnect returned an unexpected exception";
    }
    EXPECT_GE(elapsed, std::chrono::milliseconds(1800));
    EXPECT_LT(elapsed, std::chrono::milliseconds(3000));
    EXPECT_TRUE(PoolTestAccess::slot_is_empty(*store, 0));
}

TEST(PostgresCheckpointAsyncIoTest, FlushReadinessCanWinAndCancelsWriteWait) {
#ifdef _WIN32
    GTEST_SKIP() << "socketpair readiness probe is POSIX-only";
#else
    int sockets[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    struct SocketPairClose {
        int* sockets;
        ~SocketPairClose() {
            if (sockets[0] >= 0) ::close(sockets[0]);
            if (sockets[1] >= 0) ::close(sockets[1]);
        }
    } close_sockets{sockets};

    int flags = ::fcntl(sockets[0], F_GETFL, 0);
    ASSERT_NE(flags, -1);
    ASSERT_NE(::fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK), -1);
    std::array<char, 4096> fill{};
    for (;;) {
        auto sent = ::send(sockets[0], fill.data(), fill.size(), 0);
        if (sent >= 0) continue;
        ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK)
            << "failed to fill socket send buffer: errno=" << errno;
        break;
    }
    const char marker = 'R';
    ASSERT_EQ(::send(sockets[1], &marker, 1, 0), 1);

    asio::io_context io;
    std::optional<bool> read_won;
    std::exception_ptr wait_error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            read_won = co_await PoolTestAccess::wait_socket_either(sockets[0]);
        },
        [&](std::exception_ptr error) { wait_error = error; });
    io.run();

    EXPECT_EQ(wait_error, nullptr);
    ASSERT_TRUE(read_won.has_value());
    EXPECT_TRUE(*read_won);
    char received = 0;
    ASSERT_EQ(::recv(sockets[0], &received, 1, 0), 1);
    EXPECT_EQ(received, marker);
#endif
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

TEST_F(PostgresCheckpointTest, EphemeralResumeGuardSurvivesSaveAndLoad) {
    const ChannelLifecyclePolicy ephemeral{
        ChannelRetentionPolicy::Unbounded, 0, ChannelPersistencePolicy::Ephemeral};
    GraphState resumed;
    resumed.init_channel("scratch", ReducerType::OVERWRITE, nullptr, "initial", ephemeral);
    auto cp = make_state_cp("t", 0, {{"durable", {42, 1}}});
    cp.metadata = {{"_neograph_ephemeral_guard", {{"scratch", false}}}};
    store->save(cp);
    auto safe = store->load_latest("t");
    ASSERT_TRUE(safe);
    EXPECT_NO_THROW(resumed.restore_checkpoint(
        safe->channel_values, safe->metadata["_neograph_ephemeral_guard"]));

    cp = make_state_cp("t", 1, {{"durable", {43, 2}}});
    cp.metadata = {{"_neograph_ephemeral_guard", {{"scratch", true}}}};
    store->save(cp);
    auto unsafe = store->load_latest("t");
    ASSERT_TRUE(unsafe);
    EXPECT_THROW(resumed.restore_checkpoint(
        unsafe->channel_values, unsafe->metadata["_neograph_ephemeral_guard"]),
        std::runtime_error);
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
    m1["raw_diagnostic"] = json::parse(R"({"z":true,"duplicate":1,"duplicate":2,"a":null})");
    msgs.push_back(m1);
    json m2 = json::object();
    m2["role"] = "assistant";
    m2["content"] = "Hi!";
    msgs.push_back(m2);

    auto cp = make_state_cp("t", 0, {{"messages", {msgs, 1}}});
    cp.metadata = json::parse(R"({"z":"original metadata","duplicate":1,"duplicate":2,"a":null})");
    cp.channel_values["structural_residue"] = json::parse(R"({"z":7,"duplicate":3,"duplicate":4,"a":null})");
    store->save(cp);
    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    auto loaded_msgs = loaded->channel_values["channels"]["messages"]["value"];
    ASSERT_EQ(loaded_msgs.size(), 2u);
    EXPECT_EQ(loaded_msgs[0]["content"].get<std::string>(), "안녕하세요");
    EXPECT_EQ(loaded_msgs[1]["role"].get<std::string>(), "assistant");
    EXPECT_EQ(loaded_msgs.dump(), msgs.dump());
    EXPECT_EQ(loaded->metadata.dump(), cp.metadata.dump());
    EXPECT_EQ(loaded->channel_values.at("structural_residue").dump(),
              cp.channel_values.at("structural_residue").dump());
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
    std::optional<Checkpoint> loaded_by_id;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            co_await store->save_async(cp);
            loaded = co_await store->load_latest_async("t-async");
            loaded_by_id = co_await store->load_by_id_async(cp.id);
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp.id);
    EXPECT_EQ(loaded->step, 3);
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 42);
    ASSERT_TRUE(loaded_by_id.has_value());
    EXPECT_EQ(loaded_by_id->id, cp.id);
    EXPECT_EQ(loaded_by_id->channel_values["channels"]["msg"]["value"], "hi");
}

TEST_F(PostgresCheckpointTest, AsyncBlobLoadDoesNotBlockIoContext) {
    store = std::make_unique<PostgresCheckpointStore>(pg_url(), /*pool_size=*/1);
    store->drop_schema();
    auto cp = make_state_cp("async-blob-heartbeat", 0, {{"x", {42, 1}}});
    store->save(cp);

    TestPgConn locker{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(locker.raw), CONNECTION_OK) << PQerrorMessage(locker.raw);
    {
        TestPgResult result{PQexec(locker.raw,
            "BEGIN; LOCK TABLE neograph_checkpoint_blobs "
            "IN ACCESS EXCLUSIVE MODE")};
        ASSERT_NE(result.raw, nullptr);
        ASSERT_EQ(PQresultStatus(result.raw), PGRES_COMMAND_OK)
            << PQresultErrorMessage(result.raw);
    }
    TestPgConn observer{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(observer.raw), CONNECTION_OK) << PQerrorMessage(observer.raw);

    asio::io_context io;
    std::optional<Checkpoint> loaded;
    std::optional<std::chrono::milliseconds> heartbeat_elapsed;
    std::exception_ptr load_error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            loaded = co_await store->load_latest_async("async-blob-heartbeat");
        },
        [&](std::exception_ptr error) { load_error = error; });
    std::thread io_thread([&] { io.run(); });

    auto blocked_pid = wait_for_blocked_table_lock(
        observer.raw, "neograph_checkpoint_blobs");
    if (!blocked_pid) {
        TestPgResult result{PQexec(locker.raw, "ROLLBACK")};
        io_thread.join();
        FAIL() << "async blob query never reached the PostgreSQL table lock";
    }

    auto started = std::chrono::steady_clock::now();
    asio::steady_timer heartbeat(io);
    asio::post(io, [&] {
        heartbeat.expires_after(std::chrono::milliseconds(20));
        heartbeat.async_wait([&](const asio::error_code& ec) {
            if (!ec) {
                heartbeat_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
            }
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        TestPgResult result{PQexec(locker.raw, "COMMIT")};
    }
    io_thread.join();

    EXPECT_EQ(load_error, nullptr);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"], 42);
    ASSERT_TRUE(heartbeat_elapsed.has_value());
    EXPECT_LT(*heartbeat_elapsed, std::chrono::milliseconds(150))
        << "async blob materialization blocked the io_context for "
        << heartbeat_elapsed->count() << "ms";
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

TEST_F(PostgresCheckpointTest, ConcurrentAsyncPendingWritesUseSeparatePoolSlots) {
    constexpr int kWrites = 4;

    // A parallel graph records one completion per worker against the same
    // parent checkpoint. Exercise the real nonblocking transaction path on
    // distinct pool slots; a serial round-trip cannot expose ownership bugs
    // in the captured parameter vectors.
    store = std::make_unique<PostgresCheckpointStore>(pg_url(), /*pool_size=*/kWrites);
    store->drop_schema();
    auto parent = make_state_cp("t-pw-parallel", 0, {{"x", {1, 1}}});
    store->save(parent);

    std::vector<PendingWrite> writes(kWrites);
    for (int i = 0; i < kWrites; ++i) {
        writes[i].task_id = "task-" + std::to_string(i);
        writes[i].task_path = "s1:worker[" + std::to_string(i) + "]";
        writes[i].node_name = "worker";
        writes[i].writes = json::array({{{"channel", "results"}, {"value", i}}});
        writes[i].command = nullptr;
        writes[i].sends = json::array();
        writes[i].step = 1;
        writes[i].timestamp = 100 + i;
    }

    asio::io_context io;
    std::array<std::exception_ptr, kWrites> errors{};
    std::atomic<int> completed{0};
    for (int i = 0; i < kWrites; ++i) {
        asio::co_spawn(io,
            [&, i]() -> asio::awaitable<void> {
                co_await store->put_writes_async(
                    "t-pw-parallel", parent.id, writes[static_cast<size_t>(i)]);
            },
            [&, i](std::exception_ptr error) {
                errors[static_cast<size_t>(i)] = error;
                completed.fetch_add(1, std::memory_order_relaxed);
            });
    }
    io.run();

    EXPECT_EQ(completed.load(std::memory_order_relaxed), kWrites);
    for (const auto& error : errors) EXPECT_EQ(error, nullptr);

    auto loaded = store->get_writes("t-pw-parallel", parent.id);
    ASSERT_EQ(loaded.size(), static_cast<size_t>(kWrites));
    std::set<std::string> task_ids;
    for (const auto& write : loaded) task_ids.insert(write.task_id);
    EXPECT_EQ(task_ids, (std::set<std::string>{"task-0", "task-1", "task-2", "task-3"}));
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
    std::exception_ptr followup_error;
    std::exception_ptr coroutine_error;
    std::atomic<bool> followup_waiting{false};
    asio::steady_timer followup_timer(io);
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            co_await asio::this_coro::reset_cancellation_state(
                asio::enable_partial_cancellation());
            try {
                co_await store->save_async(cp);
            } catch (...) {
                save_error = std::current_exception();
            }
            if (save_error) {
                followup_timer.expires_after(std::chrono::seconds(30));
                followup_waiting.store(true, std::memory_order_release);
                try {
                    co_await followup_timer.async_wait(asio::use_awaitable);
                } catch (...) {
                    followup_error = std::current_exception();
                }
            }
        },
        asio::bind_cancellation_slot(cancel.slot(),
            [&](std::exception_ptr error) { coroutine_error = error; }));
    std::thread io_thread([&] { io.run(); });

    auto blocked_pid = wait_for_blocked_table_lock(
        observer.raw, "neograph_checkpoints");
    if (!blocked_pid) {
        TestPgResult result{PQexec(locker.raw, "ROLLBACK")};
        io_thread.join();
        FAIL() << "async save never reached the PostgreSQL table lock";
    }

    asio::post(io, [&] { cancel.emit(asio::cancellation_type::partial); });
    if (!wait_for_backend_exit(observer.raw, *blocked_pid)) {
        TestPgResult result{PQexec(locker.raw, "ROLLBACK")};
        asio::post(io, [&] { cancel.emit(asio::cancellation_type::all); });
        io_thread.join();
        FAIL() << "cancelled async save did not discard its PostgreSQL backend";
    }
    auto followup_deadline = std::chrono::steady_clock::now()
                           + std::chrono::seconds(2);
    while (!followup_waiting.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < followup_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!followup_waiting.load(std::memory_order_acquire)) {
        TestPgResult result{PQexec(locker.raw, "ROLLBACK")};
        asio::post(io, [&] { cancel.emit(asio::cancellation_type::all); });
        io_thread.join();
        FAIL() << "cancelled async save did not reach its continuation";
    }
    asio::post(io, [&] { cancel.emit(asio::cancellation_type::partial); });
    {
        TestPgResult result{PQexec(locker.raw, "COMMIT")};
    }
    io_thread.join();

    EXPECT_EQ(coroutine_error, nullptr);
    ASSERT_NE(save_error, nullptr);
    try {
        std::rethrow_exception(save_error);
    } catch (const asio::system_error& error) {
        EXPECT_EQ(error.code(), asio::error::operation_aborted);
    } catch (...) {
        FAIL() << "cancelled async query returned an unexpected exception";
    }
    ASSERT_NE(followup_error, nullptr)
        << "PostgreSQL cleanup permanently disabled caller cancellation";
    try {
        std::rethrow_exception(followup_error);
    } catch (const asio::system_error& error) {
        EXPECT_EQ(error.code(), asio::error::operation_aborted);
    } catch (...) {
        FAIL() << "follow-up wait returned an unexpected exception";
    }
    EXPECT_EQ(store->reconnect_count(), 0u)
        << "cancelled coroutine must discard without blocking to reconnect";

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
    asio::io_context recovery_io;
    std::optional<Checkpoint> recovered;
    std::exception_ptr recovery_error;
    asio::co_spawn(recovery_io,
        [&]() -> asio::awaitable<void> {
            recovered = co_await store->load_by_id_async(cp.id);
        },
        [&](std::exception_ptr error) { recovery_error = error; });
    recovery_io.run();
    EXPECT_EQ(recovery_error, nullptr);
    EXPECT_FALSE(recovered.has_value())
        << "a cancelled write was retried on the replacement connection";
    EXPECT_EQ(store->reconnect_count(), 1u);

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

TEST_F(PostgresCheckpointTest, CancelledAsyncPutWritesDiscardsTransactionWithoutRetry) {
    store = std::make_unique<PostgresCheckpointStore>(pg_url(), /*pool_size=*/1);
    store->drop_schema();

    PendingWrite write;
    write.task_id = "cancelled-task";
    write.task_path = "cancelled-path";
    write.node_name = "cancelled-node";
    write.writes = json::array();
    write.command = json();
    write.sends = json::array();

    TestPgConn locker{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(locker.raw), CONNECTION_OK) << PQerrorMessage(locker.raw);
    {
        TestPgResult result{PQexec(locker.raw,
            "BEGIN; LOCK TABLE neograph_checkpoint_writes "
            "IN ACCESS EXCLUSIVE MODE")};
        ASSERT_NE(result.raw, nullptr);
        ASSERT_EQ(PQresultStatus(result.raw), PGRES_COMMAND_OK)
            << PQresultErrorMessage(result.raw);
    }
    TestPgConn observer{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(observer.raw), CONNECTION_OK) << PQerrorMessage(observer.raw);

    asio::io_context io;
    asio::cancellation_signal cancel;
    std::exception_ptr write_error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            co_await store->put_writes_async(
                "cancel-writes", "cancel-parent", write);
        },
        asio::bind_cancellation_slot(cancel.slot(),
            [&](std::exception_ptr error) { write_error = error; }));
    std::thread io_thread([&] { io.run(); });

    auto blocked_pid = wait_for_blocked_table_lock(
        observer.raw, "neograph_checkpoint_writes");
    if (!blocked_pid) {
        TestPgResult result{PQexec(locker.raw, "ROLLBACK")};
        io_thread.join();
        FAIL() << "put_writes_async never reached its transactional SELECT";
    }

    asio::post(io, [&] { cancel.emit(asio::cancellation_type::all); });
    if (!wait_for_backend_exit(observer.raw, *blocked_pid)) {
        TestPgResult result{PQexec(locker.raw, "ROLLBACK")};
        io_thread.join();
        FAIL() << "cancelled put_writes_async did not discard its backend";
    }
    {
        TestPgResult result{PQexec(locker.raw, "COMMIT")};
    }
    io_thread.join();

    ASSERT_NE(write_error, nullptr);
    try {
        std::rethrow_exception(write_error);
    } catch (const asio::system_error& error) {
        EXPECT_EQ(error.code(), asio::error::operation_aborted);
    } catch (...) {
        FAIL() << "cancelled put_writes_async returned an unexpected exception";
    }
    EXPECT_EQ(store->reconnect_count(), 0u)
        << "cancelled coroutine must discard without blocking to reconnect";

    asio::io_context recovery_io;
    std::vector<PendingWrite> recovered;
    std::exception_ptr recovery_error;
    asio::co_spawn(recovery_io,
        [&]() -> asio::awaitable<void> {
            recovered = co_await store->get_writes_async(
                "cancel-writes", "cancel-parent");
        },
        [&](std::exception_ptr error) { recovery_error = error; });
    recovery_io.run();
    EXPECT_EQ(recovery_error, nullptr);
    EXPECT_TRUE(recovered.empty())
        << "cancelled put_writes_async retried on the replacement connection";
    EXPECT_EQ(store->reconnect_count(), 1u);
}

// ── Save order (#386, #396) ───────────────────────────────────────────
//
// A thread's latest checkpoint is the one saved last, as in the in-memory
// store, whatever its timestamp or step: every save below carries one
// timestamp, as when saves return inside the millisecond.

namespace {

using CoarsePostgresStore = test_support::CoarseClockStore<PostgresCheckpointStore>;

Checkpoint coarse_cp(const std::string& thread_id, int step, int value) {
    auto cp      = make_state_cp(thread_id, step, {{"x", {value, static_cast<uint64_t>(value)}}});
    cp.timestamp = CoarsePostgresStore::kTimestamp;
    return cp;
}

std::vector<std::string> ids_of(const std::vector<Checkpoint>& checkpoints) {
    std::vector<std::string> ids;
    for (const auto& checkpoint : checkpoints) ids.push_back(checkpoint.id);
    return ids;
}

std::vector<Checkpoint> list_async_now(PostgresCheckpointStore& store, const std::string& thread_id,
                                       int limit = 100) {
    asio::io_context        io;
    std::vector<Checkpoint> out;
    std::exception_ptr      error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> { out = co_await store.list_async(thread_id, limit); },
        [&](std::exception_ptr e) { error = e; });
    io.run();
    if (error) std::rethrow_exception(error);
    return out;
}

std::optional<Checkpoint> latest_async_now(PostgresCheckpointStore& store,
                                           const std::string& thread_id) {
    asio::io_context          io;
    std::optional<Checkpoint> out;
    std::exception_ptr        error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> { out = co_await store.load_latest_async(thread_id); },
        [&](std::exception_ptr e) { error = e; });
    io.run();
    if (error) std::rethrow_exception(error);
    return out;
}

void save_async_now(PostgresCheckpointStore& store, const Checkpoint& cp) {
    asio::io_context   io;
    std::exception_ptr error;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> { co_await store.save_async(cp); },
                   [&](std::exception_ptr e) { error = e; });
    io.run();
    if (error) std::rethrow_exception(error);
}

std::string exec_ok(PGconn* connection, const std::string& sql) {
    TestPgResult result{PQexec(connection, sql.c_str())};
    if (!result.raw) return "no result";
    const auto status = PQresultStatus(result.raw);
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) return {};
    return PQresultErrorMessage(result.raw);
}

std::string query_text(PGconn* connection, const std::string& sql) {
    TestPgResult result{PQexec(connection, sql.c_str())};
    if (!result.raw || PQresultStatus(result.raw) != PGRES_TUPLES_OK || PQntuples(result.raw) != 1)
        return "<no row>";
    return PQgetisnull(result.raw, 0, 0) ? "<null>" : PQgetvalue(result.raw, 0, 0);
}

}  // namespace

TEST_F(PostgresCheckpointTest, CoarseClockStepZeroSaveAfterAHigherStepIsTheLatest) {
    CoarsePostgresStore coarse(pg_url());
    const auto high = coarse_cp("t", 5, 1);
    const auto low  = coarse_cp("t", 0, 2);
    coarse.save(high);
    coarse.save(low);
    ASSERT_TRUE(coarse.load_latest("t"));
    EXPECT_EQ(coarse.load_latest("t")->id, low.id);
    EXPECT_EQ(ids_of(coarse.list("t")), (std::vector<std::string>{low.id, high.id}));
    const auto latest = latest_async_now(coarse, "t");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, low.id);
    EXPECT_EQ(ids_of(list_async_now(coarse, "t")), (std::vector<std::string>{low.id, high.id}));
}

TEST_F(PostgresCheckpointTest, CoarseClockEqualStepSavesKeepTheirSaveOrder) {
    // Two interrupt_before gates of one step each save a Before checkpoint of
    // that step (#396), here in one millisecond, through both save paths.
    CoarsePostgresStore      coarse(pg_url());
    std::vector<std::string> saved;
    for (int gate = 0; gate < 4; ++gate) {
        auto cp            = coarse_cp("t", 3, gate + 1);
        cp.interrupt_phase = CheckpointPhase::Before;
        if (gate % 2) save_async_now(coarse, cp);
        else coarse.save(cp);
        saved.insert(saved.begin(), cp.id);
        ASSERT_TRUE(coarse.load_latest("t"));
        EXPECT_EQ(coarse.load_latest("t")->id, cp.id) << "after gate " << gate;
    }
    EXPECT_EQ(ids_of(coarse.list("t")), saved);
    EXPECT_EQ(ids_of(coarse.list("t", 2)), (std::vector<std::string>{saved[0], saved[1]}));
    EXPECT_EQ(ids_of(list_async_now(coarse, "t", 2)),
              (std::vector<std::string>{saved[0], saved[1]}));
}

TEST_F(PostgresCheckpointTest, CoarseClockResavedIdBecomesTheLatestOnce) {
    // As in the in-memory store: a save of an ID the thread holds replaces it
    // and makes it the latest; the history lists it once, in its new place.
    CoarsePostgresStore coarse(pg_url());
    auto       first  = coarse_cp("t", 0, 1);
    const auto second = coarse_cp("t", 1, 2);
    coarse.save(first);
    coarse.save(second);
    first.next_nodes = {"edited"};
    save_async_now(coarse, first);
    const auto latest = coarse.load_latest("t");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, first.id);
    EXPECT_EQ(latest->next_nodes, std::vector<std::string>{"edited"});
    EXPECT_EQ(ids_of(coarse.list("t")), (std::vector<std::string>{first.id, second.id}));

    InMemoryCheckpointStore reference;
    auto reference_first       = first;
    reference_first.next_nodes = {"__end__"};
    reference.save(reference_first);
    reference.save(second);
    reference.save(first);
    EXPECT_EQ(ids_of(reference.list("t")), ids_of(coarse.list("t")));
}

namespace {

std::atomic<int> pg_coarse_gate_resumed{0};

class PgCoarseGateNode final : public GraphNode {
public:
    asio::awaitable<NodeResult> run(NodeInput in) override {
        NodeResult out;
        if (in.ctx.resume_value) {
            ++pg_coarse_gate_resumed;
            out.writes.push_back(ChannelWrite{"value", *in.ctx.resume_value});
            co_return out;
        }
        if (in.state.get("value") == json("ask")) throw NodeInterrupt("approval");
        out.writes.push_back(ChannelWrite{"value", json("passed")});
        co_return out;
    }
    std::string get_name() const override { return "gate"; }
};

class PgCoarseTailNode final : public GraphNode {
public:
    asio::awaitable<NodeResult> run(NodeInput) override {
        NodeResult out;
        out.writes.push_back(ChannelWrite{"tail", json(true)});
        co_return out;
    }
    std::string get_name() const override { return "tail"; }
};

}  // namespace

TEST_F(PostgresCheckpointTest, CoarseClockALaterRunAtStepZeroIsTheLatestAndResumes) {
    NodeFactory::instance().register_type(
        "pg_coarse_gate", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PgCoarseGateNode>();
        });
    NodeFactory::instance().register_type(
        "pg_coarse_tail", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PgCoarseTailNode>();
        });
    const json definition{
        {"name", "pg_coarse_clock"},
        {"channels", {{"value", {{"reducer", "overwrite"}}}, {"tail", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"gate", {{"type", "pg_coarse_gate"}}}, {"tail", {{"type", "pg_coarse_tail"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "gate"}},
                               json{{"from", "gate"}, {"to", "tail"}},
                               json{{"from", "tail"}, {"to", "__end__"}}})}};
    pg_coarse_gate_resumed.store(0);
    auto coarse = std::make_shared<CoarsePostgresStore>(pg_url());
    auto engine = GraphEngine::compile(definition, NodeContext{}, coarse);

    RunConfig first;
    first.thread_id = "coarse-clock";
    first.input     = json{{"value", "go"}};
    const auto finished = engine->run(first);
    ASSERT_FALSE(finished.interrupted);
    const auto finished_cp = coarse->load_by_id(finished.checkpoint_id);
    ASSERT_TRUE(finished_cp);
    ASSERT_GT(finished_cp->step, 0);

    RunConfig second = first;
    second.input     = json{{"value", "ask"}};
    const auto paused = engine->run(second);
    ASSERT_TRUE(paused.interrupted);
    const auto latest = coarse->load_latest("coarse-clock");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, paused.checkpoint_id) << "the checkpoint saved last is the latest";
    EXPECT_EQ(latest->timestamp, finished_cp->timestamp);
    EXPECT_LT(latest->step, finished_cp->step);
    EXPECT_EQ(coarse->list("coarse-clock").front().id, paused.checkpoint_id);

    const auto resumed = engine->resume("coarse-clock", json("answered"));
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(pg_coarse_gate_resumed.load(), 1) << "the paused node runs with the answer";
    EXPECT_EQ(resumed.output["channels"]["value"]["value"], json("answered"));
    EXPECT_EQ(resumed.output["channels"]["tail"]["value"], json(true));
}

namespace {

// neograph_checkpoints as the release before save_seq created it, with rows
// it wrote: within thread "t" its order was old-3, old-2, old-1.
constexpr const char* kPreviousCheckpointSchema = R"SQL(
DROP TABLE IF EXISTS neograph_checkpoints;
CREATE TABLE neograph_checkpoints (
    thread_id          TEXT    NOT NULL,
    checkpoint_id      TEXT    NOT NULL,
    parent_id          TEXT    NOT NULL DEFAULT '',
    current_node       TEXT    NOT NULL DEFAULT '',
    next_nodes         JSONB   NOT NULL DEFAULT '[]'::jsonb,
    interrupt_phase    TEXT    NOT NULL DEFAULT 'completed',
    barrier_state      JSONB   NOT NULL DEFAULT '{}'::jsonb,
    channel_versions   JSONB   NOT NULL DEFAULT '{}'::jsonb,
    global_version     BIGINT  NOT NULL DEFAULT 0,
    metadata           JSONB   NOT NULL DEFAULT '{}'::jsonb,
    step               BIGINT  NOT NULL DEFAULT 0,
    timestamp_ms       BIGINT  NOT NULL DEFAULT 0,
    schema_version     INT     NOT NULL DEFAULT 2,
    checkpoint_shape   JSONB,
    metadata_text      TEXT,
    checkpoint_shape_text TEXT,
    PRIMARY KEY (thread_id, checkpoint_id)
);
CREATE INDEX neograph_checkpoints_recent
    ON neograph_checkpoints (thread_id, timestamp_ms DESC, step DESC);
INSERT INTO neograph_checkpoints (thread_id, checkpoint_id, step, timestamp_ms) VALUES
    ('t', 'old-1', 0, 1000), ('t', 'old-3', 0, 2000), ('t', 'old-2', 1, 1000),
    ('other', 'other-1', 9, 5);
)SQL";

void expect_migrated_schema(PGconn* observer) {
    EXPECT_EQ(query_text(observer, "SELECT COUNT(*) FROM neograph_checkpoints WHERE save_seq IS NULL"),
              "4")
        << "the migration rewrites no legacy row";
    EXPECT_EQ(query_text(observer,
                  "SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef d JOIN pg_attribute a "
                  "ON a.attrelid = d.adrelid AND a.attnum = d.adnum "
                  "WHERE d.adrelid = 'neograph_checkpoints'::regclass AND a.attname = 'save_seq'"),
              "nextval('neograph_checkpoints_save_seq'::regclass)");
    EXPECT_EQ(query_text(observer, "SELECT to_regclass('neograph_checkpoints_recent')"), "<null>");
    EXPECT_EQ(query_text(observer, "SELECT to_regclass('neograph_checkpoints_save_order')"),
              "neograph_checkpoints_save_order");
}

}  // namespace

TEST_F(PostgresCheckpointTest, SaveOrderMigratesThePreviousSchemaInPlace) {
    TestPgConn observer{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(observer.raw), CONNECTION_OK) << PQerrorMessage(observer.raw);
    ASSERT_EQ(exec_ok(observer.raw, kPreviousCheckpointSchema), "");

    std::vector<std::string> order{"old-3", "old-2", "old-1"};
    {
        PostgresCheckpointStore migrated(pg_url(), 2);
        expect_migrated_schema(observer.raw);
        EXPECT_EQ(ids_of(migrated.list("t")), order) << "legacy rows keep (timestamp_ms, step)";
        ASSERT_TRUE(migrated.load_latest("t"));
        EXPECT_EQ(migrated.load_latest("t")->id, "old-3");

        // A new save is the latest, though its timestamp and step are lower.
        auto fresh      = make_state_cp("t", 0, {{"x", {1, 1}}});
        fresh.timestamp = 1;
        migrated.save(fresh);
        order.insert(order.begin(), fresh.id);
        EXPECT_EQ(ids_of(migrated.list("t")), order);
        EXPECT_EQ(ids_of(migrated.list("other")), (std::vector<std::string>{"other-1"}));
    }
    EXPECT_EQ(query_text(observer.raw, "SELECT COUNT(*) FROM neograph_checkpoints"), "5");

    // A writer that does not set save_seq, as the previous release does, takes
    // the column default and is the latest.
    ASSERT_EQ(exec_ok(observer.raw,
                  "INSERT INTO neograph_checkpoints (thread_id, checkpoint_id, step, timestamp_ms) "
                  "VALUES ('t', 'older-writer', 0, 0)"),
              "");
    order.insert(order.begin(), "older-writer");
    {
        // Initializing again, as every store and reconnect does, changes nothing.
        PostgresCheckpointStore again(pg_url(), 1);
        expect_migrated_schema(observer.raw);
        EXPECT_EQ(ids_of(again.list("t")), order);
        EXPECT_EQ(query_text(observer.raw,
                      "SELECT COUNT(*) FROM pg_attribute WHERE attrelid = "
                      "'neograph_checkpoints'::regclass AND attname = 'save_seq' AND NOT attisdropped"),
                  "1");

        // Re-saving a legacy row numbers it: it moves to the newest place.
        auto old_two = again.load_by_id("old-2");
        ASSERT_TRUE(old_two);
        again.save(*old_two);
        order.erase(std::find(order.begin(), order.end(), "old-2"));
        order.insert(order.begin(), "old-2");
        EXPECT_EQ(ids_of(again.list("t")), order);
        EXPECT_EQ(ids_of(list_async_now(again, "t")), order);
    }
    EXPECT_EQ(query_text(observer.raw,
                  "SELECT COUNT(*) FROM neograph_checkpoints WHERE save_seq IS NULL"),
              "3");
}

TEST_F(PostgresCheckpointTest, SaveOrderMigratesThePreviousSchemaOnAnAsyncReconnect) {
    TestPgConn observer{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(observer.raw), CONNECTION_OK) << PQerrorMessage(observer.raw);
    ASSERT_EQ(exec_ok(observer.raw, kPreviousCheckpointSchema), "");

    // A slot rebuilt on an executor initializes the schema asynchronously.
    auto pool = PoolTestAccess::make_pool(/*pool_size=*/1);
    PoolTestAccess::set_conn_str(*pool, pg_url());
    asio::io_context   io;
    std::exception_ptr rebuild_error;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> { co_await PoolTestAccess::rebuild_async(*pool, 0); },
        [&](std::exception_ptr error) { rebuild_error = error; });
    io.run();
    ASSERT_EQ(rebuild_error, nullptr);
    expect_migrated_schema(observer.raw);

    EXPECT_EQ(ids_of(list_async_now(*pool, "t")), (std::vector<std::string>{"old-3", "old-2", "old-1"}));
    auto fresh      = make_state_cp("t", 0, {{"x", {1, 1}}});
    fresh.timestamp = 1;
    save_async_now(*pool, fresh);
    const auto latest = latest_async_now(*pool, "t");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, fresh.id);
}

TEST_F(PostgresCheckpointTest, FourStoresInterleavePutsAndGetsWithinTimeout) {
    auto task = std::async(std::launch::async, [] {
        constexpr size_t kStorePoolSize = 2;
        PostgresCheckpointStore s1(pg_url(), kStorePoolSize);
        PostgresCheckpointStore s2(pg_url(), kStorePoolSize);
        PostgresCheckpointStore s3(pg_url(), kStorePoolSize);
        PostgresCheckpointStore s4(pg_url(), kStorePoolSize);

        std::array<PostgresCheckpointStore*, 4> stores = {&s1, &s2, &s3, &s4};
        std::vector<std::future<void>> workers;
        workers.reserve(stores.size());

        for (size_t worker_idx = 0; worker_idx < stores.size(); ++worker_idx) {
            workers.push_back(std::async(std::launch::async, [&stores, worker_idx] {
                for (int round = 0; round < 3; ++round) {
                    const std::string tid =
                        "multi-store-t-" + std::to_string(worker_idx) + "-" + std::to_string(round);
                    const int old_step = round * 20 + static_cast<int>(worker_idx) * 2 + 1;
                    const int new_step = old_step + 1;

                    auto cp_old = make_state_cp(
                        tid, old_step, {{"v", {old_step, static_cast<uint64_t>(round * 2 + 1)}}});
                    auto cp_new = make_state_cp(
                        tid, new_step, {{"v", {new_step, static_cast<uint64_t>(round * 2 + 2)}}});
                    cp_new.parent_id = cp_old.id;

                    auto& old_saver  = *stores[(worker_idx + static_cast<size_t>(round)) % stores.size()];
                    auto& old_writer = *stores[(worker_idx + static_cast<size_t>(round) + 1) % stores.size()];
                    auto& new_saver  = *stores[(worker_idx + static_cast<size_t>(round) + 2) % stores.size()];
                    auto& new_writer = *stores[(worker_idx + static_cast<size_t>(round) + 3) % stores.size()];

                    old_saver.save(cp_old);

                    PendingWrite pw_old;
                    pw_old.task_id   = "task-old-" + std::to_string(worker_idx) + "-" + std::to_string(round);
                    pw_old.task_path = "s0:executor_old";
                    pw_old.node_name = "executor";
                    pw_old.writes    = json::array({json{{"channel", "ch"}, {"value", old_step}}});
                    pw_old.command   = json();
                    pw_old.sends     = json::array();
                    pw_old.step      = old_step;
                    pw_old.timestamp = old_step;
                    old_writer.put_writes(tid, cp_old.id, pw_old);

                    new_saver.save(cp_new);

                    PendingWrite pw_new;
                    pw_new.task_id   = "task-new-" + std::to_string(worker_idx) + "-" + std::to_string(round);
                    pw_new.task_path = "s0:executor_new";
                    pw_new.node_name = "executor";
                    pw_new.writes    = json::array({json{{"channel", "ch"}, {"value", new_step}}});
                    pw_new.command   = json();
                    pw_new.sends     = json::array();
                    pw_new.step      = new_step;
                    pw_new.timestamp = new_step;
                    new_writer.put_writes(tid, cp_new.id, pw_new);

                    auto latest = old_writer.load_latest(tid);
                    if (!latest || latest->id != cp_new.id || latest->step != new_step ||
                        channel_val(*latest, "v") != json(new_step)) {
                        throw std::runtime_error("load_latest did not return the newest checkpoint");
                    }

                    // Read the older checkpoint by ID after the newer checkpoint was saved.
                    auto by_old = new_writer.load_by_id(cp_old.id);
                    if (!by_old || by_old->id != cp_old.id || by_old->step != old_step ||
                        channel_val(*by_old, "v") != json(old_step)) {
                        throw std::runtime_error("load_by_id did not preserve the older checkpoint");
                    }

                    auto by_new = old_saver.load_by_id(cp_new.id);
                    if (!by_new || by_new->id != cp_new.id || by_new->step != new_step ||
                        channel_val(*by_new, "v") != json(new_step)) {
                        throw std::runtime_error("load_by_id did not return the newer checkpoint");
                    }

                    auto writes_old = new_saver.get_writes(tid, cp_old.id);
                    if (writes_old.size() != 1 || writes_old[0].task_id != pw_old.task_id ||
                        writes_old[0].step != old_step ||
                        writes_old[0].writes[0]["value"] != json(old_step)) {
                        throw std::runtime_error("get_writes did not isolate the older checkpoint writes");
                    }

                    auto writes_new = old_saver.get_writes(tid, cp_new.id);
                    if (writes_new.size() != 1 || writes_new[0].task_id != pw_new.task_id ||
                        writes_new[0].step != new_step ||
                        writes_new[0].writes[0]["value"] != json(new_step)) {
                        throw std::runtime_error("get_writes did not isolate the newer checkpoint writes");
                    }
                }
            }));
        }

        for (auto& worker : workers) {
            worker.get();
        }
    });

    ASSERT_EQ(task.wait_for(std::chrono::seconds(15)), std::future_status::ready)
        << "Four stores concurrent put/get interleaving timed out";
    EXPECT_NO_THROW(task.get());
}

TEST_F(PostgresCheckpointTest, ThirdStoreConstructionDoesNotBlockOnActiveTableLocks) {
    PostgresCheckpointStore s1(pg_url(), 2);
    PostgresCheckpointStore s2(pg_url(), 2);

    auto seed = make_state_cp("lock-check-t", 1, {{"v", {10, 1}}});
    s1.save(seed);

    PendingWrite seed_write;
    seed_write.task_id   = "seed-task";
    seed_write.task_path = "s0:seed";
    seed_write.node_name = "seed";
    seed_write.writes    = json::array({json{{"channel", "v"}, {"value", 10}}});
    seed_write.command   = json();
    seed_write.sends     = json::array();
    seed_write.step      = 1;
    seed_write.timestamp = 1;
    s2.put_writes("lock-check-t", seed.id, seed_write);

    // Hold AccessShareLock on neograph_checkpoints and neograph_checkpoint_blobs
    // in two open transactions while constructing stores 3 and 4. Unconditional
    // ALTER TABLE in ensure_schema() would request AccessExclusiveLock and hang.
    TestPgConn holder_one{PQconnectdb(pg_url())};
    TestPgConn holder_two{PQconnectdb(pg_url())};
    ASSERT_EQ(PQstatus(holder_one.raw), CONNECTION_OK) << PQerrorMessage(holder_one.raw);
    ASSERT_EQ(PQstatus(holder_two.raw), CONNECTION_OK) << PQerrorMessage(holder_two.raw);
    ASSERT_EQ(exec_ok(holder_one.raw,
                      "BEGIN; SELECT COUNT(*) FROM neograph_checkpoints"),
              "");
    ASSERT_EQ(exec_ok(holder_two.raw,
                      "BEGIN; SELECT COUNT(*) FROM neograph_checkpoint_blobs"),
              "");

    auto task = std::async(std::launch::async, [&s1, &s2, &seed] {
        PostgresCheckpointStore s3(pg_url(), 2);
        PostgresCheckpointStore s4(pg_url(), 2);

        auto next_cp = make_state_cp("lock-check-t", 2, {{"v", {20, 2}}});
        next_cp.parent_id = seed.id;
        s3.save(next_cp);

        auto latest = s4.load_latest("lock-check-t");
        if (!latest || latest->id != next_cp.id || channel_val(*latest, "v") != json(20)) {
            throw std::runtime_error("store 4 failed to read latest checkpoint saved by store 3");
        }

        auto original = s3.load_by_id(seed.id);
        if (!original || original->id != seed.id || channel_val(*original, "v") != json(10)) {
            throw std::runtime_error("store 3 failed to read seed checkpoint by ID");
        }

        auto writes = s4.get_writes("lock-check-t", seed.id);
        if (writes.size() != 1 || writes[0].task_id != "seed-task") {
            throw std::runtime_error("store 4 failed to read seed writes");
        }

        auto s1_latest = s1.load_latest("lock-check-t");
        auto s2_latest = s2.load_latest("lock-check-t");
        if (!s1_latest || !s2_latest || s1_latest->id != next_cp.id || s2_latest->id != next_cp.id) {
            throw std::runtime_error("stores 1 and 2 did not observe checkpoint saved by store 3");
        }
    });

    const auto status = task.wait_for(std::chrono::seconds(10));
    (void)exec_ok(holder_one.raw, "ROLLBACK");
    (void)exec_ok(holder_two.raw, "ROLLBACK");

    ASSERT_EQ(status, std::future_status::ready)
        << "Constructing a third and fourth store hung behind active table locks";
    EXPECT_NO_THROW(task.get());
}
