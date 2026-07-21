/**
 * @file a2a/harness_backend.h
 * @brief Downstream A2A capability adapter for Harness workers.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/mcp/harness.h>

#include <map>
#include <memory>
#include <string>

namespace neograph::a2a {

class A2AClient;

/// Resolve tool executor.agent against preconfigured A2A clients.
NEOGRAPH_API mcp::HarnessCapabilityExecutor make_harness_capability_executor(
    std::map<std::string, std::shared_ptr<A2AClient>> agents);

} // namespace neograph::a2a
