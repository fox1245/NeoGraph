#pragma once

/**
 * @file postgres_pipeline.h
 * @brief libpq pipeline mode for the PostgreSQL Program transition store.
 *
 * `PgPipeline` owns the pipeline mode of one connection: it enters the mode, puts the connection in nonblocking mode,
 * sends statements without waiting for their answers, and reads every answer up to one synchronization point. Its
 * destructor always leaves the connection idle, out of pipeline mode and blocking again, on every path: after the
 * answers were read, after an exception while sending, and after an exception while reading. No other code calls
 * PQenterPipelineMode.
 *
 * A write that cannot be flushed while the server is writing answers must not deadlock against it: the flush loop
 * waits for the socket to be writable or readable and consumes input whenever it is readable (the pattern libpq's
 * documentation prescribes for nonblocking connections).
 */

#include <libpq-fe.h>

#include <cerrno>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace neograph::program::detail {

struct PipelineResultDeleter {
    void operator()(PGresult* result) const noexcept {
        if (result) PQclear(result);
    }
};
using PipelineResult = std::unique_ptr<PGresult, PipelineResultDeleter>;

/// Whether this build and the libpq it runs against have pipeline mode (libpq 14 or later).
inline bool pipeline_mode_supported() noexcept {
#ifdef LIBPQ_HAS_PIPELINING
    return PQlibVersion() >= 140000;
#else
    return false;
#endif
}

#ifdef LIBPQ_HAS_PIPELINING
class PgPipeline final {
public:
    /// Enters pipeline mode on an idle connection and switches it to nonblocking mode.
    explicit PgPipeline(PGconn* connection, bool* fail_leave = nullptr)
        : connection_(connection), fail_leave_(fail_leave) {
        if (PQenterPipelineMode(connection_) != 1) fail("PostgreSQL pipeline could not be entered");
        entered_ = true;
        if (PQsetnonblocking(connection_, 1) != 0) {
            leave();  // a constructor that throws runs no destructor
            fail("PostgreSQL connection could not be made nonblocking");
        }
    }
    PgPipeline(const PgPipeline&) = delete;
    PgPipeline& operator=(const PgPipeline&) = delete;
    ~PgPipeline() { leave(); }

    /// Queues one statement. Nothing waits for its answer.
    void send(const std::string& sql, const std::vector<const char*>& values) {
        if (PQsendQueryParams(connection_, sql.c_str(), static_cast<int>(values.size()), nullptr,
                              values.empty() ? nullptr : values.data(), nullptr, nullptr, 0) != 1)
            fail("PostgreSQL pipeline could not queue a statement");
        ++queued_;
    }

    /// Ends the pipeline with one synchronization point, sends everything, and returns one result per queued statement
    /// in order. A statement that failed is its own result; every statement after it carries PGRES_PIPELINE_ABORTED.
    std::vector<PipelineResult> finish() {
        if (PQpipelineSync(connection_) != 1) fail("PostgreSQL pipeline could not be synchronized");
        synced_ = true;
        flush();
        std::vector<PipelineResult> results;
        results.reserve(queued_);
        for (std::size_t index = 0; index < queued_; ++index) {
            PipelineResult result(PQgetResult(connection_));
            if (!result) fail("PostgreSQL pipeline lost an answer");
            PipelineResult end(PQgetResult(connection_));  // each statement's answer is followed by a null result
            if (end) fail("PostgreSQL pipeline answer was not terminated");
            results.push_back(std::move(result));
        }
        PipelineResult sync(PQgetResult(connection_));
        if (!sync || PQresultStatus(sync.get()) != PGRES_PIPELINE_SYNC)
            fail("PostgreSQL pipeline did not reach its synchronization point");
        sync_seen_ = true;
        leave();
        return results;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        std::string message(what);
        message += ": ";
        if (const auto* detail = PQerrorMessage(connection_)) message += detail;
        throw std::runtime_error(std::move(message));
    }

    // Sends the buffered statements. While the output cannot be flushed, waits until the socket is writable or readable
    // and reads whatever the server has sent, so that a large parameter cannot block against the server's answers.
    void flush() const {
        for (;;) {
            const int flushed = PQflush(connection_);
            if (flushed == 0) return;
            if (flushed < 0) fail("PostgreSQL pipeline could not be flushed");
            const auto socket = PQsocket(connection_);
            if (socket < 0) fail("PostgreSQL connection has no socket");
#ifdef _WIN32
            WSAPOLLFD wait{static_cast<SOCKET>(socket), POLLRDNORM | POLLWRNORM, 0};
            const int ready = WSAPoll(&wait, 1, -1);
            const bool readable = ready > 0 && (wait.revents & (POLLRDNORM | POLLERR | POLLHUP));
#else
            pollfd wait{socket, POLLIN | POLLOUT, 0};
            const int ready = ::poll(&wait, 1, -1);
            if (ready < 0 && errno == EINTR) continue;
            const bool readable = ready > 0 && (wait.revents & (POLLIN | POLLERR | POLLHUP));
#endif
            if (ready < 0) fail("PostgreSQL pipeline could not wait for its socket");
            if (readable && PQconsumeInput(connection_) != 1) fail("PostgreSQL pipeline could not read its answers");
        }
    }

    // Leaves the pipeline: reads and drops what is still owed, exits pipeline mode, makes the connection blocking again.
    // Never throws; a connection that cannot be restored is reset, and one that cannot be reset fails every later statement, as a dropped connection always did.
    void leave() noexcept {
        if (!entered_) return;
        try {
            if (!synced_ && PQpipelineSync(connection_) == 1) synced_ = true;
            if (synced_ && !sync_seen_) {
                flush();
                // Every queued statement owes a result and a null result after it; the synchronization point ends them.
                for (std::size_t owed = 0; owed < 2 * queued_ + 2 && !sync_seen_; ++owed) {
                    PGresult* result = PQgetResult(connection_);
                    if (!result) continue;
                    sync_seen_ = PQresultStatus(result) == PGRES_PIPELINE_SYNC;
                    PQclear(result);
                }
            }
        } catch (...) {
        }
        if (fail_leave_ && *fail_leave_) {
            *fail_leave_ = false;
            entered_     = false;
            return;
        }
        if (PQexitPipelineMode(connection_) != 1) {
            // A null result may still be owed after the synchronization point.
            while (PGresult* result = PQgetResult(connection_)) PQclear(result);
            // Answers are still owed that cannot be read (the connection dropped, or a send or a flush failed): the
            // session is started over, so the connection is neither left in pipeline mode nor inside a transaction.
            if (PQexitPipelineMode(connection_) != 1) PQreset(connection_);
        }
        (void)PQsetnonblocking(connection_, 0);
        entered_ = false;
    }

    PGconn*     connection_;
    bool*       fail_leave_ = nullptr;
    std::size_t queued_     = 0;
    bool        entered_    = false;
    bool        synced_     = false;
    bool        sync_seen_  = false;
};
#endif  // LIBPQ_HAS_PIPELINING

}  // namespace neograph::program::detail
