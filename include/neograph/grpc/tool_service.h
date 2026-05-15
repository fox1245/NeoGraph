// neograph::grpc — single-tool-call gRPC service + remote-tool client.
//
// Two mirror halves of the same `ToolService.CallTool` RPC:
//
//   * run_tool_server  — SERVE local C++ functions over gRPC (the fair
//     gRPC counterpart of MCP's JSON-RPC 2.0 `tools/call`; example 55).
//   * GrpcRemoteTool    — CONSUME a remote ToolService method as an
//     ordinary neograph::Tool, so an Agent/ReAct loop can call a tool
//     that lives in another process/language with no code change on
//     the agent side (example 57). Ported from NexaGraph's GrpcTool.
//
// Opt-in (NEOGRAPH_BUILD_GRPC). Whole header behind NEOGRAPH_HAVE_GRPC
// so the umbrella include never pulls grpc++.

#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <neograph/api.h>   // NEOGRAPH_API
#include <neograph/tool.h>  // neograph::Tool, ChatTool, json

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

/// A neograph::Tool whose execute() round-trips to a remote
/// ToolService.CallTool over gRPC. Drop it into an Agent's tool list
/// like any local Tool — the agent never knows the work runs in
/// another process.
///
/// The simple ToolService proto has no "list tools" RPC, so the
/// definition (name / description / parameter schema the LLM sees) is
/// supplied at construction. A non-empty ToolCallResponse.error from
/// the server is rethrown as std::runtime_error (a tool error, not a
/// transport error — same contract as run_tool_server's ToolFn).
///
/// pimpl so this public header stays grpc++/protobuf-free for
/// consumers (same posture as GrpcCheckpointStore).
class NEOGRAPH_API GrpcRemoteTool : public neograph::Tool {
public:
    /// target e.g. "tool-host:50091". Insecure channel.
    GrpcRemoteTool(const std::string& target,
                   std::string name,
                   std::string description,
                   neograph::json parameters);
    ~GrpcRemoteTool() override;

    ChatTool get_definition() const override;
    std::string execute(const neograph::json& arguments) override;
    std::string get_name() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
