// The binding's one asio runtime, and the hop of an asio coroutine's result onto an asyncio future. The graph
// engine's `*_async` methods (bind_graph.cpp) and the provider's `complete_endpoint_async`
// (bind_provider_endpoints.cpp) spawn their coroutines on it, so a process runs one worker thread for both.
#pragma once

#include "interpreter_exit.h"
#include "json_bridge.h"
#include "provider_bridge.h"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>

namespace neograph::pybind {

// Singleton asio runtime that drives all async calls: the engine's `run_async`
// family and `Provider.complete_endpoint_async`. One io_context, one worker
// thread, work-guard prevents premature exit even when there are no
// coroutines in flight.
//
// Why one shared runtime instead of per-engine: keeps the binding
// simple, and the engine itself is thread-safe across distinct
// thread_ids. CPU-bound parallel fan-out within a run is still
// served by the engine's own thread_pool (set_worker_count), so
// the AsyncRuntime mostly just orchestrates I/O suspension points.
//
// At interpreter exit the module's atexit hook closes the runtime
// (close_async_runtime): it stops accepting runs, lets the handler that is
// running finish and joins the worker thread, all before finalization starts.
// A worker still alive then would take the GIL while the main thread
// finalizes, which makes it exit by force and crashes the process
// (interpreter_exit.h). ~AsyncRuntime does the same for a process that never
// ran the hook, without any Python call.
class AsyncRuntime {
public:
    static AsyncRuntime& instance() {
        static AsyncRuntime r;
        return r;
    }

    // The executor that drives the runs. Throws once the exit hook has closed
    // the runtime, including when no run had started before.
    static asio::io_context::executor_type executor() {
        if (closed().load(std::memory_order_acquire))
            throw std::runtime_error(
                "the Python interpreter is exiting: no run can start any more");
        return instance().io_.get_executor();
    }

    // Stops accepting runs and, if the runtime was ever started, stops the
    // thread that drives them. The handler it is running finishes (it may need
    // the GIL, so the caller must not hold it); handlers that have not started
    // are dropped.
    static void close_if_started() {
        closed().store(true, std::memory_order_release);
        if (auto* runtime = started().load(std::memory_order_acquire)) runtime->close();
    }

private:
    static std::atomic<AsyncRuntime*>& started() {
        static std::atomic<AsyncRuntime*> pointer{nullptr};
        return pointer;
    }
    static std::atomic<bool>& closed() {
        static std::atomic<bool> flag{false};
        return flag;
    }

    AsyncRuntime()
        : guard_(asio::make_work_guard(io_)),
          worker_([this]() { io_.run(); }) {
        started().store(this, std::memory_order_release);
    }

    ~AsyncRuntime() {
        started().store(nullptr, std::memory_order_release);
        close();
    }

    void close() {
        guard_.reset();
        io_.stop();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }

    AsyncRuntime(const AsyncRuntime&) = delete;
    AsyncRuntime& operator=(const AsyncRuntime&) = delete;

    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::thread worker_;
};

// Retain asyncio objects in shared owners. Copies on the Asio worker do not
// touch Python reference counts; the final deleter acquires the GIL.
inline std::shared_ptr<py::object> hold_py(py::object obj) {
    return std::shared_ptr<py::object>(new py::object(std::move(obj)),
                                       &delete_with_gil<py::object>);
}

// Drain whatever the asio coroutine produced (RunResult or exception)
// onto the captured asyncio.Future. Runs on the asio worker thread;
// hops back to the asyncio loop thread via call_soon_threadsafe so
// `Future.set_result` runs where asyncio expects.
//
// Two failure modes guarded:
//   1. Loop closed — is_closed() short-circuits before scheduling.
//      call_soon_threadsafe on a closed loop raises RuntimeError on
//      the worker thread; we'd lose the GIL guarantee, so eat early.
//   2. Future cancelled (or already resolved) — set_result/set_exception
//      raise InvalidStateError, but the raise happens later on the
//      asyncio loop thread (call_soon_threadsafe just schedules), so
//      no C++ try/catch can catch it. The asyncio default handler
//      logs the unhandled exception to stderr — visible as a flood of
//      "Exception in callback ... InvalidStateError: invalid state"
//      under any UI that cancels long streams (FastAPI SSE + frontend
//      AbortController is the textbook case).
//
//      Fix: route through `_safe_set_future_result` /
//      `_safe_set_future_exception` Python helpers (defined via
//      m.def in init_graph, bind_graph.cpp), which `if not fut.done():` guard
//      before calling set_result/set_exception. The helpers run on
//      the loop thread under its GIL, exactly where the original
//      raise would have happened.
template <typename T>
void resolve_future_async(std::shared_ptr<py::object> future,
                          std::shared_ptr<py::object> loop,
                          std::exception_ptr& eptr,
                          T&& result) {
    py::gil_scoped_acquire g;
    // error_already_set owns Python references; release the completion
    // callback's copy before gil_scoped_acquire leaves scope.
    struct ExceptionPtrReset {
        std::exception_ptr& ptr;
        ~ExceptionPtrReset() { ptr = nullptr; }
    } reset{eptr};
    try {
        if (loop->attr("is_closed")().template cast<bool>()) return;

        py::module_ mod =
            py::module_::import("neograph_engine._neograph");

        if (eptr) {
            py::object exc = provider_cause(eptr);
            loop->attr("call_soon_threadsafe")(
                mod.attr("_safe_set_future_exception"),
                *future, exc);
        } else {
            loop->attr("call_soon_threadsafe")(
                mod.attr("_safe_set_future_result"),
                *future, py::cast(std::forward<T>(result)));
        }
    } catch (const py::error_already_set&) {
        // Loop probably racing into closure — nothing we can do
        // without breaking a Python guarantee. Eat the error.
        PyErr_Clear();
    }
}


} // namespace neograph::pybind
