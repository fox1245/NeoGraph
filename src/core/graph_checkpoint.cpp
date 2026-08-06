#include <neograph/graph/checkpoint.h>
#include <neograph/async/run_sync.h>

#include <asio/async_result.hpp>
#include <asio/bind_executor.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <type_traits>
#include <typeinfo>

namespace neograph::graph {

namespace {
enum class LegacyBridgeOperation : std::uint8_t {
    Save = 0,
    LoadLatest,
    LoadById,
    List,
    DeleteThread,
    PutWrites,
    GetWrites,
    ClearWrites,
};

thread_local std::array<bool, 8> legacy_bridge_active{};

class LegacyBridgeGuard {
public:
    explicit LegacyBridgeGuard(LegacyBridgeOperation operation)
        : active_(legacy_bridge_active[static_cast<std::size_t>(operation)]) {
        if (active_) {
            throw std::logic_error(
                "CheckpointStore must override at least one side of each sync/async pair");
        }
        active_ = true;
    }

    ~LegacyBridgeGuard() { active_ = false; }

private:
    bool& active_;
};

void reject_recursive_bridge(LegacyBridgeOperation operation) {
    if (legacy_bridge_active[static_cast<std::size_t>(operation)]) {
        throw std::logic_error(
            "CheckpointStore must override at least one side of each sync/async pair");
    }
}

asio::thread_pool& blocking_checkpoint_pool() {
    // A process-lifetime bounded pool prevents legacy synchronous stores from
    // pinning the caller's graph executor without creating one thread per
    // checkpoint operation.
    static asio::thread_pool pool([] {
        const auto hardware = std::thread::hardware_concurrency();
        return std::max<std::size_t>(
            2, std::min<std::size_t>(hardware == 0 ? 4 : hardware, 16));
    }());
    return pool;
}

template <typename Handler>
void post_checkpoint_completion(asio::any_io_executor caller_executor,
                                std::shared_ptr<Handler> completion) {
    struct CompletionBarrier {
        std::condition_variable cv;
        std::mutex mutex;
        bool post_returned = false;
    };

    auto barrier = std::make_shared<CompletionBarrier>();
    asio::post(
        caller_executor,
        [completion = std::move(completion), barrier]() mutable {
            std::unique_lock lock(barrier->mutex);
            barrier->cv.wait(lock, [&] { return barrier->post_returned; });
            lock.unlock();
            (*completion)();
        });

    {
        std::lock_guard lock(barrier->mutex);
        barrier->post_returned = true;
    }
    barrier->cv.notify_one();
}

bool is_exact_in_memory_store(const InMemoryCheckpointStore& store) {
    return typeid(store) == typeid(InMemoryCheckpointStore);
}

template <typename Fn>
asio::awaitable<void> run_blocking_checkpoint(Fn fn, LegacyBridgeOperation operation) {
    struct Result {
        std::exception_ptr error;
    };
    auto result = std::make_shared<Result>();

    const auto caller_executor = co_await asio::this_coro::executor;
    auto caller_work = asio::make_work_guard(caller_executor);
    auto completion_token = asio::bind_executor(caller_executor, asio::use_awaitable);
    co_await asio::async_initiate<decltype(completion_token), void()>(
        [fn = std::move(fn), result, caller_executor, operation](auto handler) mutable {
            using Handler = std::decay_t<decltype(handler)>;
            auto completion = std::make_shared<Handler>(std::move(handler));
            asio::post(
                blocking_checkpoint_pool().get_executor(),
                [fn = std::move(fn), result, completion = std::move(completion),
                 caller_executor, operation]() mutable {
                    try {
                        LegacyBridgeGuard guard(operation);
                        fn();
                    } catch (...) {
                        result->error = std::current_exception();
                    }
                    post_checkpoint_completion(
                        caller_executor, std::move(completion));
                });
        },
        completion_token);

    if (result->error) std::rethrow_exception(result->error);
    co_return;
}

template <typename T, typename Fn>
asio::awaitable<T> run_blocking_checkpoint(Fn fn, LegacyBridgeOperation operation) {
    struct Result {
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto result = std::make_shared<Result>();

    const auto caller_executor = co_await asio::this_coro::executor;
    auto caller_work = asio::make_work_guard(caller_executor);
    auto completion_token = asio::bind_executor(caller_executor, asio::use_awaitable);
    co_await asio::async_initiate<decltype(completion_token), void()>(
        [fn = std::move(fn), result, caller_executor, operation](auto handler) mutable {
            using Handler = std::decay_t<decltype(handler)>;
            auto completion = std::make_shared<Handler>(std::move(handler));
            asio::post(
                blocking_checkpoint_pool().get_executor(),
                [fn = std::move(fn), result, completion = std::move(completion),
                 caller_executor, operation]() mutable {
                    try {
                        LegacyBridgeGuard guard(operation);
                        result->value.emplace(fn());
                    } catch (...) {
                        result->error = std::current_exception();
                    }
                    post_checkpoint_completion(
                        caller_executor, std::move(completion));
                });
        },
        completion_token);

    if (result->error) std::rethrow_exception(result->error);
    co_return std::move(*result->value);
}


 

class CapabilityCheckpointStore final : public CheckpointStore {
public:
    explicit CapabilityCheckpointStore(std::shared_ptr<CheckpointStoreCore> core)
        : core_(std::move(core)),
          async_(std::dynamic_pointer_cast<AsyncCheckpointStore>(core_)),
          pending_(std::dynamic_pointer_cast<PendingWritesCheckpointStore>(core_)) {}

    void save(const Checkpoint& cp) override { core_->save(cp); }
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override {
        return core_->load_latest(thread_id);
    }
    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        return core_->load_by_id(id);
    }
    std::vector<Checkpoint> list(const std::string& thread_id, int limit) override {
        return core_->list(thread_id, limit);
    }
    void delete_thread(const std::string& thread_id) override {
        core_->delete_thread(thread_id);
    }

    asio::awaitable<void> save_async(const Checkpoint& cp) override {
        if (async_) {
            co_await async_->save_async(cp);
        } else {
            auto core = core_;
            co_await run_blocking_checkpoint(
                [core = std::move(core), cp] { core->save(cp); },
                LegacyBridgeOperation::Save);
        }
    }
    asio::awaitable<std::optional<Checkpoint>>
    load_latest_async(const std::string& thread_id) override {
        if (async_) co_return co_await async_->load_latest_async(thread_id);
        auto core = core_;
        co_return co_await run_blocking_checkpoint<std::optional<Checkpoint>>(
            [core = std::move(core), thread_id] { return core->load_latest(thread_id); },
            LegacyBridgeOperation::LoadLatest);
    }
    asio::awaitable<std::optional<Checkpoint>>
    load_by_id_async(const std::string& id) override {
        if (async_) co_return co_await async_->load_by_id_async(id);
        auto core = core_;
        co_return co_await run_blocking_checkpoint<std::optional<Checkpoint>>(
            [core = std::move(core), id] { return core->load_by_id(id); },
            LegacyBridgeOperation::LoadById);
    }
    asio::awaitable<std::vector<Checkpoint>>
    list_async(const std::string& thread_id, int limit) override {
        if (async_) co_return co_await async_->list_async(thread_id, limit);
        auto core = core_;
        co_return co_await run_blocking_checkpoint<std::vector<Checkpoint>>(
            [core = std::move(core), thread_id, limit] { return core->list(thread_id, limit); },
            LegacyBridgeOperation::List);
    }
    asio::awaitable<void> delete_thread_async(const std::string& thread_id) override {
        if (async_) {
            co_await async_->delete_thread_async(thread_id);
        } else {
            auto core = core_;
            co_await run_blocking_checkpoint(
                [core = std::move(core), thread_id] { core->delete_thread(thread_id); },
                LegacyBridgeOperation::DeleteThread);
        }
    }

    void put_writes(const std::string& thread_id,
                    const std::string& parent_checkpoint_id,
                    const PendingWrite& write) override {
        if (pending_) pending_->put_writes(thread_id, parent_checkpoint_id, write);
    }
    std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override {
        return pending_ ? pending_->get_writes(thread_id, parent_checkpoint_id)
                        : std::vector<PendingWrite>{};
    }
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override {
        if (pending_) pending_->clear_writes(thread_id, parent_checkpoint_id);
    }

private:
    std::shared_ptr<CheckpointStoreCore> core_;
    std::shared_ptr<AsyncCheckpointStore> async_;
    std::shared_ptr<PendingWritesCheckpointStore> pending_;
};

} // namespace

std::shared_ptr<CheckpointStore>
adapt_checkpoint_store(std::shared_ptr<CheckpointStoreCore> core) {
    if (!core) {
        throw std::invalid_argument("adapt_checkpoint_store requires a non-null core");
    }
    if (auto legacy = std::dynamic_pointer_cast<CheckpointStore>(core)) {
        return legacy;
    }
    return std::make_shared<CapabilityCheckpointStore>(std::move(core));
}

// =========================================================================
// CheckpointStore — sync ↔ async crossover defaults (Sem 3.1)
// =========================================================================
//
// Each pair below is the same shape as Provider::complete /
// complete_async: the sync method bridges to the async peer through
// run_sync, the async peer co_returns the sync call. Subclasses
// override one side and inherit the other.

void CheckpointStore::save(const Checkpoint& cp) {
    reject_recursive_bridge(LegacyBridgeOperation::Save);
    neograph::async::run_sync(save_async(cp));
}
asio::awaitable<void> CheckpointStore::save_async(const Checkpoint& cp) {
    co_await run_blocking_checkpoint(
        [this, cp] { save(cp); }, LegacyBridgeOperation::Save);
}

std::optional<Checkpoint>
CheckpointStore::load_latest(const std::string& thread_id) {
    reject_recursive_bridge(LegacyBridgeOperation::LoadLatest);
    return neograph::async::run_sync(load_latest_async(thread_id));
}
asio::awaitable<std::optional<Checkpoint>>
CheckpointStore::load_latest_async(const std::string& thread_id) {
    co_return co_await run_blocking_checkpoint<std::optional<Checkpoint>>(
        [this, thread_id] { return load_latest(thread_id); },
        LegacyBridgeOperation::LoadLatest);
}

std::optional<Checkpoint>
CheckpointStore::load_by_id(const std::string& id) {
    reject_recursive_bridge(LegacyBridgeOperation::LoadById);
    return neograph::async::run_sync(load_by_id_async(id));
}
asio::awaitable<std::optional<Checkpoint>>
CheckpointStore::load_by_id_async(const std::string& id) {
    co_return co_await run_blocking_checkpoint<std::optional<Checkpoint>>(
        [this, id] { return load_by_id(id); }, LegacyBridgeOperation::LoadById);
}

std::vector<Checkpoint>
CheckpointStore::list(const std::string& thread_id, int limit) {
    reject_recursive_bridge(LegacyBridgeOperation::List);
    return neograph::async::run_sync(list_async(thread_id, limit));
}
asio::awaitable<std::vector<Checkpoint>>
CheckpointStore::list_async(const std::string& thread_id, int limit) {
    co_return co_await run_blocking_checkpoint<std::vector<Checkpoint>>(
        [this, thread_id, limit] { return list(thread_id, limit); },
        LegacyBridgeOperation::List);
}

void CheckpointStore::delete_thread(const std::string& thread_id) {
    reject_recursive_bridge(LegacyBridgeOperation::DeleteThread);
    neograph::async::run_sync(delete_thread_async(thread_id));
}
asio::awaitable<void>
CheckpointStore::delete_thread_async(const std::string& thread_id) {
    co_await run_blocking_checkpoint(
        [this, thread_id] { delete_thread(thread_id); },
        LegacyBridgeOperation::DeleteThread);
}

asio::awaitable<void> CheckpointStore::put_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    co_await run_blocking_checkpoint(
        [this, thread_id, parent_checkpoint_id, write] {
            put_writes(thread_id, parent_checkpoint_id, write);
        },
        LegacyBridgeOperation::PutWrites);
}
asio::awaitable<std::vector<PendingWrite>> CheckpointStore::get_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    co_return co_await run_blocking_checkpoint<std::vector<PendingWrite>>(
        [this, thread_id, parent_checkpoint_id] {
            return get_writes(thread_id, parent_checkpoint_id);
        },
        LegacyBridgeOperation::GetWrites);
}
asio::awaitable<void> CheckpointStore::clear_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    co_await run_blocking_checkpoint(
        [this, thread_id, parent_checkpoint_id] {
            clear_writes(thread_id, parent_checkpoint_id);
        },
        LegacyBridgeOperation::ClearWrites);
}

