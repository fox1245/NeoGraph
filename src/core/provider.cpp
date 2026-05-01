/**
 * @file provider.cpp
 * @brief Out-of-line default implementations of Provider::complete /
 *        complete_async — the sync↔async crossover bridge.
 *
 * Stage 3 / Semester 2.1. Kept in a dedicated TU so the asio coroutine
 * instantiation cost is paid once (here) rather than at every include
 * of provider.h.
 */
#include <neograph/provider.h>
#include <neograph/async/run_sync.h>

namespace neograph {

ChatCompletion Provider::complete(const CompletionParams& params) {
    return neograph::async::run_sync(complete_async(params));
}

asio::awaitable<ChatCompletion>
Provider::complete_async(const CompletionParams& params) {
    co_return complete(params);
}

asio::awaitable<ChatCompletion>
Provider::complete_stream_async(const CompletionParams& params,
                                const StreamCallback& on_chunk) {
    // Default bridge: runs the synchronous streaming impl inline.
    // This blocks the awaiting coroutine's executor for the full
    // duration of the stream — fine for short streams or when the
    // executor has multiple worker threads, but suboptimal under
    // contention. Subclasses with a native async streaming transport
    // (e.g. OpenAI WebSocket Responses) SHOULD override this with a
    // genuine non-blocking implementation that posts tokens on the
    // executor and resumes the coroutine on the final chunk.
    co_return complete_stream(params, on_chunk);
}

} // namespace neograph
