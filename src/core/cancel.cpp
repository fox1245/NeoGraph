/**
 * @file core/cancel.cpp
 *
 * v1.0 (9d): the thread-local `current_cancel_token()` /
 * `CurrentCancelTokenScope` smuggling channel is gone. This TU is
 * intentionally empty — kept so existing CMakeLists / build outputs
 * referencing `cancel.cpp.o` don't need a touch-up alongside the 9d
 * cleanup. Future cleanup may remove it.
 */
#include <neograph/graph/cancel.h>