// =========================================================================
// CheckpointPhase <-> string
// =========================================================================
const char* to_string(CheckpointPhase phase) {
    switch (phase) {
        case CheckpointPhase::Before:        return "before";
        case CheckpointPhase::After:         return "after";
        case CheckpointPhase::Completed:     return "completed";
        case CheckpointPhase::NodeInterrupt: return "node_interrupt";
        case CheckpointPhase::Updated:       return "updated";
    }
    return "unknown";  // unreachable — all enum values handled
}

CheckpointPhase parse_checkpoint_phase(std::string_view s) {
    if (s == "before")         return CheckpointPhase::Before;
    if (s == "after")          return CheckpointPhase::After;
    if (s == "completed")      return CheckpointPhase::Completed;
    if (s == "node_interrupt") return CheckpointPhase::NodeInterrupt;
    if (s == "updated")        return CheckpointPhase::Updated;
    throw std::invalid_argument(
        "parse_checkpoint_phase: unknown phase '" + std::string(s) + "'");
}

// =========================================================================
// UUID v4 generation
// =========================================================================
std::string Checkpoint::generate_id() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    auto r = [&]() { return dist(gen); };

    uint32_t a = r(), b = r(), c = r(), d = r();
    // Set version (4) and variant (10xx)
    b = (b & 0xFFFF0FFF) | 0x00004000;
    c = (c & 0x3FFFFFFF) | 0x80000000;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(8) << a << '-'
       << std::setw(4) << (b >> 16) << '-'
       << std::setw(4) << (b & 0xFFFF) << '-'
       << std::setw(4) << (c >> 16) << '-'
       << std::setw(4) << (c & 0xFFFF)
       << std::setw(8) << d;
    return ss.str();
}

