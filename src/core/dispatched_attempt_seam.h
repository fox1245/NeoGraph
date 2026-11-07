#pragma once

// Test seams of the dispatched node attempts (graph_executor.cpp, "Dispatched attempts"). An attempt is released on a
// worker of the process-wide attempt pool, after its run has been woken and possibly after the run has ended.

#include <neograph/api.h>

#include <string_view>

namespace neograph::graph::detail {

/// Test seam: when set, called with the run id on the worker that releases a dispatched attempt whose body has
/// finished, right before the release, so a test can hold the release until it lets it go. Null outside tests.
using DispatchedAttemptReleaseObserver = void (*)(std::string_view run_id) noexcept;
NEOGRAPH_API void set_dispatched_attempt_release_observer_for_testing(
    DispatchedAttemptReleaseObserver observer) noexcept;

/// Test seam: when set, called with the run id by the orchestrator of a dispatched attempt right after it has decided
/// that the attempt timed out (outside the attempt's lock), so a test can order its steps after the timeout instead of
/// after a delay. Null outside tests.
using DispatchedAttemptTimeoutObserver = void (*)(std::string_view run_id) noexcept;
NEOGRAPH_API void set_dispatched_attempt_timeout_observer_for_testing(
    DispatchedAttemptTimeoutObserver observer) noexcept;

}  // namespace neograph::graph::detail
