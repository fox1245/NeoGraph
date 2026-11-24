/**
 * @file process_generation.h
 * @brief Internal: tells a store's copy in a child made by fork from the store itself.
 *
 * A process has a generation: a number that a fork handler changes in every child made by fork and that nothing else
 * changes. A store records the generation when it opens its database connection; in a process whose generation is
 * another one, the store is the copy a child inherited, and it must neither use the connection nor release it (the
 * database libraries allow neither in a process other than the one that opened the connection).
 *
 * Reading the generation is one relaxed atomic load and no system call, so a store checks it on every operation. A
 * platform without fork (Windows, WebAssembly) registers no handler, and its generation never changes.
 *
 * Exported for the other NeoGraph libraries that own a database connection; no compatibility promise.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/store_opened_in_another_process.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace neograph::detail {

/// The generation of this process: one relaxed atomic load, no system call.
NEOGRAPH_API std::uint64_t process_generation() noexcept;

/// Registers the fork handler that changes the generation, once per process (the first call does it), and returns the
/// generation. A store calls it when it opens its connection. Throws std::system_error when the platform could not
/// register the handler; every later call throws the same.
NEOGRAPH_API std::uint64_t register_process_generation();

/// The generation a store recorded when it opened its connection. It is the first member of the object that holds
/// the connection, so it is recorded before the connection is opened.
class OpenedInProcess {
public:
    OpenedInProcess() : generation_(register_process_generation()) {}

    /// Throws StoreOpenedInAnotherProcess, naming `store`, in a process other than the one that opened the store.
    /// Every operation calls it before it touches the connection or takes the store's lock.
    void check(std::string_view store) const {
        if (generation_ != process_generation()) [[unlikely]]
            throw StoreOpenedInAnotherProcess(store);
    }

    /// True in a process other than the one that opened the store: the store is a copy that a child made by fork
    /// inherited. Its destructor then calls nothing that would touch the parent's connection.
    bool inherited() const noexcept { return generation_ != process_generation(); }

private:
    std::uint64_t generation_;
};

/// The destructor's step for a store whose implementation object holds the connection and an OpenedInProcess named
/// `opened`: in a child's copy the object is released without being destroyed, so no call reaches the database library
/// and no lock is taken; what the copy holds stays as it is until the child execs or exits. In the process that opened
/// the store nothing happens here, and the owner of `impl` destroys it as before.
template <class Impl>
void release_if_inherited(std::unique_ptr<Impl>& impl) noexcept {
    if (impl && impl->opened.inherited()) static_cast<void>(impl.release());
}

}  // namespace neograph::detail
