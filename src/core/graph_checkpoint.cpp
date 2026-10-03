#include <neograph/graph/checkpoint.h>
#include <neograph/async/run_sync.h>
#include "managed_budget_journal.h"

#include <asio/async_result.hpp>
#include <asio/bind_executor.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
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

json checkpoint_storage_metadata(const Checkpoint& checkpoint) {
    if (checkpoint.native_history || checkpoint.native_subgraph_writes ||
        (checkpoint.channel_values.is_object() && checkpoint.channel_values.value("native_checkpoint_required", false)))
        throw std::invalid_argument("In-memory native custody requires explicit archive serialization before durable storage");
    constexpr const char* key = "_neograph_provider_checkpoint";
    if (checkpoint.metadata.is_object() && checkpoint.metadata.contains(key))
        throw std::invalid_argument("Checkpoint metadata uses a reserved provider custody key");
    json state = json::object();
    if (checkpoint.channel_values.is_object())
        for (const auto& [name, value] : checkpoint.channel_values.items())
            if (name != "channels" && name != "global_version") state[name] = value;
    if (state.empty()) return checkpoint.metadata;
    return json{{key, json{{"schema", "neograph.checkpoint-provider-state/v1"},
        {"metadata", checkpoint.metadata}, {"state", std::move(state)}}}};
}

void restore_checkpoint_storage_envelope(Checkpoint& checkpoint) {
    constexpr const char* key = "_neograph_provider_checkpoint";
    if (!checkpoint.metadata.is_object() || !checkpoint.metadata.contains(key)) return;
    const auto envelope = checkpoint.metadata.at(key);
    if (!envelope.is_object() || envelope.value("schema", std::string{}) != "neograph.checkpoint-provider-state/v1" ||
        !envelope.contains("metadata") || !envelope.contains("state") || !envelope.at("state").is_object())
        throw std::invalid_argument("Invalid durable provider checkpoint envelope");
    for (const auto& [name, value] : envelope.at("state").items()) {
        if (name == "channels" || name == "global_version")
            throw std::invalid_argument("Provider checkpoint envelope cannot replace state channels");
        checkpoint.channel_values[name] = value;
    }
    checkpoint.metadata = envelope.at("metadata");
}

