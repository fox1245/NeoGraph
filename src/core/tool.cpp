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

// Default async entry: run legacy synchronous work on the shared bounded
// blocking executor. Copy arguments before the first suspension: callers are
// allowed to pass a temporary JSON value to execute_async().
asio::awaitable<std::string> Tool::execute_async(const json& arguments) {
    co_return co_await detail::execute_blocking_tool_async(*this, json(arguments));
}

std::string AsyncTool::execute(const json& arguments) {
    return neograph::async::run_sync(execute_async(arguments));
}

} // namespace neograph
