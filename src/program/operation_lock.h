/**
 * @file operation_lock.h
 * @brief Internal. A lock for an operation that stays inside it across store I/O and across a suspension (#557).
 *
 * A std::mutex must never be held across a co_await, and a store call is where a coroutine suspends once the stores
 * are awaitable. An OperationLock is the lock for a critical section that contains either. It is not recursive and it
 * grants in the order of the requests. It is taken in two ways:
 *
 *   - `co_await lock.lock_async()` suspends the coroutine and never blocks a thread. The lock is handed over by a
 *     post to the waiter's executor, so a waiter never runs inside the releasing call.
 *   - `lock.lock_blocking()` blocks the calling thread, for the synchronous API. In a build without threads nothing
 *     can release the lock while the caller waits, so a contended request is refused.
 *
 * A wait is not interruptible: a coroutine that is cancelled while it waits for `lock_async` still takes its turn,
 * and releases the lock when it leaves its scope. Keep a critical section short enough for that.
 *
 * `std::mutex` remains for state that is pure memory: nothing in its critical section calls a store or suspends
 * (see lock_audit.h).
 */
#pragma once

#include <neograph/build_config.h>
#include <neograph/program/runtime.h>

#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_executor.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace neograph::program::detail {

class OperationLock {
public:
    /// Releases the lock when it is destroyed or when `unlock` is called.
    class Guard {
    public:
        Guard() noexcept = default;
        Guard(Guard&& other) noexcept : lock_(std::exchange(other.lock_, nullptr)) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                unlock();
                lock_ = std::exchange(other.lock_, nullptr);
            }
            return *this;
        }
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { unlock(); }

        bool owns() const noexcept { return lock_ != nullptr; }
        void unlock() noexcept {
            if (auto* lock = std::exchange(lock_, nullptr)) lock->release();
        }

    private:
        friend class OperationLock;
        explicit Guard(OperationLock* lock) noexcept : lock_(lock) {}
        OperationLock* lock_ = nullptr;
    };

    OperationLock() = default;
    OperationLock(const OperationLock&)            = delete;
    OperationLock& operator=(const OperationLock&) = delete;

    /// The lock, or nothing if it is held or someone is waiting for it.
    std::optional<Guard> try_lock() {
        std::lock_guard state(state_);
        if (locked_ || !queue_.empty()) return std::nullopt;
        locked_ = true;
        return Guard(this);
    }

    /// Blocks the calling thread until the lock is its. Do not call it from a coroutine that must not block its
    /// thread; use lock_async there.
    Guard lock_blocking() {
        std::unique_lock state(state_);
        if (!locked_ && queue_.empty()) {
            locked_ = true;
            return Guard(this);
        }
        if (!NEOGRAPH_BUILD_THREADS)
            throw ProgramBlockingCallRefused(
                "ProgramRuntime", {},
                "a synchronous call needs a lock that an operation suspended on this thread's executor holds");
        auto waiter = std::make_shared<Waiter>();
        queue_.push_back(waiter);
        waiter->ready.wait(state, [&] { return waiter->granted; });
        return Guard(this);
    }

    /// Suspends until the lock is the caller's. The lock may be released on any thread.
    asio::awaitable<Guard> lock_async() {
        {
            std::lock_guard state(state_);
            if (!locked_ && queue_.empty()) {
                locked_ = true;
                co_return Guard(this);
            }
        }
        const auto executor = co_await asio::this_coro::executor;
        auto       token    = asio::bind_executor(executor, asio::use_awaitable);
        co_await asio::async_initiate<decltype(token), void()>(
            [this, executor](auto handler) {
                using Handler    = std::decay_t<decltype(handler)>;
                auto completion  = std::make_shared<Handler>(std::move(handler));
                bool granted_now = false;
                {
                    std::lock_guard state(state_);
                    if (!locked_ && queue_.empty()) {
                        locked_     = true;
                        granted_now = true;
                    } else {
                        auto waiter    = std::make_shared<Waiter>();
                        waiter->resume = [executor, completion]() mutable {
                            asio::post(executor, [completion] { (*completion)(); });
                        };
                        queue_.push_back(std::move(waiter));
                    }
                }
                if (granted_now) asio::post(executor, [completion] { (*completion)(); });
            },
            token);
        co_return Guard(this);
    }

