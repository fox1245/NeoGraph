// neograph::grpc — single-tool-call gRPC service.
//
// The fair gRPC counterpart of MCP's JSON-RPC 2.0 `tools/call`, used
// by example 55's head-to-head bench. Opt-in (NEOGRAPH_BUILD_GRPC).

#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <functional>
#include <string>
#include <unordered_map>

#include <neograph/api.h>   // NEOGRAPH_API

namespace neograph::grpc {

/// name → fn(arguments_json) → result_json. Throwing from the fn
/// surfaces in ToolCallResponse.error (not as a gRPC transport error).
using ToolFn = std::function<std::string(const std::string&)>;

/// Build + run a blocking gRPC ToolService on `address`
/// ("0.0.0.0:50091") serving `tools`, until the process is signalled.
/// Insecure credentials.
NEOGRAPH_API void run_tool_server(
    const std::string& address,
    std::unordered_map<std::string, ToolFn> tools);

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
