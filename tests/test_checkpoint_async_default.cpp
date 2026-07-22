// Stage 3 / Semester 3.1 regression — CheckpointStore now exposes
// async peers (save_async, load_latest_async, …) alongside the sync
// API. Each pair is connected by a crossover default: subclasses
// override one side and inherit the other through run_sync /
// co_return.
//
// InMemoryCheckpointStore still overrides only the sync side. These
// cases pin that the async peers route through the default bridge
// correctly:
//   * save_async()/load_latest_async() round-trip a checkpoint.
//   * load_by_id_async() finds an explicit id.
//   * list_async() returns the persisted set.
//   * put_writes_async / get_writes_async / clear_writes_async track
//     pending writes through the same default bridge.

#include <gtest/gtest.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/async/run_sync.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

using namespace neograph::graph;

namespace {

Checkpoint make_cp(const std::string& thread, int step) {
    Checkpoint cp;
    cp.id = Checkpoint::generate_id();
    cp.thread_id = thread;
    cp.step = step;
    cp.timestamp = step * 1000;
    cp.interrupt_phase = CheckpointPhase::Completed;
    return cp;
}

class CapabilityStore final : public CheckpointStoreCore,
                              public AsyncCheckpointStore,
                              public PendingWritesCheckpointStore {
public:
    void save(const Checkpoint& cp) override { checkpoints[cp.id] = cp; }
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override {
        for (auto it = checkpoints.rbegin(); it != checkpoints.rend(); ++it) {
            if (it->second.thread_id == thread_id) return it->second;
        }
        return std::nullopt;
    }
    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        auto it = checkpoints.find(id);
        return it == checkpoints.end() ? std::nullopt
                                       : std::optional<Checkpoint>{it->second};
    }
    std::vector<Checkpoint> list(const std::string& thread_id, int limit) override {
        std::vector<Checkpoint> result;
        for (auto it = checkpoints.rbegin(); it != checkpoints.rend()
             && static_cast<int>(result.size()) < limit; ++it) {
            if (it->second.thread_id == thread_id) result.push_back(it->second);
        }
        return result;
    }
    void delete_thread(const std::string& thread_id) override {
        for (auto it = checkpoints.begin(); it != checkpoints.end();) {
            if (it->second.thread_id == thread_id) it = checkpoints.erase(it);
            else ++it;
        }
    }

    asio::awaitable<void> save_async(const Checkpoint& cp) override {
        ++async_calls;
        save(cp);
        co_return;
    }
    asio::awaitable<std::optional<Checkpoint>>
    load_latest_async(const std::string& thread_id) override {
        ++async_calls;
        co_return load_latest(thread_id);
    }
    asio::awaitable<std::optional<Checkpoint>>
    load_by_id_async(const std::string& id) override {
        ++async_calls;
        co_return load_by_id(id);
    }
    asio::awaitable<std::vector<Checkpoint>>
    list_async(const std::string& thread_id, int limit) override {
        ++async_calls;
        co_return list(thread_id, limit);
    }
    asio::awaitable<void> delete_thread_async(const std::string& thread_id) override {
        ++async_calls;
        delete_thread(thread_id);
        co_return;
    }

    void put_writes(const std::string&, const std::string&,
                    const PendingWrite& write) override {
        pending.push_back(write);
    }
    std::vector<PendingWrite> get_writes(const std::string&,
                                         const std::string&) override {
        return pending;
    }
    void clear_writes(const std::string&, const std::string&) override {
        pending.clear();
    }

    std::map<std::string, Checkpoint> checkpoints;
    std::vector<PendingWrite> pending;
    int async_calls = 0;
};

class CoreOnlyCapabilityStore final : public CheckpointStoreCore {
public:
    void save(const Checkpoint& cp) override { saved = cp; }
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override {
        return saved && saved->thread_id == thread_id ? saved : std::nullopt;
    }
    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        return saved && saved->id == id ? saved : std::nullopt;
    }
    std::vector<Checkpoint> list(const std::string& thread_id, int limit) override {
        if (limit > 0 && saved && saved->thread_id == thread_id) return {*saved};
        return {};
    }
    void delete_thread(const std::string& thread_id) override {
        if (saved && saved->thread_id == thread_id) saved.reset();
    }

    std::optional<Checkpoint> saved;
};

} // namespace