// =========================================================================
// InMemoryCheckpointStore
// =========================================================================
//
// Channel-blob deduplication
// ──────────────────────────
// On `save()` each channel value is moved out of the cp's inline
// `channel_values["channels"][n]["value"]` and into `blobs_`, keyed by
// (thread_id, channel_name, version). The cp itself is stored as a
// shell that retains only `version` per channel. On `load_*()` the
// shell is rehydrated by joining the pointers back with the blob map.
//
// Why this is safe: `Channel::version` is the per-channel monotonic
// counter (assigned from a global write counter, see graph_state.cpp),
// so the same `(thread, channel, version)` triple uniquely identifies
// one value. Duplicate puts at the same key are no-ops.
//
// Why this is helpful: a typical super-step touches only a handful of
// channels; the rest carry over unchanged at the same version. Without
// dedup, every cp would re-store every channel — `O(steps × channels)`
// blobs. With dedup it's `O(distinct (channel, version) pairs)`, which
// in steady state is `O(steps + channels)`.

// The neograph::json wrapper has no in-place erase and `items()` yields
// pairs by value, so split_/join_ rebuild a fresh `channels` object
// rather than mutating in place. The cost is one extra deep copy per cp
// transition; in exchange we keep blob dedup on the persistence path.

Checkpoint InMemoryCheckpointStore::split_blobs_locked(Checkpoint cp) {
    if (!cp.channel_values.is_object()) return cp;
    if (!cp.channel_values.contains("channels")) return cp;
    json channels_in = cp.channel_values["channels"];
    if (!channels_in.is_object()) return cp;

    json shell_channels = json::object();
    for (auto [name, ch] : channels_in.items()) {
        if (!ch.is_object() || !ch.contains("version")) {
            // Unknown shape — pass through verbatim so we don't lose data.
            shell_channels[name] = ch;
            continue;
        }
        uint64_t ver = ch["version"].get<uint64_t>();
        if (ch.contains("value")) {
            auto key = std::make_tuple(cp.thread_id, name, ver);
            // try_emplace: first writer wins, identical re-puts are no-ops.
            // Same (thread, channel, version) implies same value because
            // version is monotonic per write — see graph_state.cpp.
            blobs_.try_emplace(key, ch["value"]);
        }
        json entry = json::object();
        entry["version"] = ver;
        shell_channels[name] = entry;
    }

    json new_cv = json::object();
    new_cv["channels"] = shell_channels;
    if (cp.channel_values.contains("global_version")) {
        new_cv["global_version"] = cp.channel_values["global_version"];
    }
    cp.channel_values = new_cv;
    return cp;
}