namespace {

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
void post_checkpoint_completion(asio::any_io_executor& caller_executor,
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

    // The resumed coroutine may finish run_sync and destroy its io_context.
    // Release the pool worker's executor before allowing that to happen: a
    // retained strand also touches the context's service during destruction.
    caller_executor = {};
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
asio::awaitable<void> run_blocking_checkpoint(Fn fn) {
    struct Result {
        std::exception_ptr error;
    };
    auto result = std::make_shared<Result>();

    auto caller_executor = co_await asio::this_coro::executor;
    auto caller_work = asio::make_work_guard(caller_executor);
    auto completion_token = asio::bind_executor(caller_executor, asio::use_awaitable);
    co_await asio::async_initiate<decltype(completion_token), void()>(
        [fn = std::move(fn), result, caller_executor](auto handler) mutable {
            using Handler = std::decay_t<decltype(handler)>;
            auto completion = std::make_shared<Handler>(std::move(handler));
            asio::post(
                blocking_checkpoint_pool().get_executor(),
                [fn = std::move(fn), result, completion = std::move(completion),
                 caller_executor]() mutable {
                    try {
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
asio::awaitable<T> run_blocking_checkpoint(Fn fn) {
    struct Result {
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto result = std::make_shared<Result>();

    auto caller_executor = co_await asio::this_coro::executor;
    auto caller_work = asio::make_work_guard(caller_executor);
    auto completion_token = asio::bind_executor(caller_executor, asio::use_awaitable);
    co_await asio::async_initiate<decltype(completion_token), void()>(
        [fn = std::move(fn), result, caller_executor](auto handler) mutable {
            using Handler = std::decay_t<decltype(handler)>;
            auto completion = std::make_shared<Handler>(std::move(handler));
            asio::post(
                blocking_checkpoint_pool().get_executor(),
                [fn = std::move(fn), result, completion = std::move(completion),
                 caller_executor]() mutable {
                    try {
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
    explicit CapabilityCheckpointStore(std::shared_ptr<AsyncCheckpointStore> async)
        : async_(std::move(async)),
          pending_(std::dynamic_pointer_cast<PendingWritesCheckpointStore>(async_)) {}

    void save(const Checkpoint& cp) override {
        if (core_) core_->save(cp);
        else neograph::async::run_sync(async_->save_async(cp));
    }
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override {
        return core_ ? core_->load_latest(thread_id)
                     : neograph::async::run_sync(async_->load_latest_async(thread_id));
    }
    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        return core_ ? core_->load_by_id(id)
                     : neograph::async::run_sync(async_->load_by_id_async(id));
    }
    std::vector<Checkpoint> list(const std::string& thread_id, int limit) override {
        return core_ ? core_->list(thread_id, limit)
                     : neograph::async::run_sync(async_->list_async(thread_id, limit));
    }
    void delete_thread(const std::string& thread_id) override {
        if (core_) core_->delete_thread(thread_id);
        else neograph::async::run_sync(async_->delete_thread_async(thread_id));
    }

    asio::awaitable<void> save_async(const Checkpoint& cp) override {
        if (async_) {
            co_await async_->save_async(cp);
        } else {
            auto core = core_;
            co_await run_blocking_checkpoint(
                [core = std::move(core), cp] { core->save(cp); });
        }
    }
    asio::awaitable<std::optional<Checkpoint>>
    load_latest_async(const std::string& thread_id) override {
        if (async_) co_return co_await async_->load_latest_async(thread_id);
        auto core = core_;
        co_return co_await run_blocking_checkpoint<std::optional<Checkpoint>>(
            [core = std::move(core), thread_id] { return core->load_latest(thread_id); });
    }
    asio::awaitable<std::optional<Checkpoint>>
    load_by_id_async(const std::string& id) override {
        if (async_) co_return co_await async_->load_by_id_async(id);
        auto core = core_;
        co_return co_await run_blocking_checkpoint<std::optional<Checkpoint>>(
            [core = std::move(core), id] { return core->load_by_id(id); });
    }
    asio::awaitable<std::vector<Checkpoint>>
    list_async(const std::string& thread_id, int limit) override {
        if (async_) co_return co_await async_->list_async(thread_id, limit);
        auto core = core_;
        co_return co_await run_blocking_checkpoint<std::vector<Checkpoint>>(
            [core = std::move(core), thread_id, limit] { return core->list(thread_id, limit); });
    }
    asio::awaitable<void> delete_thread_async(const std::string& thread_id) override {
        if (async_) {
            co_await async_->delete_thread_async(thread_id);
        } else {
            auto core = core_;
            co_await run_blocking_checkpoint(
                [core = std::move(core), thread_id] { core->delete_thread(thread_id); });
        }
    }

    bool requires_managed_budget(const std::string& thread_id) override {
        return core_ ? core_->requires_managed_budget(thread_id)
                     : neograph::async::run_sync(async_->requires_managed_budget_async(thread_id));
    }
    asio::awaitable<bool> requires_managed_budget_async(std::string thread_id) override {
        if (async_) co_return co_await async_->requires_managed_budget_async(std::move(thread_id));
        auto core = core_;
        co_return co_await run_blocking_checkpoint<bool>(
            [core = std::move(core), thread_id = std::move(thread_id)] {
                return core->requires_managed_budget(thread_id);
            });
    }

    std::shared_ptr<OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const ManagedBudgetLeaseScope& scope, const std::string& id, const std::string& commitment) override {
        return core_ ? core_->acquire_managed_budget_lease(scope, id, commitment)
            : neograph::async::run_sync(async_->acquire_managed_budget_lease_async(scope, id, commitment));
    }
    asio::awaitable<std::shared_ptr<OwnedManagedBudgetLease>> acquire_managed_budget_lease_async(
        ManagedBudgetLeaseScope scope, std::string id, std::string commitment) override {
        if (async_) co_return co_await async_->acquire_managed_budget_lease_async(
            std::move(scope), std::move(id), std::move(commitment));
        auto core = core_;
        co_return co_await run_blocking_checkpoint<std::shared_ptr<OwnedManagedBudgetLease>>(
            [core = std::move(core), scope = std::move(scope), id = std::move(id),
             commitment = std::move(commitment)] {
                return core->acquire_managed_budget_lease(scope, id, commitment);
            });
    }
    ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& id,
        std::uint64_t amount, const std::string& digest) override {
        return core_ ? core_->begin_managed_budget_effect(lease, id, amount, digest)
            : neograph::async::run_sync(async_->begin_managed_budget_effect_async(lease, id, amount, digest));
    }
    asio::awaitable<ManagedBudgetEffectReceipt> begin_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, std::string id,
        std::uint64_t amount, std::string digest) override {
        if (async_) co_return co_await async_->begin_managed_budget_effect_async(
            std::move(lease), std::move(id), amount, std::move(digest));
        auto core = core_;
        co_return co_await run_blocking_checkpoint<ManagedBudgetEffectReceipt>(
            [core = std::move(core), lease = std::move(lease), id = std::move(id),
             amount, digest = std::move(digest)] {
                return core->begin_managed_budget_effect(lease, id, amount, digest);
            });
    }
    void settle_managed_budget_effect(const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const ManagedBudgetEffectReceipt& effect, sp::runtime::Result outcome,
        const UsageAccumulator::AuthoritySnapshot& authority) override {
        if (core_) core_->settle_managed_budget_effect(lease, effect, std::move(outcome), authority);
        else neograph::async::run_sync(async_->settle_managed_budget_effect_async(
            lease, effect, std::move(outcome), authority));
    }
    asio::awaitable<void> settle_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, ManagedBudgetEffectReceipt effect,
        sp::runtime::Result outcome, UsageAccumulator::AuthoritySnapshot authority) override {
        if (async_) {
            co_await async_->settle_managed_budget_effect_async(
                std::move(lease), std::move(effect), std::move(outcome), std::move(authority));
            co_return;
        }
        auto core = core_;
        co_await run_blocking_checkpoint([core = std::move(core), lease = std::move(lease),
            effect = std::move(effect), outcome = std::move(outcome), authority = std::move(authority)] {
                core->settle_managed_budget_effect(lease, effect, outcome, authority);
            });
    }
    void publish_managed_budget_checkpoint(const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const Checkpoint& checkpoint) override {
        if (core_) core_->publish_managed_budget_checkpoint(lease, checkpoint);
        else neograph::async::run_sync(async_->publish_managed_budget_checkpoint_async(lease, checkpoint));
    }
    asio::awaitable<void> publish_managed_budget_checkpoint_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, Checkpoint checkpoint) override {
        if (async_) {
            co_await async_->publish_managed_budget_checkpoint_async(std::move(lease), std::move(checkpoint));
            co_return;
        }
        auto core = core_;
        co_await run_blocking_checkpoint([core = std::move(core), lease = std::move(lease),
            checkpoint = std::move(checkpoint)] {
                core->publish_managed_budget_checkpoint(lease, checkpoint);
            });
    }
    void release_managed_budget_lease(const std::shared_ptr<OwnedManagedBudgetLease>& lease) override {
        if (core_) core_->release_managed_budget_lease(lease);
        else neograph::async::run_sync(async_->release_managed_budget_lease_async(lease));
    }
    asio::awaitable<void> release_managed_budget_lease_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease) override {
        if (async_) {
            co_await async_->release_managed_budget_lease_async(std::move(lease));
            co_return;
        }
        auto core = core_;
        co_await run_blocking_checkpoint([core = std::move(core), lease = std::move(lease)] {
            core->release_managed_budget_lease(lease);
        });
    }
    bool retains_native_checkpoint() const noexcept override {
        return core_ ? core_->retains_native_checkpoint() : async_->retains_native_checkpoint();
    }
    void publish_managed_budget_fork(const Checkpoint& source, const Checkpoint& forked) override {
        if (core_) core_->publish_managed_budget_fork(source, forked);
        else neograph::async::run_sync(async_->publish_managed_budget_fork_async(source, forked));
    }
    asio::awaitable<void> publish_managed_budget_fork_async(Checkpoint source, Checkpoint forked) override {
        if (async_) {
            co_await async_->publish_managed_budget_fork_async(std::move(source), std::move(forked));
            co_return;
        }
        auto core = core_;
        co_await run_blocking_checkpoint([core = std::move(core), source = std::move(source),
                                         forked = std::move(forked)] {
            core->publish_managed_budget_fork(source, forked);
        });
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
std::shared_ptr<CheckpointStore>
adapt_async_checkpoint_store(std::shared_ptr<AsyncCheckpointStore> backend) {
    if (!backend)
        throw std::invalid_argument("adapt_async_checkpoint_store requires a backend");
    return std::make_shared<CapabilityCheckpointStore>(std::move(backend));
}


// =========================================================================
// CheckpointStore — explicit legacy sync errors; async offloads sync backends
// =========================================================================
// Async-native backends must opt in through adapt_async_checkpoint_store().
// The base sync facade never calls an async method, so an omitted backend
// operation reports a capability error instead of recursing.

void CheckpointStore::save(const Checkpoint&) {
    throw std::logic_error("CheckpointStore::save requires a synchronous backend");
}
asio::awaitable<void> CheckpointStore::save_async(const Checkpoint& cp) {
    co_await run_blocking_checkpoint([this, cp] { save(cp); });
}

std::optional<Checkpoint>
CheckpointStore::load_latest(const std::string&) {
    throw std::logic_error("CheckpointStore::load_latest requires a synchronous backend");
}
asio::awaitable<std::optional<Checkpoint>>
CheckpointStore::load_latest_async(const std::string& thread_id) {
    co_return co_await run_blocking_checkpoint<std::optional<Checkpoint>>(
        [this, thread_id] { return load_latest(thread_id); });
}

std::optional<Checkpoint>
CheckpointStore::load_by_id(const std::string&) {
    throw std::logic_error("CheckpointStore::load_by_id requires a synchronous backend");
}
asio::awaitable<std::optional<Checkpoint>>
CheckpointStore::load_by_id_async(const std::string& id) {
    co_return co_await run_blocking_checkpoint<std::optional<Checkpoint>>(
        [this, id] { return load_by_id(id); });
}

std::vector<Checkpoint>
CheckpointStore::list(const std::string&, int) {
    throw std::logic_error("CheckpointStore::list requires a synchronous backend");
}
asio::awaitable<std::vector<Checkpoint>>
CheckpointStore::list_async(const std::string& thread_id, int limit) {
    co_return co_await run_blocking_checkpoint<std::vector<Checkpoint>>(
        [this, thread_id, limit] { return list(thread_id, limit); });
}

void CheckpointStore::delete_thread(const std::string&) {
    throw std::logic_error("CheckpointStore::delete_thread requires a synchronous backend");
}
asio::awaitable<void>
CheckpointStore::delete_thread_async(const std::string& thread_id) {
    co_await run_blocking_checkpoint(
        [this, thread_id] { delete_thread(thread_id); });
}

asio::awaitable<void> CheckpointStore::put_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    co_await run_blocking_checkpoint(
        [this, thread_id, parent_checkpoint_id, write] {
            put_writes(thread_id, parent_checkpoint_id, write);
        });
}
asio::awaitable<std::vector<PendingWrite>> CheckpointStore::get_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    co_return co_await run_blocking_checkpoint<std::vector<PendingWrite>>(
        [this, thread_id, parent_checkpoint_id] {
            return get_writes(thread_id, parent_checkpoint_id);
        });
}
asio::awaitable<void> CheckpointStore::clear_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    co_await run_blocking_checkpoint(
        [this, thread_id, parent_checkpoint_id] {
            clear_writes(thread_id, parent_checkpoint_id);
        });
}

[[noreturn]] static bool unsupported_managed_budget_obligations() {
    throw std::logic_error("Checkpoint backend does not support managed-bank custody obligations");
}

bool CheckpointStoreCore::requires_managed_budget(const std::string&) {
    return unsupported_managed_budget_obligations();
}
asio::awaitable<bool> AsyncCheckpointStore::requires_managed_budget_async(std::string) {
    co_return unsupported_managed_budget_obligations();
}
bool CheckpointStore::requires_managed_budget(const std::string&) {
    return unsupported_managed_budget_obligations();
}
asio::awaitable<bool> CheckpointStore::requires_managed_budget_async(std::string thread_id) {
    co_return co_await run_blocking_checkpoint<bool>(
        [this, thread_id = std::move(thread_id)] { return requires_managed_budget(thread_id); });
}

template <typename T>
[[noreturn]] static T unsupported_managed_budget_journal() {
    throw std::logic_error("Checkpoint backend does not support owned managed-bank journals");
}

std::shared_ptr<OwnedManagedBudgetLease> CheckpointStoreCore::acquire_managed_budget_lease(
    const ManagedBudgetLeaseScope&, const std::string&, const std::string&) {
    return unsupported_managed_budget_journal<std::shared_ptr<OwnedManagedBudgetLease>>();
}
ManagedBudgetEffectReceipt CheckpointStoreCore::begin_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>&, const std::string&, std::uint64_t, const std::string&) {
    return unsupported_managed_budget_journal<ManagedBudgetEffectReceipt>();
}
void CheckpointStoreCore::settle_managed_budget_effect(const std::shared_ptr<OwnedManagedBudgetLease>&,
    const ManagedBudgetEffectReceipt&, sp::runtime::Result, const UsageAccumulator::AuthoritySnapshot&) {
    unsupported_managed_budget_journal<void>();
}
void CheckpointStoreCore::publish_managed_budget_checkpoint(
    const std::shared_ptr<OwnedManagedBudgetLease>&, const Checkpoint&) {
    unsupported_managed_budget_journal<void>();
}
void CheckpointStoreCore::release_managed_budget_lease(const std::shared_ptr<OwnedManagedBudgetLease>&) {
    unsupported_managed_budget_journal<void>();
}
asio::awaitable<std::shared_ptr<OwnedManagedBudgetLease>> AsyncCheckpointStore::acquire_managed_budget_lease_async(
    ManagedBudgetLeaseScope, std::string, std::string) {
    co_return unsupported_managed_budget_journal<std::shared_ptr<OwnedManagedBudgetLease>>();
}
asio::awaitable<ManagedBudgetEffectReceipt> AsyncCheckpointStore::begin_managed_budget_effect_async(
    std::shared_ptr<OwnedManagedBudgetLease>, std::string, std::uint64_t, std::string) {
    co_return unsupported_managed_budget_journal<ManagedBudgetEffectReceipt>();
}
asio::awaitable<void> AsyncCheckpointStore::settle_managed_budget_effect_async(
    std::shared_ptr<OwnedManagedBudgetLease>, ManagedBudgetEffectReceipt, sp::runtime::Result,
    UsageAccumulator::AuthoritySnapshot) {
    co_return unsupported_managed_budget_journal<void>();
}
asio::awaitable<void> AsyncCheckpointStore::publish_managed_budget_checkpoint_async(
    std::shared_ptr<OwnedManagedBudgetLease>, Checkpoint) {
    co_return unsupported_managed_budget_journal<void>();
}
asio::awaitable<void> AsyncCheckpointStore::release_managed_budget_lease_async(
    std::shared_ptr<OwnedManagedBudgetLease>) {
    co_return unsupported_managed_budget_journal<void>();
}
std::shared_ptr<OwnedManagedBudgetLease> CheckpointStore::acquire_managed_budget_lease(
    const ManagedBudgetLeaseScope&, const std::string&, const std::string&) {
    return unsupported_managed_budget_journal<std::shared_ptr<OwnedManagedBudgetLease>>();
}
asio::awaitable<std::shared_ptr<OwnedManagedBudgetLease>> CheckpointStore::acquire_managed_budget_lease_async(
    ManagedBudgetLeaseScope scope, std::string id, std::string commitment) {
    co_return co_await run_blocking_checkpoint<std::shared_ptr<OwnedManagedBudgetLease>>(
        [this, scope = std::move(scope), id = std::move(id), commitment = std::move(commitment)] {
            return acquire_managed_budget_lease(scope, id, commitment);
        });
}
ManagedBudgetEffectReceipt CheckpointStore::begin_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>&, const std::string&, std::uint64_t, const std::string&) {
    return unsupported_managed_budget_journal<ManagedBudgetEffectReceipt>();
}
asio::awaitable<ManagedBudgetEffectReceipt> CheckpointStore::begin_managed_budget_effect_async(
    std::shared_ptr<OwnedManagedBudgetLease> lease, std::string id, std::uint64_t amount, std::string digest) {
    co_return co_await run_blocking_checkpoint<ManagedBudgetEffectReceipt>(
        [this, lease = std::move(lease), id = std::move(id), amount, digest = std::move(digest)] {
            return begin_managed_budget_effect(lease, id, amount, digest);
        });
}
void CheckpointStore::settle_managed_budget_effect(const std::shared_ptr<OwnedManagedBudgetLease>&,
    const ManagedBudgetEffectReceipt&, sp::runtime::Result, const UsageAccumulator::AuthoritySnapshot&) {
    unsupported_managed_budget_journal<void>();
}
asio::awaitable<void> CheckpointStore::settle_managed_budget_effect_async(
    std::shared_ptr<OwnedManagedBudgetLease> lease, ManagedBudgetEffectReceipt effect,
    sp::runtime::Result outcome, UsageAccumulator::AuthoritySnapshot authority) {
    co_await run_blocking_checkpoint([this, lease = std::move(lease), effect = std::move(effect),
        outcome = std::move(outcome), authority = std::move(authority)] {
            settle_managed_budget_effect(lease, effect, outcome, authority);
        });
}
void CheckpointStore::publish_managed_budget_checkpoint(
    const std::shared_ptr<OwnedManagedBudgetLease>&, const Checkpoint&) {
    unsupported_managed_budget_journal<void>();
}
asio::awaitable<void> CheckpointStore::publish_managed_budget_checkpoint_async(
    std::shared_ptr<OwnedManagedBudgetLease> lease, Checkpoint checkpoint) {
    co_await run_blocking_checkpoint([this, lease = std::move(lease), checkpoint = std::move(checkpoint)] {
        publish_managed_budget_checkpoint(lease, checkpoint);
    });
}
void CheckpointStore::release_managed_budget_lease(const std::shared_ptr<OwnedManagedBudgetLease>&) {
    unsupported_managed_budget_journal<void>();
}
asio::awaitable<void> CheckpointStore::release_managed_budget_lease_async(
    std::shared_ptr<OwnedManagedBudgetLease> lease) {
    co_await run_blocking_checkpoint([this, lease = std::move(lease)] {
        release_managed_budget_lease(lease);
    });
}

void CheckpointStoreCore::publish_managed_budget_fork(const Checkpoint&, const Checkpoint&) {
    throw std::logic_error("Checkpoint backend does not support original C++ shared-bank forks");
}
asio::awaitable<void> AsyncCheckpointStore::publish_managed_budget_fork_async(Checkpoint, Checkpoint) {
    throw std::logic_error("Checkpoint backend does not support original C++ shared-bank forks");
    co_return;
}
void CheckpointStore::publish_managed_budget_fork(const Checkpoint&, const Checkpoint&) {
    throw std::logic_error("Checkpoint backend does not support original C++ shared-bank forks");
}
asio::awaitable<void> CheckpointStore::publish_managed_budget_fork_async(Checkpoint source, Checkpoint forked) {
    co_await run_blocking_checkpoint([this, source = std::move(source), forked = std::move(forked)] {
        publish_managed_budget_fork(source, forked);
    });
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

    cp.channel_values["channels"] = std::move(shell_channels);
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

    cp.channel_values["channels"] = std::move(full_channels);
    return cp;
}

void InMemoryCheckpointStore::save(const Checkpoint& cp) {
    std::lock_guard lock(mutex_);
    // Ordinary new rows never advance the trusted current branch. Replacing
    // that exact ID with edited content or custody permanently invalidates it.
    if (const auto previous = by_id_.find(cp.id); previous != by_id_.end()) {
        if (const auto branch = managed_budget_branch_heads_.find(previous->second.thread_id);
            branch != managed_budget_branch_heads_.end() && branch->second.checkpoint_id == cp.id &&
            (branch->second.commitment != managed_budget_checkpoint_commitment(cp) ||
             branch->second.native_history != cp.native_history))
            branch->second.valid = false;
    }
    if (detail::ManagedBudgetJournalAccess::checkpoint_requires_obligation(cp))
        managed_budget_obligations_.insert(cp.thread_id);
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
    if (const auto branch = managed_budget_branch_heads_.find(thread_id);
        branch != managed_budget_branch_heads_.end()) branch->second.valid = false;
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

bool InMemoryCheckpointStore::requires_managed_budget(const std::string& thread_id) {
    std::lock_guard lock(mutex_);
    return managed_budget_obligations_.contains(thread_id);
}
asio::awaitable<bool> InMemoryCheckpointStore::requires_managed_budget_async(std::string thread_id) {
    if (!is_exact_in_memory_store(*this))
        co_return co_await CheckpointStore::requires_managed_budget_async(std::move(thread_id));
    co_return requires_managed_budget(thread_id);
}

const InMemoryCheckpointStore::ManagedBudgetBranchHead&
InMemoryCheckpointStore::authenticate_managed_budget_branch_locked(
    const Checkpoint& source, const std::string& commitment) const {
    const auto branch = managed_budget_branch_heads_.find(source.thread_id);
    const auto stored = by_id_.find(source.id);
    if (branch == managed_budget_branch_heads_.end() || !branch->second.valid ||
        branch->second.checkpoint_id != source.id || branch->second.commitment != commitment ||
        branch->second.native_history != source.native_history || stored == by_id_.end() ||
        stored->second.thread_id != source.thread_id || stored->second.native_history != source.native_history ||
        managed_budget_checkpoint_commitment(source) != commitment ||
        managed_budget_checkpoint_commitment(join_blobs_locked(stored->second)) != commitment)
        throw std::invalid_argument("Managed-bank source is not the actual trusted current branch head");
    return branch->second;
}

void InMemoryCheckpointStore::validate_managed_budget_execution_locked(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const json& head) const {
    const auto key = detail::ManagedBudgetJournalAccess::storage_key(lease->scope());
    const auto& execution_key = lease->execution_storage_thread_id();
    const auto id = head.at("head_checkpoint_id").get<std::string_view>();
    if (id.empty()) {
        if (execution_key != key || lease->execution_thread_id() != lease->scope().thread_id)
            throw std::invalid_argument("Managed-bank unpublished root cannot acquire branch execution authority");
        return;
    }
    const auto branch = managed_budget_branch_heads_.find(execution_key);
    if (branch == managed_budget_branch_heads_.end() ||
        branch->second.financial_storage_thread_id != key ||
        branch->second.execution_thread_id != lease->execution_thread_id() ||
        branch->second.checkpoint_id != id ||
        branch->second.commitment != head.at("head_commitment").get<std::string_view>())
        throw std::invalid_argument("Managed-bank receipt does not select the current trusted execution branch");
}

std::shared_ptr<OwnedManagedBudgetLease> InMemoryCheckpointStore::acquire_managed_budget_lease(
    const ManagedBudgetLeaseScope& scope, const std::string& id, const std::string& commitment) {
    std::lock_guard lock(mutex_);
    const auto key = detail::ManagedBudgetJournalAccess::storage_key(scope);
    std::string execution_key = key;
    std::string execution_thread = scope.thread_id;
    if (!id.empty()) {
        const auto source = by_id_.find(id);
        if (source == by_id_.end())
            throw std::invalid_argument("Managed-bank source checkpoint is missing");
        auto complete_source = join_blobs_locked(source->second);
        const auto& branch = authenticate_managed_budget_branch_locked(complete_source, commitment);
        if (branch.financial_storage_thread_id != key)
            throw std::invalid_argument("Managed-bank branch changed its original financial namespace");
        execution_key = complete_source.thread_id;
        execution_thread = branch.execution_thread_id;
    }
    const auto found = managed_budget_journals_.find(key);
    json candidate = found == managed_budget_journals_.end() ? json() : found->second;
    if (!id.empty()) detail::ManagedBudgetJournalAccess::select_branch_head(candidate, id, commitment);
    auto lease = detail::ManagedBudgetJournalAccess::acquire_branch(candidate,
        managed_budget_obligations_.contains(key), scope, id, commitment, execution_thread, execution_key);
    managed_budget_obligations_.insert(key);
    managed_budget_journals_[key] = std::move(candidate);
    detail::ManagedBudgetJournalAccess::refresh(lease, managed_budget_journals_.at(key));
    detail::ManagedBudgetJournalAccess::bind_cpp_native_retention(lease, managed_budget_journals_.at(key));
    return lease;
}
ManagedBudgetEffectReceipt InMemoryCheckpointStore::begin_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& id,
    std::uint64_t amount, const std::string& digest) {
    if (!lease) throw std::invalid_argument("Managed-bank effect requires an owned lease");
    std::lock_guard lock(mutex_);
    const auto key = detail::ManagedBudgetJournalAccess::storage_key(lease->scope());
    const auto head = managed_budget_journals_.find(key);
    if (head == managed_budget_journals_.end()) throw std::invalid_argument("Managed-bank journal is missing");
    validate_managed_budget_execution_locked(lease, head->second);
    detail::ManagedBudgetJournalAccess::bind_cpp_native_retention(lease, head->second);
    auto candidate = head->second;
    const auto effect_key = std::make_tuple(key, lease->bank_generation(), id);
    const auto found = managed_budget_effects_.find(effect_key);
    json effect = found == managed_budget_effects_.end() ? json() : found->second;
    auto receipt = detail::ManagedBudgetJournalAccess::begin(candidate, effect, lease, id, amount, digest);
    managed_budget_effects_[effect_key] = std::move(effect);
    head->second = std::move(candidate);
    detail::ManagedBudgetJournalAccess::refresh(lease, head->second);
    return receipt;
}
void InMemoryCheckpointStore::settle_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const ManagedBudgetEffectReceipt& receipt,
    sp::runtime::Result outcome, const UsageAccumulator::AuthoritySnapshot& authority) {
    if (!lease || !receipt.active()) throw std::invalid_argument("Managed-bank settlement requires owned custody");
    std::lock_guard lock(mutex_);
    const auto key = detail::ManagedBudgetJournalAccess::storage_key(lease->scope());
    const auto head = managed_budget_journals_.find(key);
    const auto effect_key = std::make_tuple(key, lease->bank_generation(), receipt.effect_id());
    const auto found = managed_budget_effects_.find(effect_key);
    if (head == managed_budget_journals_.end() || found == managed_budget_effects_.end())
        throw std::invalid_argument("Managed-bank settlement lacks its original journal effect");
    validate_managed_budget_execution_locked(lease, head->second);
    auto candidate = head->second;
    auto effect = found->second;
    detail::ManagedBudgetJournalAccess::settle(candidate, effect, lease, receipt, outcome, authority);
    // Real immutable C++ outcome retains native seals when no archive exists.
    // Financial JSON is not a public native replay import.
    managed_budget_results_[effect_key] = std::move(outcome);
    found->second = std::move(effect);
    head->second = std::move(candidate);
    detail::ManagedBudgetJournalAccess::refresh(lease, head->second);
}
void InMemoryCheckpointStore::publish_managed_budget_checkpoint(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint) {
    if (!lease) throw std::invalid_argument("Managed-bank checkpoint requires an owned lease");
    std::lock_guard lock(mutex_);
    const auto key = detail::ManagedBudgetJournalAccess::storage_key(lease->scope());
    const auto head = managed_budget_journals_.find(key);
    if (head == managed_budget_journals_.end()) throw std::invalid_argument("Managed-bank journal is missing");
    if (by_id_.contains(checkpoint.id)) throw std::invalid_argument("Managed-bank checkpoint ID has already been used");
    const auto execution_key = lease->execution_storage_thread_id();
    const auto branch = managed_budget_branch_heads_.find(execution_key);
    if (!lease->head_checkpoint_id().empty()) {
        const auto source = by_id_.find(lease->head_checkpoint_id());
        if (source == by_id_.end())
            throw std::invalid_argument("Managed-bank publication source was removed");
        const auto& current = authenticate_managed_budget_branch_locked(
            join_blobs_locked(source->second), lease->head_commitment());
        if (current.financial_storage_thread_id != key ||
            current.execution_thread_id != lease->execution_thread_id() ||
            source->second.thread_id != execution_key)
            throw std::invalid_argument("Managed-bank publication changed its trusted execution branch");
    } else if (branch != managed_budget_branch_heads_.end() ||
               by_thread_.contains(execution_key) || execution_key != key) {
        throw std::invalid_argument("Managed-bank first publication target is not a fresh original branch");
    }
    auto candidate = head->second;
    detail::ManagedBudgetJournalAccess::publish(candidate, lease, checkpoint);
    auto shell = split_blobs_locked(checkpoint);
    if (managed_budget_checkpoint_commitment(join_blobs_locked(shell)) !=
        managed_budget_checkpoint_commitment(checkpoint))
        throw std::invalid_argument("Managed-bank checkpoint cannot preserve its complete source state");
    ManagedBudgetBranchHead next_branch{key, lease->execution_thread_id(), checkpoint.id,
        candidate.at("head_commitment").get<std::string>(), checkpoint.native_history, true};
    const bool existing_branch = branch != managed_budget_branch_heads_.end();
    bool created_history = false;
    bool inserted_checkpoint = false;
    bool inserted_branch = false;
    try {
        auto [history, created] = by_thread_.try_emplace(execution_key);
        created_history = created;
        by_id_.emplace(checkpoint.id, shell);
        inserted_checkpoint = true;
        if (!existing_branch) {
            managed_budget_branch_heads_.emplace(execution_key, std::move(next_branch));
            inserted_branch = true;
        }
        history->second.push_back(std::move(shell));
    } catch (...) {
        if (inserted_branch) managed_budget_branch_heads_.erase(execution_key);
        if (inserted_checkpoint) by_id_.erase(checkpoint.id);
        if (created_history) by_thread_.erase(execution_key);
        throw;
    }
    if (existing_branch) branch->second = std::move(next_branch);
    head->second = std::move(candidate);
    detail::ManagedBudgetJournalAccess::refresh(lease, head->second);
}

void InMemoryCheckpointStore::publish_managed_budget_fork(
    const Checkpoint& source, const Checkpoint& forked) {
    std::lock_guard lock(mutex_);
    const auto commitment = managed_budget_checkpoint_commitment(source);
    const auto& source_branch = authenticate_managed_budget_branch_locked(source, commitment);
    const auto head = managed_budget_journals_.find(source_branch.financial_storage_thread_id);
    if (head == managed_budget_journals_.end())
        throw std::invalid_argument("Managed-bank fork source has no original financial journal");
    auto candidate = head->second;
    detail::ManagedBudgetJournalAccess::select_branch_head(candidate, source.id, commitment);
    const auto execution_thread =
        detail::ManagedBudgetJournalAccess::validate_shared_bank_fork(candidate, source, forked);
    const auto pending_target = pending_.lower_bound({forked.thread_id, {}});
    if (by_id_.contains(forked.id) || by_thread_.contains(forked.thread_id) ||
        managed_budget_obligations_.contains(forked.thread_id) ||
        managed_budget_journals_.contains(forked.thread_id) ||
        managed_budget_branch_heads_.contains(forked.thread_id) ||
        (pending_target != pending_.end() && pending_target->first.first == forked.thread_id))
        throw std::invalid_argument("Managed-bank fork target namespace or checkpoint ID is not fresh");

    // Keep this one structural checkpoint inline until its first real branch
    // publication, so a failed fork cannot leave partial target channel blobs.
    auto shell = forked;
    const auto fork_commitment = managed_budget_checkpoint_commitment(forked);
    ManagedBudgetBranchHead branch{source_branch.financial_storage_thread_id, execution_thread,
        forked.id, fork_commitment, forked.native_history, true};
    // All real authority records commit under this store lock. If allocation
    // fails, no target head/alias/obligation is left authoritative.
    try {
        by_id_.emplace(shell.id, shell);
        by_thread_.emplace(forked.thread_id, std::vector<Checkpoint>{std::move(shell)});
        managed_budget_branch_heads_.emplace(forked.thread_id, std::move(branch));
        managed_budget_obligations_.insert(forked.thread_id);
    } catch (...) {
        by_id_.erase(forked.id);
        by_thread_.erase(forked.thread_id);
        managed_budget_branch_heads_.erase(forked.thread_id);
        managed_budget_obligations_.erase(forked.thread_id);
        throw;
    }
    // Structural fork grants no currency: the original financial generation,
    // actor/revision/frontier and source branch head are deliberately unchanged.
}
void InMemoryCheckpointStore::release_managed_budget_lease(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    if (!lease) throw std::invalid_argument("Managed-bank release requires an owned lease");
    std::lock_guard lock(mutex_);
    const auto key = detail::ManagedBudgetJournalAccess::storage_key(lease->scope());
    const auto head = managed_budget_journals_.find(key);
    if (head == managed_budget_journals_.end()) throw std::invalid_argument("Managed-bank journal is missing");
    validate_managed_budget_execution_locked(lease, head->second);
    auto candidate = head->second;
    detail::ManagedBudgetJournalAccess::release(candidate, lease);
    head->second = std::move(candidate);
    detail::ManagedBudgetJournalAccess::refresh(lease, head->second);
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
