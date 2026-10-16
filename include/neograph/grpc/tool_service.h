// neograph::grpc — single-tool-call gRPC service + remote-tool client.
//
// Two mirror halves of the same `ToolService.CallTool` RPC:
//
//   * run_tool_server  — SERVE local C++ functions over gRPC (the fair
//     gRPC counterpart of MCP's JSON-RPC 2.0 `tools/call`; example 55).
//   * GrpcRemoteTool    — CONSUME a remote ToolService method as an
//     ordinary neograph::Tool, so an Agent/ReAct loop can call a tool
//     that lives in another process/language with no code change on
//     the agent side (example 57).
//
// Opt-in (NEOGRAPH_BUILD_GRPC). Whole header behind NEOGRAPH_HAVE_GRPC
// so the umbrella include never pulls grpc++.
//
// make_tool_service() returns a std::unique_ptr<::grpc::Service>, which the
// caller destroys: the header therefore includes the gRPC service base
// (<grpcpp/impl/service_type.h>), and including it alone is enough to build
// and destroy the service. The generated neograph.grpc.pb.h is not needed.

#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// ::grpc::Service, complete for the destructor of the returned unique_ptr.
#if __has_include(<grpcpp/impl/service_type.h>)
#include <grpcpp/impl/service_type.h>
#else
#include <grpcpp/impl/codegen/service_type.h>
#endif

#include <neograph/api.h>   // NEOGRAPH_API
#include <neograph/grpc/serve_options.h>
#include <neograph/tool.h>  // neograph::Tool, ChatTool, json

#include <asio/awaitable.hpp>

namespace neograph::grpc {

/// name → fn(arguments_json) → result_json. A failure the fn throws is a
/// tool error in the payload, like MCP's isError: ToolCallResponse.error holds
/// its public message (never what()) and error_code its stable code
/// ("NG.TaskFailed", "NG.Timeout", an author code, ...). A run control signal
/// is a status instead: CANCELLED, or FAILED_PRECONDITION (DEADLINE_EXCEEDED
/// for a timed-out Hook) with ErrorInfo NG.HookBlocked or NG.Interrupted. An
/// unknown tool is NOT_FOUND, arguments that are not a JSON object
/// INVALID_ARGUMENT. See the reference, "Protocol failure mapping".
using ToolFn = std::function<std::string(const std::string&)>;

/// Build the ToolService as a generated service base, so embedders can
/// register it with their own grpc::ServerBuilder (TLS, auth, ports).
NEOGRAPH_API std::unique_ptr<::grpc::Service> make_tool_service(
    std::unordered_map<std::string, ToolFn> tools);

/// Build + run a blocking gRPC ToolService on `address`
/// ("0.0.0.0:50091") serving `tools`, until the process is signalled.
/// Insecure credentials, with the standard health service.
NEOGRAPH_API void run_tool_server(
    const std::string& address,
    std::unordered_map<std::string, ToolFn> tools);
/// The same, until `options.stop` is requested (see ServeOptions).
NEOGRAPH_API void run_tool_server(
    const std::string& address,
    std::unordered_map<std::string, ToolFn> tools,
    const ServeOptions& options);

/// A neograph::Tool whose execute() round-trips to a remote
/// ToolService.CallTool over gRPC. Drop it into an Agent's tool list
/// like any local Tool — the agent never knows the work runs in
/// another process.
///
/// The simple ToolService proto has no "list tools" RPC, so the
/// definition (name / description / parameter schema the LLM sees) is
/// supplied at construction. A non-empty ToolCallResponse.error from
/// the server, or a non-OK status, is rethrown as std::runtime_error naming
/// the remote code (an ordinary tool failure; a remote run control code is
/// not mapped back to a local control signal), and DEADLINE_EXCEEDED as a
/// timed-out call.
///
/// pimpl so the class itself exposes no generated or protobuf type (same
/// posture as GrpcCheckpointStore).
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

    /// Every call's deadline: this long from the call (60 s by default), and
    /// no later than the run's deadline. The run's cancel token cancels the
    /// RPC in flight. Throws std::invalid_argument for a non-positive value.
    void set_call_timeout(std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
