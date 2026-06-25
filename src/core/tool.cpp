/**
 * @file tool.cpp
 * @brief Out-of-line definition of AsyncTool::execute (Sem 4.2).
 *
 * Kept in its own TU so the run_sync template instantiation cost is
 * paid here once, not at every include of <neograph/tool.h>.
 */
#include <neograph/tool.h>
#include <neograph/async/run_sync.h>

namespace neograph {

// Default async entry: bridge to the sync execute(). Tools whose work
// is naturally blocking get a valid awaitable for free; the call runs
// to completion before the first suspension, so `arguments` stays
// valid. I/O-bound tools override execute_async() for real overlap.
asio::awaitable<std::string> Tool::execute_async(const json& arguments) {
    co_return execute(arguments);
}

std::string AsyncTool::execute(const json& arguments) {
    return neograph::async::run_sync(execute_async(arguments));
}

} // namespace neograph
