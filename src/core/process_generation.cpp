#include "process_generation.h"

#include <atomic>
#include <string>
#include <system_error>

// A platform without fork registers no handler, and the generation never changes there.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__wasi__)
#define NEOGRAPH_PROCESS_GENERATION_HAS_FORK 1
#include <pthread.h>
#endif

namespace neograph {

StoreOpenedInAnotherProcess::StoreOpenedInAnotherProcess(std::string_view store)
    : std::runtime_error(std::string(store) +
                         " was opened in another process: a child made by fork opens a new store of its own "
                         "instead of using the one it inherited") {}

StoreOpenedInAnotherProcess::~StoreOpenedInAnotherProcess() = default;

namespace detail {
namespace {

std::atomic<std::uint64_t> generation{0};

#if defined(NEOGRAPH_PROCESS_GENERATION_HAS_FORK)
// The handler runs in the child right after fork, in a process that may have had several threads: it does one
// lock-free atomic step and nothing else.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the fork handler changes the generation without a lock");

void after_fork_in_child() noexcept { generation.fetch_add(1, std::memory_order_relaxed); }

pthread_once_t registration       = PTHREAD_ONCE_INIT;
int            registration_error = 0;

void register_fork_handler() noexcept {
    registration_error = pthread_atfork(nullptr, nullptr, &after_fork_in_child);
}
#endif

}  // namespace

std::uint64_t process_generation() noexcept { return generation.load(std::memory_order_relaxed); }

std::uint64_t register_process_generation() {
#if defined(NEOGRAPH_PROCESS_GENERATION_HAS_FORK)
    if (const int status = pthread_once(&registration, &register_fork_handler); status != 0)
        throw std::system_error(status, std::generic_category(), "cannot register the fork handler");
    if (registration_error != 0)
        throw std::system_error(registration_error, std::generic_category(), "cannot register the fork handler");
#endif
    return process_generation();
}

}  // namespace detail
}  // namespace neograph
