/**
 * @file a2a/harness_backend.h
 * @brief Downstream A2A capability adapter for Harness workers.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/mcp/harness.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace neograph::a2a {

class A2AClient;

/// Resolve tool executor.agent against preconfigured A2A clients.
///
/// A call ends when the job's cancel token is cancelled, and, unless
/// @p call_timeout is zero, when the agent has not answered within
/// @p call_timeout: the connection is closed and the executor throws. The
/// default is the 30 s that these calls had while the client's default bounded
/// every exchange. A client's own `set_timeout` applies as well, whichever ends
/// the call first.
NEOGRAPH_API mcp::HarnessCapabilityExecutor make_harness_capability_executor(
    std::map<std::string, std::shared_ptr<A2AClient>> agents,
    std::chrono::seconds call_timeout = std::chrono::seconds(30));

} // namespace neograph::a2a