Checkpoint InMemoryCheckpointStore::join_blobs_locked(Checkpoint cp) const {
    if (!cp.channel_values.is_object()) return cp;
    if (!cp.channel_values.contains("channels")) return cp;
    json channels_in = cp.channel_values["channels"];
    if (!channels_in.is_object()) return cp;

    json full_channels = json::object();
    for (auto [name, ch] : channels_in.items()) {
        if (!ch.is_object() || !ch.contains("version")) {
            full_channels[name] = ch;
            continue;
        }
        json entry = json::object();
        entry["version"] = ch["version"];
        if (ch.contains("value")) {
            // Already inline (legacy blob never went through split_, or a
            // shape we deliberately preserved) — keep as-is.
            entry["value"] = ch["value"];
        } else {
            uint64_t ver = ch["version"].get<uint64_t>();
            auto key = std::make_tuple(cp.thread_id, name, ver);
            auto it = blobs_.find(key);
            // Defensive: missing blob yields null, never throws. Indicates
            // store corruption (cp shell present, blob evicted) — caller
            // sees a deserializable but stale value rather than a crash.
            entry["value"] = (it != blobs_.end()) ? it->second : json();
        }
        full_channels[name] = entry;
    }

    json new_cv = json::object();
    new_cv["channels"] = full_channels;
    if (cp.channel_values.contains("global_version")) {
        new_cv["global_version"] = cp.channel_values["global_version"];
    }
    cp.channel_values = new_cv;
    return cp;
}

