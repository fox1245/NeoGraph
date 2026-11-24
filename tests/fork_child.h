// Runs a callable in a child made by fork, for tests of what a child does with the objects it inherited.
#pragma once

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__wasi__)
#define NEOGRAPH_TESTS_HAVE_FORK 1

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>

#include <csignal>
#endif

#include <cerrno>
#include <cstdlib>
#include <ostream>
#include <system_error>
#include <utility>

namespace neograph::test {

/// How a child made by run_in_forked_child() ended.
struct ForkChildEnd {
    /// It left through std::_Exit with `status`; otherwise a signal (`signal`) ended it.
    bool exited = false;
    int  status = 0;
    int  signal = 0;

    bool ok() const noexcept { return exited && status == 0; }
};

inline std::ostream& operator<<(std::ostream& out, const ForkChildEnd& end) {
    if (end.exited) return out << "the child exited with status " << end.status;
    return out << "the child was ended by signal " << end.signal;
}

/// The child's status when the callable threw.
inline constexpr int kForkChildThrew = 125;
/// The child's status when its parent had gone before the child could tie its own end to the parent's.
inline constexpr int kForkChildOrphaned = 126;

/// Makes a child by fork and runs `body` (a callable returning an exit status) in it. The child never returns into
/// the test runner: it leaves through std::_Exit with the status `body` returns, or kForkChildThrew when `body`
/// throws, so neither the test framework nor a static destructor runs there. `body` reports through its status and
/// does not use the framework's assertions.
///
/// The parent waits for the child with no bound of its own: a child that does not end is ended together with its
/// test by the test's TIMEOUT property (on Linux the child is killed when the thread that made it ends).
template <class Body>
ForkChildEnd run_in_forked_child(Body&& body) {
    const pid_t parent = ::getpid();
    const pid_t child  = ::fork();
    if (child < 0) throw std::system_error(errno, std::generic_category(), "fork");
    if (child == 0) {
#if defined(__linux__)
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent) std::_Exit(kForkChildOrphaned);
#else
        static_cast<void>(parent);
#endif
        int status = kForkChildThrew;
        try {
            status = std::forward<Body>(body)();
        } catch (...) {
        }
        std::_Exit(status);
    }
    int raw = 0;
    while (::waitpid(child, &raw, 0) < 0) {
        if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "waitpid");
    }
    ForkChildEnd end;
    if (WIFEXITED(raw)) {
        end.exited = true;
        end.status = WEXITSTATUS(raw);
    } else if (WIFSIGNALED(raw)) {
        end.signal = WTERMSIG(raw);
    }
    return end;
}

}  // namespace neograph::test

#endif
