#pragma once

#include <neograph/graph/checkpoint.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

struct pg_conn;

namespace neograph::graph {

struct PgConn {
    pg_conn* raw = nullptr;
    PgConn() = default;
    explicit PgConn(pg_conn* p) : raw(p) {}
    ~PgConn();
    PgConn(const PgConn&) = delete;
    PgConn& operator=(const PgConn&) = delete;
    PgConn(PgConn&&) = delete;
    PgConn& operator=(PgConn&&) = delete;
};

// Frozen origin/master declaration used to prove old-header/new-library ABI.
class NEOGRAPH_API PostgresCheckpointStore : public CheckpointStore {
public:
    explicit PostgresCheckpointStore(const std::string& conn_str,
                                     size_t pool_size = 8);
    size_t reconnect_count() const { return reconnect_count_; }
    size_t pool_size() const { return pool_.size(); }
    ~PostgresCheckpointStore() override;

    PostgresCheckpointStore(const PostgresCheckpointStore&) = delete;
    PostgresCheckpointStore& operator=(const PostgresCheckpointStore&) = delete;

    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                 int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    void put_writes(const std::string& thread_id,
                    const std::string& parent_checkpoint_id,
                    const PendingWrite& write) override;
    std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override;
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override;

    asio::awaitable<void> save_async(const Checkpoint& cp) override;
    asio::awaitable<std::optional<Checkpoint>>
    load_latest_async(const std::string& thread_id) override;
    asio::awaitable<std::optional<Checkpoint>>
    load_by_id_async(const std::string& id) override;
    asio::awaitable<std::vector<Checkpoint>>
    list_async(const std::string& thread_id, int limit = 100) override;
    asio::awaitable<void> delete_thread_async(const std::string& thread_id) override;
    asio::awaitable<void> put_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id,
        const PendingWrite& write) override;
    asio::awaitable<std::vector<PendingWrite>> get_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override;
    asio::awaitable<void> clear_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override;

    void drop_schema();
    size_t blob_count();

private:
    void ensure_schema();
    template <typename Fn> auto with_conn(Fn&& fn);
    template <typename Fn>
    auto with_conn_async(Fn fn) -> decltype(fn(std::declval<pg_conn*>()));
    size_t acquire_slot();
    void release_slot(size_t idx);

    std::string conn_str_;
    std::vector<std::unique_ptr<PgConn>> pool_;
    std::queue<size_t> free_;
    std::mutex pool_mutex_;
    std::condition_variable pool_cv_;
    std::atomic<size_t> reconnect_count_{0};
};

#if defined(__linux__) && INTPTR_MAX == INT64_MAX
static_assert(sizeof(PostgresCheckpointStore) == 240);
static_assert(alignof(PostgresCheckpointStore) == 8);
#endif

} // namespace neograph::graph