void InMemoryCheckpointStore::save(const Checkpoint& cp) {
    std::lock_guard lock(mutex_);
    Checkpoint shell = split_blobs_locked(cp);
    by_id_[shell.id] = shell;
    by_thread_[shell.thread_id].push_back(std::move(shell));
}

std::optional<Checkpoint> InMemoryCheckpointStore::load_latest(
    const std::string& thread_id) {
    std::lock_guard lock(mutex_);
    auto it = by_thread_.find(thread_id);
    if (it == by_thread_.end() || it->second.empty()) return std::nullopt;
    return join_blobs_locked(it->second.back());
}

std::optional<Checkpoint> InMemoryCheckpointStore::load_by_id(
    const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return std::nullopt;
    return join_blobs_locked(it->second);
}

std::vector<Checkpoint> InMemoryCheckpointStore::list(
    const std::string& thread_id, int limit) {
    std::lock_guard lock(mutex_);
    auto it = by_thread_.find(thread_id);
    if (it == by_thread_.end()) return {};

    auto& vec = it->second;
    int count = std::min(limit, static_cast<int>(vec.size()));

    // Materialize each shell on the way out so callers always see full
    // inline values, matching pre-dedup behavior.
    std::vector<Checkpoint> result;
    result.reserve(count);
    for (auto rit = vec.rbegin(); rit != vec.rbegin() + count; ++rit) {
        result.push_back(join_blobs_locked(*rit));
    }
    return result;
}

