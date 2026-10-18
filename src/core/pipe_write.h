#pragma once

// Writes to a child process's stdin pipe that fail with EPIPE instead of
// raising SIGPIPE (#495). Once the child has stopped reading, a write to
// the pipe raises SIGPIPE, and its default action ends the whole process.
//
// - macOS raises it for the process, not the writing thread (XNU's
//   dofilewrite calls psignal on the process), so any thread that does not
//   block SIGPIPE receives it. F_SETNOSIGPIPE on the write end turns the
//   signal off for that open file, every dup of it included.
// - Linux raises it for the writing thread. write_without_sigpipe() blocks
//   it on this thread around the write and takes back the one the write
//   raised.

#if !defined(_WIN32)

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

namespace neograph::detail {

/// Mark the write end of a pipe so that a write to it never raises SIGPIPE,
/// where the platform can (macOS). Call it before handing the descriptor
/// out; elsewhere it does nothing and write_without_sigpipe() does the work.
inline void no_sigpipe_on(int fd) noexcept {
#if defined(F_SETNOSIGPIPE)
    (void)::fcntl(fd, F_SETNOSIGPIPE, 1);
#else
    (void)fd;
#endif
}

/// ::write(), except that a reader that is gone gives EPIPE without a
/// SIGPIPE, on a descriptor marked with no_sigpipe_on().
inline ssize_t write_without_sigpipe(int fd, const void* data, std::size_t size) noexcept {
#if defined(F_SETNOSIGPIPE)
    return ::write(fd, data, size);
#else
    sigset_t pipe_signal;
    sigemptyset(&pipe_signal);
    sigaddset(&pipe_signal, SIGPIPE);
    // A SIGPIPE already pending for this thread belongs to someone else.
    sigset_t pending;
    sigemptyset(&pending);
    const bool was_pending = ::sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
    sigset_t previous;
    if (::pthread_sigmask(SIG_BLOCK, &pipe_signal, &previous) != 0) return ::write(fd, data, size);
    const ssize_t written = ::write(fd, data, size);
    const int     error   = errno;
    if (written < 0 && error == EPIPE && !was_pending) {
        const timespec now{0, 0};
        while (::sigtimedwait(&pipe_signal, nullptr, &now) < 0 && errno == EINTR) {
        }
    }
    (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = error;
    return written;
#endif
}

}  // namespace neograph::detail

#endif  // !_WIN32
