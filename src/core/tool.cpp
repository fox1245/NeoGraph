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

std::string AsyncTool::execute(const json& arguments) {
    return neograph::async::run_sync(execute_async(arguments));
}

} // namespace neograph
