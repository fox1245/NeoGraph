/**
 * @file mcp/harness_mcp_backend.h
 * @brief Downstream MCP capability adapter for Harness workers.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/mcp/harness.h>

#include <map>
#include <memory>
#include <string>

namespace neograph::mcp {

class MCPClient;
class HardenedMcpClientRegistry;

/// Resolve tool executor.server_ref against preconfigured, initialized clients.
NEOGRAPH_API HarnessCapabilityExecutor make_mcp_harness_capability_executor(
    std::map<std::string, std::shared_ptr<MCPClient>> clients);

/// Use the policy-bound local adoption registry without duplicating client ownership.
NEOGRAPH_API HarnessCapabilityExecutor make_mcp_harness_capability_executor_from_registry(
    std::shared_ptr<HardenedMcpClientRegistry> registry);

} // namespace neograph::mcp
