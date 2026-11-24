// An object with threads, owned by one process of this program.
//
// The binding keeps objects that own threads (the runtime that drives `run_async`, the pools that run Python tool and
// node bodies). A child made by fork inherits a copy of such an object and none of its threads. The copy is of no use
// to the child, and touching it is harmful: joining a thread that is not the child's own, waiting for work that only
// that thread can finish, or taking a lock that a thread of the parent held at the fork. Destroying it does all of
// these (a thread object that is still joinable ends the process in its destructor).
//
// ProcessOwned<T> holds the one T of the process that created it:
//  * get() returns that T and creates it on first use. In a child made by fork the T the parent created is another
//    process's: it is left as it is, never used, never destroyed, and the child creates one of its own, on threads of
//    its own, the first time it needs one. Nothing in the inherited T is read, so no lock of it is taken.
//  * current() returns the T of this process, or nothing: the exit hook stops these, and only these.
//  * Destroying the holder (static destruction at exit) destroys the T of this process as before and releases a
//    parent's without destroying it.
//
// The process is told from its parent by the generation of src/core/process_generation.h: a fork handler changes it in
// every child, and reading it is one atomic load. The slot records the generation when it is created, so the
// handler is registered before the first T exists. A platform without fork never changes it.
//
// Nothing here takes a lock: a slot is installed with one compare-and-exchange, and a thread that loses the race
// destroys the T it made (it is its own and has not been shared).

#pragma once

#include "core/process_generation.h"

#include <atomic>
#include <memory>
#include <utility>

namespace neograph::pybind {

template <typename T>
class ProcessOwned {
public:
    ProcessOwned() = default;
    ProcessOwned(const ProcessOwned&) = delete;
    ProcessOwned& operator=(const ProcessOwned&) = delete;

    ~ProcessOwned() {
        std::unique_ptr<Slot> slot(slot_.exchange(nullptr, std::memory_order_acq_rel));
        // The copy a child inherited is released, not destroyed.
        if (slot && slot->opened.inherited()) static_cast<void>(slot.release());
    }

    /// The T of this process; `make()` returns a std::unique_ptr<T> and runs when this process has none yet.
    template <typename Make>
    T& get(Make&& make) {
        Slot* seen = slot_.load(std::memory_order_acquire);
        if (seen && !seen->opened.inherited()) return *seen->value;
        auto fresh = std::make_unique<Slot>(std::forward<Make>(make));
        Slot* expected = seen;
        while (!slot_.compare_exchange_weak(expected, fresh.get(), std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            // Someone installed one meanwhile: use it when it is this process's, otherwise replace it.
            if (expected && !expected->opened.inherited()) return *expected->value;
        }
        // The slot that was replaced (a parent's) stays where it is, unreferenced: it is not destroyed.
        return *fresh.release()->value;
    }

    /// The T of this process, or null when this process has not created one.
    T* current() const noexcept {
        Slot* slot = slot_.load(std::memory_order_acquire);
        return slot && !slot->opened.inherited() ? slot->value.get() : nullptr;
    }

private:
    struct Slot {
        template <typename Make>
        explicit Slot(Make&& make) : value(std::forward<Make>(make)()) {}

        // First member: the generation is recorded (and the fork handler registered) before the T exists.
        neograph::detail::OpenedInProcess opened;
        std::unique_ptr<T>      value;
    };

    std::atomic<Slot*> slot_{nullptr};
};

}  // namespace neograph::pybind