private:
    struct Waiter {
        bool                    granted = false;
        std::condition_variable ready;
        /// Set for an awaiting coroutine; empty for a blocked thread.
        std::function<void()>   resume;
    };

    void release() noexcept {
        std::shared_ptr<Waiter> next;
        {
            std::lock_guard state(state_);
            if (queue_.empty()) {
                locked_ = false;
                return;
            }
            next = std::move(queue_.front());
            queue_.pop_front();
            next->granted = true;  // the lock stays held: it passes to `next`
        }
        if (next->resume)
            next->resume();
        else
            next->ready.notify_one();
    }

    std::mutex                         state_;
    bool                               locked_ = false;
    std::deque<std::shared_ptr<Waiter>> queue_;
};

/**
 * An OperationLock that the thread holding it through lock_blocking can take again, as a std::recursive_mutex could
 * be. The runtime's process-wide transition lock is one: cancel, replace and migrate_graph nest. A holder that
 * took it with lock_async is not bound to a thread and must not call a function that takes it with lock_blocking.
 */
class ReentrantOperationLock {
public:
    class Guard {
    public:
        Guard() noexcept = default;
        Guard(Guard&& other) noexcept : lock_(std::exchange(other.lock_, nullptr)) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                unlock();
                lock_ = std::exchange(other.lock_, nullptr);
            }
            return *this;
        }
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { unlock(); }

        bool owns() const noexcept { return lock_ != nullptr; }
        void unlock() noexcept {
            if (auto* lock = std::exchange(lock_, nullptr)) lock->release();
        }

    private:
        friend class ReentrantOperationLock;
        explicit Guard(ReentrantOperationLock* lock) noexcept : lock_(lock) {}
        ReentrantOperationLock* lock_ = nullptr;
    };

    Guard lock_blocking() {
        const auto self = std::this_thread::get_id();
        if (owner_.load(std::memory_order_relaxed) == self) {
            ++depth_;
            return Guard(this);
        }
        held_ = lock_.lock_blocking();
        owner_.store(self, std::memory_order_relaxed);
        depth_ = 1;
        return Guard(this);
    }

    asio::awaitable<Guard> lock_async() {
        held_  = co_await lock_.lock_async();
        depth_ = 1;
        co_return Guard(this);
    }

private:
    void release() noexcept {
        if (--depth_ != 0) return;
        owner_.store(std::thread::id{}, std::memory_order_relaxed);
        held_.unlock();
    }

    OperationLock                lock_;
    OperationLock::Guard         held_;  // the underlying lock, while depth_ > 0; touched only by the holder
    std::atomic<std::thread::id> owner_{};
    unsigned                     depth_ = 0;
};

/**
 * A family of OperationLocks, one per exact key, with no sharing between keys. An entry exists while at least one
 * caller holds or waits for its key and is erased when the last of them leaves, so the table is empty when nothing is
 * in flight. The table is guarded by a std::mutex that is pure memory: it is never held across a store call or a
 * suspension, only to find, create or erase an entry. The grants of one key are those of an OperationLock: in the
 * order of the requests, and a cancelled waiter still takes its turn and releases.
 */
class KeyedOperationLock {
public:
    struct Key {
        std::string coordination;
        std::string owner_scope;
        std::string logical_run_id;

        bool operator<(const Key& other) const noexcept {
            return std::tie(coordination, owner_scope, logical_run_id) <
                   std::tie(other.coordination, other.owner_scope, other.logical_run_id);
        }
    };

private:
    struct Entry {
        OperationLock lock;
        std::size_t   users = 0;  // callers that hold or wait for the key; guarded by State::mutex
    };

    struct State {
        std::mutex                            mutex;
        std::map<Key, std::shared_ptr<Entry>> entries;

