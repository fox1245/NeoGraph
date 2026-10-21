/**
 * @file lock_audit.h
 * @brief Internal. The rule that a mutex of the Program runtime guards memory only, checked (#557).
 *
 * The runtime's `std::mutex`es protect in-memory state and nothing else: no store is read or written, and no
 * coroutine suspends, while one is held. A critical section that does either uses an OperationLock
 * (operation_lock.h), which a coroutine can wait for without blocking its thread.
 *
 * With NEOGRAPH_PROGRAM_LOCK_AUDIT defined (Debug builds, and any build configured with
 * -DNEOGRAPH_PROGRAM_LOCK_AUDIT=ON) PureMutex counts the locks the calling thread holds, and the transition store
 * the runtime was given is wrapped (audit_transition_store) so that every call into it aborts with a message when
 * the count is not zero. Otherwise PureMutex is std::mutex and nothing is checked or wrapped.
 */
#pragma once

#include <neograph/program/transition_store.h>

#include <condition_variable>
#include <memory>
#include <mutex>

#if defined(NEOGRAPH_PROGRAM_LOCK_AUDIT) && NEOGRAPH_PROGRAM_LOCK_AUDIT
#define NEOGRAPH_PROGRAM_LOCK_AUDIT_ENABLED 1
#else
#define NEOGRAPH_PROGRAM_LOCK_AUDIT_ENABLED 0
#endif

namespace neograph::program::detail {

#if NEOGRAPH_PROGRAM_LOCK_AUDIT_ENABLED

/// The number of PureMutex locks the calling thread holds.
unsigned& pure_locks_held() noexcept;

/// Aborts, naming `call`, when the calling thread holds a PureMutex.
void assert_no_pure_lock_held(const char* call) noexcept;

class PureMutex {
public:
    void lock() {
        mutex_.lock();
        ++pure_locks_held();
    }
    bool try_lock() {
        if (!mutex_.try_lock()) return false;
        ++pure_locks_held();
        return true;
    }
    void unlock() {
        --pure_locks_held();
        mutex_.unlock();
    }

private:
    std::mutex mutex_;
};
using PureConditionVariable = std::condition_variable_any;

/// A store whose every call first asserts that no PureMutex is held, then forwards to `inner`.
std::shared_ptr<ProgramTransitionStore> audit_transition_store(std::shared_ptr<ProgramTransitionStore> inner);

#else

using PureMutex             = std::mutex;
using PureConditionVariable = std::condition_variable;
inline void assert_no_pure_lock_held(const char*) noexcept {}
inline std::shared_ptr<ProgramTransitionStore> audit_transition_store(
    std::shared_ptr<ProgramTransitionStore> inner) {
    return inner;
}

#endif

}  // namespace neograph::program::detail
