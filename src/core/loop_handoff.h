// The way back from a thread of the process's pools to the coroutine of a HostLoop that awaits it (#605).
//
// A tool that blocks a thread (execute_blocking_tool_async), a process (execute_process_tool_async) or a synchronous
// checkpoint store runs on a pool of the process and hands its result back by posting to the executor of the run that
// awaits it. When that executor belongs to a HostLoop, the host may destroy the loop while the work is still running,
// and it cannot join the pool. Posting from the pool thread then touches an io_context that is gone, and so does
// releasing a work guard or a strand it holds. Executors that are not run by a HostLoop do not use a LoopHandoff.
//
// A LoopHandoff owns the continuation (`resume`, which owns the awaiting coroutine) and the executor it is posted to,
// for the whole time the pool thread works:
//
//   - finish() posts the continuation under the loop's gate while the io_context is alive, requests a wake outside the
//     gate that ~HostLoop() waits for before returning, and releases its reference to the loop: once finish() returns,
//     the handoff holds nothing of the loop's and never calls the host executor again.
//   - A loop that is closed while the work is still running takes the continuation, and destroys it on the host's
//     thread, before the io_context it ran on: the coroutine is destroyed as the coroutines that the loop holds are,
//     and the result of the work is dropped when it arrives. finish() then only releases its reference to the loop
//     (whose io_context and host executor reference are already gone). A handoff destroyed without finish() hands an
//     untaken continuation back to the loop for the host thread to destroy and requests a turn if the loop is still
//     open.
#pragma once

#include <neograph/api.h>

#include <asio/any_io_executor.hpp>

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace neograph::async::detail {

class HostLoopCore;

/// A callable that is moved, never copied, so that it can own a coroutine's completion handler.
class Resume {
public:
    Resume() = default;
    template <typename Function,
              typename = std::enable_if_t<!std::is_same_v<std::decay_t<Function>, Resume>>>
    Resume(Function function) : callable_(std::make_unique<Model<Function>>(std::move(function))) {}
    Resume(Resume&&) noexcept = default;
    Resume& operator=(Resume&&) noexcept = default;
    void operator()() { (*callable_)(); }
    explicit operator bool() const noexcept { return callable_ != nullptr; }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void operator()() = 0;
    };
    template <typename Function>
    struct Model final : Concept {
        explicit Model(Function function) : function_(std::move(function)) {}
        void operator()() override { function_(); }
        Function function_;
    };
    std::unique_ptr<Concept> callable_;
};

class LoopHandoff {
public:
    /// On the thread that runs the awaiting coroutine, before the work is handed to the pool. `executor` must be
    /// run by a HostLoop (`is_host_pumped(executor)`). `resume` runs on `executor` once, from finish(), and owns the
    /// coroutine's completion handler.
    static NEOGRAPH_API std::shared_ptr<LoopHandoff> open(const asio::any_io_executor& executor, Resume resume);

    LoopHandoff(const LoopHandoff&) = delete;
    LoopHandoff& operator=(const LoopHandoff&) = delete;
    /// Hands an unfinished continuation (the pool discarded the work) back to the loop for the host thread to destroy.
    NEOGRAPH_API ~LoopHandoff();

    /// From the pool thread, once, after the result is stored where `resume` reads it. Posts the continuation if the
    /// loop has not taken it over, and releases the handoff's reference to the loop. Touches nothing of the loop after
    /// it returns.
    NEOGRAPH_API void finish() noexcept;

    /// The number of LoopHandoff instances currently alive across the process.
    static NEOGRAPH_API std::size_t active_count() noexcept;

private:
    friend class HostLoopCore;
    LoopHandoff(asio::any_io_executor executor, Resume resume, std::shared_ptr<HostLoopCore> core);

    enum class State : unsigned char { Open, Posted, Taken };

    // Everything below is the loop's gate to guard.
    std::shared_ptr<HostLoopCore> core_;
    asio::any_io_executor executor_;
    Resume resume_;
    State state_ = State::Open;
};

}  // namespace neograph::async::detail