TEST(CheckpointAsyncDefault, SaveAndLoadLatestThroughBridge) {
    InMemoryCheckpointStore store;
    auto cp = make_cp("t1", 1);
    auto cp_id = cp.id;

    asio::io_context io;
    std::optional<Checkpoint> loaded;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await store.save_async(cp);
            loaded = co_await store.load_latest_async("t1");
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp_id);
    EXPECT_EQ(loaded->step, 1);
}

TEST(CheckpointAsyncDefault, LoadByIdAndListThroughBridge) {
    InMemoryCheckpointStore store;
    store.save(make_cp("t1", 1));
    auto cp2 = make_cp("t1", 2);
    store.save(cp2);
    store.save(make_cp("t1", 3));

    auto by_id = neograph::async::run_sync(store.load_by_id_async(cp2.id));
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->step, 2);

    auto listed = neograph::async::run_sync(store.list_async("t1", 100));
    EXPECT_EQ(listed.size(), 3u);
}

TEST(CheckpointAsyncDefault, PendingWritesRoundTripThroughBridge) {
    InMemoryCheckpointStore store;
    PendingWrite w;
    w.task_id = "task-A";
    w.task_path = "s1:n1";
    w.node_name = "n1";
    w.step = 1;
    w.timestamp = 100;

    neograph::async::run_sync(
        store.put_writes_async("t1", "parent-1", w));

    auto loaded = neograph::async::run_sync(
        store.get_writes_async("t1", "parent-1"));
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].task_id, "task-A");

    neograph::async::run_sync(
        store.clear_writes_async("t1", "parent-1"));

    auto after = neograph::async::run_sync(
        store.get_writes_async("t1", "parent-1"));
    EXPECT_TRUE(after.empty());
}

TEST(CheckpointAsyncDefault, DeleteThreadAsyncRoutesToSync) {
    InMemoryCheckpointStore store;
    store.save(make_cp("doomed", 1));
    store.save(make_cp("kept", 1));

    neograph::async::run_sync(store.delete_thread_async("doomed"));

    EXPECT_FALSE(store.load_latest("doomed").has_value());
    EXPECT_TRUE(store.load_latest("kept").has_value());
}

TEST(CheckpointCapabilities, AdapterDelegatesCoreAsyncAndPendingWrites) {
    auto capabilities = std::make_shared<CapabilityStore>();
    auto store = adapt_checkpoint_store(capabilities);
    auto cp = make_cp("capabilities", 1);

    neograph::async::run_sync(store->save_async(cp));
    auto loaded = neograph::async::run_sync(
        store->load_latest_async("capabilities"));
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp.id);
    EXPECT_EQ(capabilities->async_calls, 2);

    PendingWrite write;
    write.task_id = "task-capability";
    store->put_writes("capabilities", cp.id, write);
    ASSERT_EQ(store->get_writes("capabilities", cp.id).size(), 1u);
    EXPECT_EQ(store->get_writes("capabilities", cp.id)[0].task_id,
              "task-capability");
    store->clear_writes("capabilities", cp.id);
    EXPECT_TRUE(store->get_writes("capabilities", cp.id).empty());
}

TEST(CheckpointCapabilities, AdapterRejectsNullCore) {
    EXPECT_THROW((void)adapt_checkpoint_store(nullptr), std::invalid_argument);
}

TEST(CheckpointCapabilities, CoreOnlyAdapterProvidesLegacyAsyncFallback) {
    auto capabilities = std::make_shared<CoreOnlyCapabilityStore>();
    auto store = adapt_checkpoint_store(capabilities);
    auto cp = make_cp("core-only", 1);

    neograph::async::run_sync(store->save_async(cp));
    auto loaded = neograph::async::run_sync(store->load_latest_async("core-only"));
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp.id);
}
