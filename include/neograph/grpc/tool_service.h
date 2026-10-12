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

#include <asio/awaitable.hpp>

namespace grpc { class Service; }

namespace neograph::grpc {

/// name → fn(arguments_json) → result_json. Throwing from the fn
/// surfaces in ToolCallResponse.error with its stable code in
/// ToolCallResponse.error_code ("NG.TaskFailed", "NG.HookBlocked",
/// "NG.Timeout", ...), not as a gRPC transport error; a cancellation is the
/// CANCELLED status. See the reference, "Protocol failure mapping".
using ToolFn = std::function<std::string(const std::string&)>;

/// Build the ToolService as a generated service base, so embedders can
/// register it with their own grpc::ServerBuilder (TLS, auth, ports).
NEOGRAPH_API std::unique_ptr<::grpc::Service> make_tool_service(
    std::unordered_map<std::string, ToolFn> tools);

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
/// transport error — same contract as run_tool_server's ToolFn); its
/// error_code is not mapped back to a local control signal.
///
/// pimpl so this public header stays grpc++/protobuf-free for
/// consumers (same posture as GrpcCheckpointStore).
class NEOGRAPH_API GrpcRemoteTool : public neograph::Tool,
                                     public neograph::ContextualAsyncTool {
public:
    /// target e.g. "tool-host:50091". Insecure channel.
    GrpcRemoteTool(const std::string& target,
                   std::string name,
                   std::string description,
                   neograph::json parameters);
    ~GrpcRemoteTool() override;

    ChatTool get_definition() const override;
    std::string execute(const neograph::json& arguments) override;
    /// What every dispatcher calls: the run's cancel token cancels the RPC in
    /// flight (graph::CancelledException) and its deadline becomes the RPC
    /// deadline (asio::system_error(asio::error::timed_out)), as for MCPTool.
    asio::awaitable<std::string> execute_async(
        const neograph::json& arguments, neograph::ToolExecutionContext context) override;
    std::string get_name() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