void InMemoryCheckpointStore::delete_thread(const std::string& thread_id) {
    std::lock_guard lock(mutex_);
    auto it = by_thread_.find(thread_id);
    if (it != by_thread_.end()) {
        for (const auto& cp : it->second) {
            by_id_.erase(cp.id);
        }
        by_thread_.erase(it);
    }
    // Drop blobs for the thread. Linear scan; acceptable because
    // delete_thread is administrative, not on the hot path.
    for (auto bit = blobs_.begin(); bit != blobs_.end(); ) {
        if (std::get<0>(bit->first) == thread_id) {
            bit = blobs_.erase(bit);
        } else {
            ++bit;
        }
    }
}

asio::awaitable<void> InMemoryCheckpointStore::save_async(const Checkpoint& cp) {
    if (!is_exact_in_memory_store(*this)) {
        co_await CheckpointStore::save_async(cp);
        co_return;
    }
    save(cp);
}

asio::awaitable<std::optional<Checkpoint>>
InMemoryCheckpointStore::load_latest_async(const std::string& thread_id) {
    if (!is_exact_in_memory_store(*this)) {
        co_return co_await CheckpointStore::load_latest_async(thread_id);
    }
    co_return load_latest(thread_id);
}

asio::awaitable<std::optional<Checkpoint>>
InMemoryCheckpointStore::load_by_id_async(const std::string& id) {
    if (!is_exact_in_memory_store(*this)) {
        co_return co_await CheckpointStore::load_by_id_async(id);
    }
    co_return load_by_id(id);
}

asio::awaitable<std::vector<Checkpoint>>
InMemoryCheckpointStore::list_async(const std::string& thread_id, int limit) {
    if (!is_exact_in_memory_store(*this)) {
        co_return co_await CheckpointStore::list_async(thread_id, limit);
    }
    co_return list(thread_id, limit);
}

asio::awaitable<void>
InMemoryCheckpointStore::delete_thread_async(const std::string& thread_id) {
    if (!is_exact_in_memory_store(*this)) {
        co_await CheckpointStore::delete_thread_async(thread_id);
        co_return;
    }
    delete_thread(thread_id);
}

asio::awaitable<void> InMemoryCheckpointStore::put_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    if (!is_exact_in_memory_store(*this)) {
        co_await CheckpointStore::put_writes_async(
            thread_id, parent_checkpoint_id, write);
        co_return;
    }
    put_writes(thread_id, parent_checkpoint_id, write);
}

asio::awaitable<std::vector<PendingWrite>>
InMemoryCheckpointStore::get_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    if (!is_exact_in_memory_store(*this)) {
        co_return co_await CheckpointStore::get_writes_async(
            thread_id, parent_checkpoint_id);
    }
    co_return get_writes(thread_id, parent_checkpoint_id);
}

asio::awaitable<void> InMemoryCheckpointStore::clear_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    if (!is_exact_in_memory_store(*this)) {
        co_await CheckpointStore::clear_writes_async(
            thread_id, parent_checkpoint_id);
        co_return;
    }
    clear_writes(thread_id, parent_checkpoint_id);
}

size_t InMemoryCheckpointStore::size() const {
    std::lock_guard lock(mutex_);
    return by_id_.size();
}

size_t InMemoryCheckpointStore::blob_count() const {
    std::lock_guard lock(mutex_);
    return blobs_.size();
}

// =========================================================================
// Pending writes (fine-grained progress log)
// =========================================================================

void InMemoryCheckpointStore::put_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    std::lock_guard lock(mutex_);
    pending_[{thread_id, parent_checkpoint_id}].push_back(write);
}

std::vector<PendingWrite> InMemoryCheckpointStore::get_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    std::lock_guard lock(mutex_);
    auto it = pending_.find({thread_id, parent_checkpoint_id});
    if (it == pending_.end()) return {};
    return it->second;
}

void InMemoryCheckpointStore::clear_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    std::lock_guard lock(mutex_);
    pending_.erase({thread_id, parent_checkpoint_id});
}

size_t InMemoryCheckpointStore::pending_writes_count(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) const {
    std::lock_guard lock(mutex_);
    auto it = pending_.find({thread_id, parent_checkpoint_id});
    if (it == pending_.end()) return 0;
    return it->second.size();
}

} // namespace neograph::graph
