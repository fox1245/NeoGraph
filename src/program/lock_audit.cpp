#include "lock_audit.h"

#if NEOGRAPH_PROGRAM_LOCK_AUDIT_ENABLED

#include <cstdio>
#include <cstdlib>

#if defined(__linux__) && __has_include(<execinfo.h>)
#include <execinfo.h>
#define NEOGRAPH_LOCK_AUDIT_BACKTRACE 1
#endif

namespace neograph::program::detail {

unsigned& pure_locks_held() noexcept {
    thread_local unsigned held = 0;
    return held;
}

void assert_no_pure_lock_held(const char* call) noexcept {
    if (pure_locks_held() == 0) return;
    std::fprintf(stderr,
                 "NeoGraph Program lock audit: %s was called while this thread holds %u mutex(es) that guard "
                 "memory only. A critical section that reaches a store uses an OperationLock.\n",
                 call, pure_locks_held());
#ifdef NEOGRAPH_LOCK_AUDIT_BACKTRACE
    void* frames[64];
    const int count = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, count, 2);
#endif
    std::abort();
}

}  // namespace neograph::program::detail

#endif
