/**
 * @file core/cancel.cpp
 * @brief Out-of-line storage for the thread-local current cancel token.
 *
 * The thread_local lives in this TU (one definition across the shared
 * library) so multiple translation units can read/write it via the
 * inline helpers in ``graph/cancel.h`` without ODR pain.
 */
#include <neograph/graph/cancel.h>

namespace neograph::graph {

namespace {
thread_local CancelToken* tls_current = nullptr;
}

CancelToken* current_cancel_token() noexcept {
    return tls_current;
}

CurrentCancelTokenScope::CurrentCancelTokenScope(CancelToken* tok) noexcept
    : prev_(tls_current) {
    tls_current = tok;
}

CurrentCancelTokenScope::~CurrentCancelTokenScope() noexcept {
    tls_current = prev_;
}

}  // namespace neograph::graph
