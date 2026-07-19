/**
 * @file provider.cpp
 * @brief Out-of-line default implementations of Provider::complete /
 *        complete_async / complete_stream_async — the sync↔async
 *        crossover bridges.
 *
 * Stage 3 / Semester 2.1. Kept in a dedicated TU so the asio coroutine
 * instantiation cost is paid once (here) rather than at every include
 * of provider.h.
 */
#include <neograph/provider.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>

#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace neograph {

ChatCompletion Provider::complete(const CompletionParams& params) {
    // v1.0 (9d): the legacy `current_cancel_token()` thread_local
    // fallback is gone. Callers must pin `params.cancel_token` if they
    // need cancel propagation through `run_sync`.
    auto* tok = params.cancel_token ? params.cancel_token.get() : nullptr;
    return neograph::async::run_sync(complete_async(params), tok);
}

asio::awaitable<ChatCompletion>
Provider::complete_async(const CompletionParams& params) {
    co_return complete(params);
}

ChatCompletion Provider::complete_stream(const CompletionParams& params,
                                         const StreamCallback& on_chunk) {
    // Issue #22: default body so mocks / test fixtures / non-streaming
    // demos don't have to stub out a four-line override that does
    // exactly this. Streaming-native subclasses (OpenAI / schema-driven
    // / OpenInference) override this to emit incremental tokens.
    auto result = complete(params);
    if (on_chunk && !result.message.content.empty()) {
        on_chunk(result.message.content);
    }
    return result;
}

asio::awaitable<ChatCompletion>
Provider::complete_stream_async(const CompletionParams& params,
                                const StreamCallback& on_chunk) {
    // Issue #4 fix: previous implementation was `co_return
    // complete_stream(params, on_chunk)`, which blocked the awaiting
    // coroutine's executor for the full duration of the stream. With
    // an outer engine driven by `GraphEngine::run_stream_async` on the
    // caller's `io_context`, that meant the engine's worker thread
    // was suspended inside a single httplib `Post` callback while the
    // user's `on_chunk` fired on the same thread mid-coroutine — a
    // reentrancy/race surface that segfaulted on `SchemaProvider`'s
    // shared state. For the WebSocket Responses path it was worse:
    // `complete_stream` itself called `run_sync(...)`, so a fresh
    // io_context was nested on top of the engine's io_context worker
    // thread.
    //
    // New default: spawn a dedicated worker thread to run the
    // synchronous `complete_stream`. The worker only writes to shared
    // state; the awaiting coroutine polls and drains queued tokens on
    // its own executor. This keeps `on_chunk` single-threaded with the
    // awaiter without letting the worker retain or use that executor
    // after its io_context begins teardown.
    //
    // Subclasses with a fully async streaming transport (e.g.
    // SchemaProvider's WebSocket Responses path) SHOULD override this
    // and skip the thread spawn entirely — see
    // `SchemaProvider::complete_stream_async`.
    auto exec = co_await asio::this_coro::executor;

    struct Shared {
        std::mutex mutex;
        bool abandoned = false;
        bool finished = false;
        std::vector<std::string> chunks;
        std::optional<ChatCompletion> result;
        std::exception_ptr err;
    };
    auto shared = std::make_shared<Shared>();

    StreamCallback wrapped = [shared](const std::string& chunk) {
        std::lock_guard lock(shared->mutex);
        if (!shared->abandoned) shared->chunks.push_back(chunk);
    };

    struct AbandonGuard {
        std::shared_ptr<Shared> shared;
        ~AbandonGuard() {
            std::lock_guard lock(shared->mutex);
            shared->abandoned = true;
            shared->chunks.clear();
        }
    } abandon_guard{shared};

    // params is captured by value so the worker thread doesn't
    // outlive the caller's stack-allocated CompletionParams. The
    // CancelToken inside params (shared_ptr) keeps its target alive
    // through the worker.
    std::thread([this, params, wrapped, shared]() mutable {
        std::optional<ChatCompletion> result;
        std::exception_ptr err;
        try {
            result = this->complete_stream(params, wrapped);
        } catch (...) {
            err = std::current_exception();
        }
        {
            std::lock_guard lock(shared->mutex);
            shared->result = std::move(result);
            shared->err = err;
            shared->finished = true;
        }
    }).detach();

    asio::steady_timer poll(exec);
    for (;;) {
        std::vector<std::string> chunks;
        std::optional<ChatCompletion> result;
        std::exception_ptr err;
        bool finished = false;
        {
            std::lock_guard lock(shared->mutex);
            chunks.swap(shared->chunks);
            finished = shared->finished;
            if (finished) {
                result = std::move(shared->result);
                err = shared->err;
            }
        }

        if (on_chunk) {
            for (const auto& chunk : chunks) on_chunk(chunk);
        }
        if (finished) {
            if (err) std::rethrow_exception(err);
            co_return std::move(*result);
        }

        poll.expires_after(std::chrono::milliseconds(1));
        asio::error_code ec;
        co_await poll.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
}

// Compatibility callback-selected entry point. Existing providers keep
// working because this default forwards to the stable 4-virtual chain.
// New providers should derive from CompletionProvider, where explicit
// CompletionRequest mode selection reaches one do_invoke() override.
//
// v1.0 (9d): invoke() default is now the simple single-dispatch
// forward — `params.cancel_token` is the only cancel channel. Engine-
// internal nodes stamp `params.cancel_token = in.ctx.cancel_token`
// before calling invoke(); user code passes their own token. The
// legacy `current_cancel_token()` thread_local fallback is gone with
// `CurrentCancelTokenScope` (see 9d).
asio::awaitable<ChatCompletion>
Provider::invoke(const CompletionParams& params, StreamCallback on_chunk) {
    if (on_chunk) {
        co_return co_await complete_stream_async(params, on_chunk);
    }
    co_return co_await complete_async(params);
}

} // namespace neograph