        /// One more caller for `key`; the entry is created for the first.
        std::shared_ptr<Entry> join(const Key& key) {
            std::lock_guard lock(mutex);
            auto&           slot = entries[key];
            if (!slot) slot = std::make_shared<Entry>();
            ++slot->users;
            return slot;
        }

        /// The caller leaves `key`; the entry is erased with the last of them.
        void leave(const Key& key) noexcept {
            std::lock_guard lock(mutex);
            const auto      found = entries.find(key);
            if (found != entries.end() && --found->second->users == 0) entries.erase(found);
        }
    };

    /// A caller's place in the table from `join` until it holds the lock; it leaves the table again if the caller
    /// never gets as far (a coroutine that is destroyed or fails while it waits).
    class Membership {
    public:
        Membership(std::shared_ptr<State> state, Key key) : state_(std::move(state)), key_(std::move(key)) {
            entry_ = state_->join(key_);
        }
        Membership(const Membership&)            = delete;
        Membership& operator=(const Membership&) = delete;
        ~Membership() {
            if (state_) state_->leave(key_);
        }
        const std::shared_ptr<Entry>& entry() const noexcept { return entry_; }
        /// The caller now holds the lock; the guard takes over the place.
        std::tuple<std::shared_ptr<State>, Key, std::shared_ptr<Entry>> release() noexcept {
            return {std::move(state_), std::move(key_), std::move(entry_)};
        }

    private:
        std::shared_ptr<State> state_;
        Key                    key_;
        std::shared_ptr<Entry> entry_;
    };

public:
    /// Releases the key's lock, then the caller's place in the table, when it is destroyed or when `unlock` is called.
    class Guard {
    public:
        Guard() noexcept = default;
        Guard(Guard&& other) noexcept
            : state_(std::move(other.state_)),
              key_(std::move(other.key_)),
              entry_(std::move(other.entry_)),
              held_(std::move(other.held_)) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                unlock();
                state_ = std::move(other.state_);
                key_   = std::move(other.key_);
                entry_ = std::move(other.entry_);
                held_  = std::move(other.held_);
            }
            return *this;
        }
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { unlock(); }

        bool owns() const noexcept { return held_.owns(); }
        void unlock() noexcept {
            held_.unlock();
            entry_.reset();
            if (auto state = std::move(state_)) state->leave(key_);
        }

    private:
        friend class KeyedOperationLock;
        Guard(std::shared_ptr<State> state, Key key, std::shared_ptr<Entry> entry, OperationLock::Guard held) noexcept
            : state_(std::move(state)), key_(std::move(key)), entry_(std::move(entry)), held_(std::move(held)) {}

        std::shared_ptr<State> state_;
        Key                    key_;
        std::shared_ptr<Entry> entry_;
        OperationLock::Guard   held_;
    };

    KeyedOperationLock() : state_(std::make_shared<State>()) {}
    KeyedOperationLock(const KeyedOperationLock&)            = delete;
    KeyedOperationLock& operator=(const KeyedOperationLock&) = delete;

    /// Blocks the calling thread until the lock of `key` is its.
    Guard lock_blocking(Key key) {
        Membership member(state_, std::move(key));
        auto       held = member.entry()->lock.lock_blocking();
        auto [state, joined, entry] = member.release();
        return Guard(std::move(state), std::move(joined), std::move(entry), std::move(held));
    }

    /// Suspends until the lock of `key` is the caller's. The lock may be released on any thread.
    asio::awaitable<Guard> lock_async(Key key) {
        Membership member(state_, std::move(key));
        auto       held = co_await member.entry()->lock.lock_async();
        auto [state, joined, entry] = member.release();
        co_return Guard(std::move(state), std::move(joined), std::move(entry), std::move(held));
    }

    /// The number of keys that are held or waited for.
    std::size_t size_for_test() const {
        std::lock_guard lock(state_->mutex);
        return state_->entries.size();
    }

private:
    std::shared_ptr<State> state_;
};

}  // namespace neograph::program::detail
