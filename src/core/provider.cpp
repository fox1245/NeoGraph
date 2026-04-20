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

} // namespace neograph
