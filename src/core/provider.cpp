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

#include <asio/dispatch.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <thread>

namespace neograph {

ChatCompletion Provider::complete(const CompletionParams& params) {
    // v0.3: thread cancel propagation through the sync path. The
    // engine sets a thread-local CancelToken before invoking each
    // node, so a node calling provider.complete() picks it up
    // automatically. Caller can still pin params.cancel_token
    // explicitly to override (e.g. share an abort across threads).
    auto* tok = params.cancel_token
                    ? params.cancel_token.get()
                    : neograph::graph::current_cancel_token();
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
    // synchronous `complete_stream`, and dispatch each token onto the
    // *awaiter's* executor so the caller's `on_chunk` runs single-
    // threaded with the coroutine that issued the await — same
    // invariant the engine already gives node bodies. Completion is
    // signalled via a one-shot `steady_timer.cancel()` posted onto
    // the awaiter's executor.
    //
    // Subclasses with a fully async streaming transport (e.g.
    // SchemaProvider's WebSocket Responses path) SHOULD override this
    // and skip the thread spawn entirely — see
    // `SchemaProvider::complete_stream_async`.
    auto exec = co_await asio::this_coro::executor;

    struct Shared {
        std::optional<ChatCompletion> result;
        std::exception_ptr err;
    };
    auto shared = std::make_shared<Shared>();

    // expires_at::max + cancel() == one-shot completion signal. Same
    // pattern graph_executor.cpp uses for retry-wait timers.
    auto done = std::make_shared<asio::steady_timer>(exec);
    done->expires_at(std::chrono::steady_clock::time_point::max());

    // Wrap the user's on_chunk so each chunk is dispatched onto the
    // awaiter's executor (not the worker thread). Restores the
    // single-threaded-with-the-awaiter invariant.
    StreamCallback wrapped = [exec, on_chunk](const std::string& chunk) {
        asio::dispatch(exec, [on_chunk, chunk]() { on_chunk(chunk); });
    };

    // params is captured by value so the worker thread doesn't
    // outlive the caller's stack-allocated CompletionParams. The
    // CancelToken inside params (shared_ptr) keeps its target alive
    // through the worker.
    std::thread([this, params, wrapped, shared, done, exec]() mutable {
        try {
            shared->result = this->complete_stream(params, wrapped);
        } catch (...) {
            shared->err = std::current_exception();
        }
        // Wake the awaiter on its executor.
        asio::dispatch(exec, [done]() { done->cancel(); });
    }).detach();

    // Suspend until the worker fires cancel(). async_wait completes
    // with operation_aborted on cancel — we don't care about the ec
    // value, only that we resumed.
    asio::error_code ec;
    co_await done->async_wait(asio::redirect_error(asio::use_awaitable, ec));

    if (shared->err) std::rethrow_exception(shared->err);
    co_return std::move(*shared->result);
}

// v1.0 unified invoke() — additive virtual that future-proofs the
// Provider dispatch surface. New providers override THIS one method;
// legacy providers (OpenAIProvider's complete_async + complete_stream_async
// overrides, SchemaProvider's same pair, RateLimitedProvider's 4-method
// delegation, every user-written Provider subclass) keep working
// unchanged because the default body below forwards to the legacy
// 4-virtual chain — preserving the current cross-product fallback.
//
// Subsequent Candidate 6 PRs migrate native subclasses to override
// invoke() directly; the 4 legacy virtuals then gain [[deprecated]]
// markers; v1.0 deletes them. See ROADMAP_v1.md Candidate 6.
//
// No recursion guard needed in this PR: the 4 legacy virtuals' defaults
// don't yet route back through invoke(), so the chain terminates at
// either complete_async (which co_returns complete()) or
// complete_stream_async (which spawns a worker thread). When a future
// PR flips the legacy defaults to forward INTO invoke() — closing the
// loop — a guard analogous to ExecuteDefaultGuard (graph_node.cpp) will
// be added then, not now.
asio::awaitable<ChatCompletion>
Provider::invoke(const CompletionParams& params, StreamCallback on_chunk) {
    if (on_chunk) {
        co_return co_await complete_stream_async(params, on_chunk);
    }
    co_return co_await complete_async(params);
}

} // namespace neograph
